#pragma once

// ── ExternalAcpBackend — an out-of-process ACP agent as an AcpBackend ─────────
//
// Implements the `AcpBackend` contract (provider/acp_backend.hpp) by driving a
// *remote* ACP agent — `claude-agent-acp`, `codex-acp`, or any conforming
// `agentclientprotocol` agent — over an rpc::Peer.
// agentty is the CLIENT (editor); the subprocess is the AGENT (LLM side).
//
// This is the INBOUND direction of the same `acp::SessionUpdate` vocabulary
// agentty already emits OUTBOUND from src/acp/server.cpp. One `prompt()` call:
//
//   1. lazily opens a session (`session/new` once, then reused across rounds);
//   2. sends the tail user message as `session/prompt`;
//   3. forwards every inbound `session/update` — agent message / thought /
//      tool_call / tool_call_update / usage — straight to the `TurnSink`;
//   4. settles the round with a `TurnResult` whose `StopReason` is decoded from
//      the agent's turn-level ACP `StopReason` (end_turn / max_tokens /
//      refusal / cancelled), applying the terminal-event discipline exactly
//      once — no spurious "cancelled" on a clean turn.
//
// It drives an already-connected peer rather than spawning one: the
// subprocess and `initialize` are a separate lifecycle. That keeps it
// testable against an in-memory channel wired to a fake agent, and usable
// over any byte channel. `spawn_acp_agent()` (see the .cpp) produces a
// connected handle for production.
//
// The Claude reference adapter (claude-agent-acp/src/acp-agent.ts) is the model
// this mirrors: a thin protocol translator that owns exactly one quirk surface
// (its `SDKMessage` → `session/update` mapping) and nothing structural.
//
// See docs/internal-acp-backends.md — this header is §6, step 5 of that plan.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <maya/runtime.hpp>

#include <acp/methods.hpp>        // acp::StopReason, PromptParams, NewSessionParams
#include <acp/protocol.hpp>       // the method table, jsonrpc::Router
#include <acp/updates.hpp>        // acp::SessionUpdate, SessionUpdateMsg

#include "agentty/io/http.hpp"                 // http::CancelTokenPtr
#include "agentty/provider/acp_backend.hpp"    // AcpBackend, TurnSink, TurnResult
#include "agentty/provider/provider.hpp"       // provider::Request
#include "agentty/rpc/peer.hpp"                // rpc::Peer, rpc::Dispatch
#include "agentty/util/callback.hpp"
#include "agentty/util/sendable.hpp"           // util::Callback

namespace agentty::provider {

// ── Sandbox delegate ─────────────────────────────────────────────────────────
// An external agent may ask the CLIENT (us) to read/write files and run
// terminals on its behalf (ACP `fs/*` and `terminal/*`). agentty already owns a
// sandbox + permission gate for exactly this; rather than reach into it from
// here, the backend takes a small delegate so the wiring stays in one place and
// tests can supply an in-memory stub. All hooks are optional — an unset hook
// declines the capability (the agent is told the method is unsupported), which
// is the safe default: a mis-declared agent can't silently escape the sandbox.
struct AcpClientDelegate {
    // fs/read_text_file → return file contents (optionally a line window).
    std::function<std::optional<std::string>(
        const std::string& path, std::optional<int> line, std::optional<int> limit)>
        read_text_file;

    // fs/write_text_file → persist; return false to refuse (permission denied).
    std::function<bool(const std::string& path, const std::string& content)>
        write_text_file;

    // request_permission → true = allow the tool the agent is about to run.
    // Unset ⇒ allow (the agent's own gate governs); return false to deny.
    std::function<bool(const acp::RequestPermissionParams&)> request_permission;

    // terminal/create → run a command and return its result. The ACP terminal
    // protocol is a stateful lifecycle (create → output/wait_for_exit →
    // kill/release), but a synchronous executor satisfies it by running the
    // command to completion inside create() and caching the result for the
    // follow-up calls — which is exactly what run_terminal does here. Return
    // value: (merged stdout+stderr, exit code). Unset ⇒ terminals declined.
    struct TerminalResult {
        std::string output;
        int         exit_code = 0;
        bool        truncated = false;
    };
    std::function<TerminalResult(const std::string& command,
                                 const std::vector<std::string>& args,
                                 const std::optional<std::string>& cwd)>
        run_terminal;
};

// A delegate wired to agentty's REAL boundary: fs reads go through the read
// gate (workspace + skill read-allowlist, symlink-escape blocked), writes
// through the write gate (workspace only), and terminal/permission through the
// OS-native sandbox. This is the delegate production callers want; tests supply
// their own in-memory stub. Defined in the .cpp so the header stays free of the
// tool/sandbox headers.
[[nodiscard]] AcpClientDelegate default_sandbox_delegate();

// A permission question handed to the round's asker, answered later. At
// most once: by the user, or with Cancelled when the round ends first (Esc,
// the agent finishing) -- ACP requires a client that cancels to answer every
// outstanding request_permission. `done` is the once-latch, so the deferred
// reply is written by exactly one thread.
struct AcpPendingAsk {
    explicit AcpPendingAsk(rpc::Peer::Deferred<acp::to_client::RequestPermission> d)
        : reply(std::move(d)) {}
    // The reply, claimed by whoever answers first (the user, or the round
    // ending): taking it out under the lock is the once-latch, so it is
    // written by exactly one thread -- and written AFTER the lock is gone.
    using Reply = rpc::Peer::Deferred<acp::to_client::RequestPermission>;
    maya::guarded<std::optional<Reply>> reply_slot;
    rpc::Peer::Deferred<acp::to_client::RequestPermission> reply;
    void arm(const maya::unlocked& cap) {
        reply_slot.with(cap, [](std::optional<Reply>& s, Reply r) { s.emplace(std::move(r)); },
                        std::move(reply));
    }
    void answer(const maya::unlocked& cap, acp::RequestPermissionOutcome o) {
        auto r = reply_slot.with(cap, [](std::optional<Reply>& s) { return std::exchange(s, std::nullopt); });
        if (!r) return;
        acp::RequestPermissionResult res;
        res.outcome = std::move(o);
        r->ok(cap, res);
    }
    // Answered elsewhere (the router's synchronous policy): drop the claim.
    void disarm(const maya::unlocked& cap) {
        (void)reply_slot.with(cap, [](std::optional<Reply>& s) { return std::exchange(s, std::nullopt); });
    }
};

// ── Options ─────────────────────────────────────────────────────────────────────────────
struct ExternalAcpOptions {
    // Working directory handed to `session/new` (the agent's project root).
    std::string cwd;

    // If true, one session is opened on the first `prompt()` and reused for
    // every subsequent round (the agent keeps its own transcript — this is how
    // claude-agent-acp / codex-acp work, and it's what lets the agent replay
    // its own reasoning between tool rounds without us re-sending it). If
    // false, a fresh session is opened per round (stateless; for agents that
    // don't persist, or for strict per-round isolation in tests).
    bool reuse_session = true;

    // MCP servers delegated to the external agent through session/new. The
    // delegated agent owns execution; agentty only observes its tool updates.
    std::vector<acp::McpServer> mcp_servers;

    // The command that launched the agent ("devin"), for a sign-in hint
    // that names what to run. Empty in tests.
    std::string agent_command;

    // Optional client-side capabilities the agent may call back into.
    AcpClientDelegate delegate;
};

class ExternalAcpBackend final : public AcpBackend {
public:
    // Construct the backend BEFORE its peer exists: the peer's dispatch comes
    // from make_dispatch() below, which closes over this backend. Call
    // connect() once the peer is started and initialized. prompt() before
    // connect() fails cleanly.
    explicit ExternalAcpBackend(ExternalAcpOptions opts);

    // Bind the (already-initialized) peer. It MUST outlive this backend's
    // use of it; ownership stays with the caller (usually SpawnedAcpAgent).
    void connect(rpc::Peer& peer) noexcept;

    // Non-copyable, non-movable — the dispatch closes over `this`.
    ExternalAcpBackend(const ExternalAcpBackend&)            = delete;
    ExternalAcpBackend& operator=(const ExternalAcpBackend&) = delete;

    ~ExternalAcpBackend() override = default;

    // AcpBackend contract. Drives one round; streams SessionUpdates to `sink`;
    // returns the round's TurnResult. Thread-compatible: one round at a time.
    // ExternalAcpBackend's turn: send session/prompt and stream session/update
    // into `sink` until the PromptResult. Exactly one terminal event.
    // Blocks on the agent's reply: `unlocked`.
    TurnResult prompt(const maya::unlocked&       cap,
                      const Request&              req,
                      const TurnSink&             sink,
                      const http::CancelTokenPtr& cancel) override;

    // Test/inspection hook: the session id opened by the first prompt (empty
    // until then, or in per-round mode).
    [[nodiscard]] std::string session_id() const;

    // What `initialize` returned: how the agent can be signed in to. The
    // spawn path hands it over; a backend that never got one has none.
    void set_auth_methods(acp::List<acp::AuthMethod> m);
    [[nodiscard]] acp::List<acp::AuthMethod> auth_methods() const;

    // The agent's session config options (model, mode, effort...) as the
    // agent last reported them: from session/new, a config_option_update,
    // or a set_config_option reply. Empty before the first session.
    [[nodiscard]] acp::List<acp::ConfigOption> config_options() const;

    // Ask the agent to change one option ("model" = SWE-2, say). Sent at
    // once if a session is open; otherwise remembered and applied right
    // after session/new, so a pick made before the first turn still holds.
    // Waits for the agent: `unlocked`.
    [[nodiscard]] std::optional<std::string> set_config_option(
        const maya::unlocked& cap, const std::string& config_id, const std::string& value);

    // The option the agent uses to pick its model: one whose category or id
    // is "model". nullopt when the agent exposes none.
    [[nodiscard]] std::optional<acp::ConfigOption> model_option() const;

    // Open (or reuse) the session without prompting, so the agent's options
    // and sign-in state are known before the first turn. Same path prompt()
    // takes; fills `err` on failure. Waits for the agent: `unlocked`.
    std::optional<acp::SessionId> open_session(const maya::unlocked& cap,
                                               std::optional<TurnError>& err) {
        return ensure_session_(cap, Request{}, err);
    }

    // How the agent's calls reach us (session/update → active sink; fs/*,
    // terminal/*, request_permission → delegate). The spawn factory builds
    // the peer with it. It closes over `this`, so this backend MUST outlive
    // the peer.
    [[nodiscard]] rpc::Dispatch make_dispatch();

private:
    // What a router handler gets: the backend, and the reader thread's
    // capability for the call it is answering (it lives on the reader's
    // stack for exactly that call).
    struct Ctx {
        ExternalAcpBackend&   self;
        const maya::unlocked& cap;
    };
    using Router = ::jsonrpc::Router<Ctx>;
    Router make_router() const;

    std::optional<acp::SessionId> ensure_session_(const maya::unlocked& cap,
                                                  const Request& req,
                                                  std::optional<TurnError>& err);

    rpc::Peer*         peer_ = nullptr;   // set once by connect()
    ExternalAcpOptions opts_;
    Router             router_;

    // The reused session id: set once, read every round. A snapshot.
    maya::snapshot<std::optional<std::string>> session_;

    // Sign-in methods from initialize, the agent's current config options,
    // and option picks waiting for a session to apply to.
    maya::snapshot<acp::List<acp::AuthMethod>>  auth_methods_;
    maya::snapshot<acp::List<acp::ConfigOption>> config_;
    maya::guarded<std::vector<std::pair<std::string, std::string>>> pending_config_;
    // Sign in (authenticate) with the agent's first non-terminal method, once
    // per backend; true when it was tried and worked.
    bool try_authenticate_(const maya::unlocked& cap, std::string& why);
    void apply_pending_config_(const maya::unlocked& cap, const acp::SessionId& sid);

    // The round in flight. session/update arrives on the peer's reader,
    // which joins `inflight` before calling `sink`. prompt() takes the
    // round on exit and waits for in-flight calls, so the sink (which points at
    // prompt's stack) is never called after it returns. No round: updates drop.
    struct Round {
        const util::Callback<void(acp::SessionUpdate)> sink;   // set at build, then only called
        // The round's interactive permission asker (TurnSink::ask), or empty.
        const util::Callback<bool(const maya::unlocked&, const acp::RequestPermissionParams&,
                                  TurnSink::PermissionReply)> ask;
        maya::stop_group                               inflight;
    };
    maya::published<Round> round_;

    // Questions out to the user this round. Copy-on-write, no lock: an
    // answer writes to the peer, so it must never run under one.
    using AskList = std::vector<std::shared_ptr<AcpPendingAsk>>;
    maya::published<const AskList> asks_;
    void cancel_asks_(const maya::unlocked& cap);

    // terminal/create runs the command synchronously (via the delegate) and
    // keeps its result here under a fresh id until terminal/release.
    struct TerminalState { std::string output; int exit_code = 0; bool truncated = false; };
    struct Terminals {
        std::unordered_map<std::string, TerminalState> by_id;
        std::uint64_t                                  next_id = 1;
    };
    maya::guarded<Terminals> terminals_;
};

// ── Turn-level StopReason mapping (pure, testable) ───────────────────────────
// ACP's turn-level StopReason → agentty's round-level StopReason. `cancelled`
// and `refusal` are NOT stop reasons in agentty's ladder — the caller turns
// them into a TurnError instead — so this maps only the "clean finish" arms;
// the caller special-cases cancelled/refusal before calling.
[[nodiscard]] StopReason map_acp_stop_reason(acp::StopReason r) noexcept;

// ── Subprocess factory ───────────────────────────────────────────────────────
// A connected, initialized agent and the process behind it. Destroy it to
// tear the agent down (closes pipes, waits/kills the child).
struct SpawnedAcpAgent {
    std::unique_ptr<rpc::Peer> connection;
    // Opaque owner of the subprocess. Reset after the peer: the peer's
    // reader reads the child's stdout stream.
    std::shared_ptr<void> process;
    // From initialize: how to sign in, and the agent's own display name.
    acp::List<acp::AuthMethod> auth_methods;
    std::string                agent_title;

    SpawnedAcpAgent() = default;
    SpawnedAcpAgent(SpawnedAcpAgent&&) noexcept = default;
    SpawnedAcpAgent& operator=(SpawnedAcpAgent&&) noexcept = default;
    SpawnedAcpAgent(const SpawnedAcpAgent&) = delete;
    SpawnedAcpAgent& operator=(const SpawnedAcpAgent&) = delete;
    ~SpawnedAcpAgent() { reset(); }

    void reset() noexcept;

    [[nodiscard]] bool ok() const noexcept { return connection != nullptr; }
};

// Spawn `argv[0]` (e.g. "claude-agent-acp" or "codex-acp") with the remaining
// args as an ACP agent subprocess (util::ChildProcess: fork/exec/pipe on
// POSIX, CreateProcess on Windows), run an rpc::Peer over its stdio with
// `dispatch`, run `initialize`, and return the connected handle. On failure
// returns a SpawnedAcpAgent with a null connection and `err` filled.
//
// PRODUCTION FLOW (resolves the backend/dispatch/peer cycle):
//     ExternalAcpBackend backend{opts};
//     auto agent = spawn_acp_agent(argv, init, backend.make_dispatch(), err);
//     if (!agent.ok()) { /* handle err */ }
//     backend.connect(*agent.connection);
//     // ... backend.prompt(...) ...  (keep `agent` alive for the backend's life)
// Spawn `argv` as an ACP agent, run an rpc::Peer over its stdio, and complete
// the `initialize` handshake. On failure returns !ok() and sets `err`.
// Waits for the handshake: `unlocked`.
[[nodiscard]] SpawnedAcpAgent spawn_acp_agent(const maya::unlocked&           cap,
                                              const std::vector<std::string>& argv,
                                              const acp::InitializeParams&    init,
                                              rpc::Dispatch                   dispatch,
                                              std::string&                    err);

} // namespace agentty::provider

// The deferred reply sits in a guarded slot taken out under the lock; the
// struct is otherwise unchanged after arm(). Many threads may hold one.
MAYA_SYNC(agentty::provider::AcpPendingAsk);
