#pragma once

// ── agentty::provider — ExternalAcpBackend → Provider adapter ────────────────
//
// Presents an external ACP agent subprocess as a plain `stream(Request,
// EventSink)` Provider, so the runtime's ONE dispatch seam (main.cpp
// `stream_fn`) can route to it with a single branch — exactly like the native
// Anthropic / ChatGPT providers. No new `Kind` switch fans out across the
// codebase; the agent-specific ACP quirks stay inside ExternalAcpBackend.
//
// Responsibilities:
//   • Lazily SPAWN (and cache) the agent subprocess keyed by its spec id, using
//     the launch spec from acp_agents.hpp. One long-lived subprocess per agent
//     serves every request — its ACP session is reused across rounds.
//   • Translate one `prompt()` round: acp::SessionUpdate → agentty Msg
//     (StreamTextDelta / StreamThinkingDelta / StreamToolUse* / StreamUsage),
//     then settle with StreamFinished or StreamError from the TurnResult.
//   • Cancellation: the Request carries no cancel token, so this owns one and
//     wires it through so Esc mid-stream reaches the agent.
//
// Thread-safety: the spawn cache is guarded; stream() may be called from the
// provider worker thread. The cached subprocess is torn down at process exit
// (or when release_acp_agents() is called).

#include <optional>
#include <string>
#include <vector>

#include <maya/runtime.hpp>

#include "agentty/domain/catalog.hpp"       // ModelInfo
#include "agentty/provider/provider.hpp"   // Request, EventSink, StreamResult

namespace agentty::provider {

// Drive the external ACP agent named by `agent_id` (a registry spec id such as
// "claude-agent-acp" / "codex-acp", or a config-defined id) for one request,
// streaming normalized Msgs into `sink`. Spawns + caches the subprocess on
// first use. On spawn/launch failure emits StreamStarted + StreamError so the
// UI shows a clean error instead of a silent hang. Returns a StreamResult
// naming how the round ended (mapped from the ACP backend's own TurnResult) so
// the ACP arm reports its outcome through the same value as the native ones.
StreamResult stream_external_acp(const maya::unlocked& cap,
                                 const std::string&    agent_id,
                                 Request               req,
                                 EventSink             sink);

// Tear down every cached agent subprocess (idempotent). Called at shutdown; the
// per-agent teardown uses ExternalAcpBackend's hardened watchdog so a wedged
// agent can't hang the exit path. It waits on subprocesses: `unlocked`.
void release_acp_agents(const maya::unlocked& cap) noexcept;

// Answer an AcpPermissionAsk. `option_id` is one of the ask's options, or
// empty to cancel (the agent treats that as "don't"). A stale or unknown id
// is a no-op: the round already ended and answered it Cancelled.
void answer_acp_permission(const maya::unlocked& cap, const std::string& ask_id,
                           const std::string& option_id);

// The agent's models, from its "model" config option, as picker rows
// (id = the option value, display_name = the agent's label, provider = the
// agent id). Starts the agent and opens a session if needed, so this is a
// worker-thread call: `unlocked`. Empty when the agent offers no choice;
// `current` gets the agent's current pick, `error` a launch/sign-in failure.
[[nodiscard]] std::vector<ModelInfo> list_acp_agent_models(const maya::unlocked& cap,
                                                          const std::string& agent_id,
                                                          std::string& current,
                                                          std::string& error);

// Switch the agent's model (the "model" option) for this and later turns.
// Remembered if no session is open yet. Returns an error, or nullopt.
[[nodiscard]] std::optional<std::string> set_acp_agent_model(const maya::unlocked& cap,
                                                            const std::string& agent_id,
                                                            const std::string& model);

} // namespace agentty::provider
