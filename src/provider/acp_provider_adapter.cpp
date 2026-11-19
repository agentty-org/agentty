// agentty::provider — ExternalAcpBackend → Provider adapter.
// See acp_provider_adapter.hpp.

#include "agentty/provider/acp_provider_adapter.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <maya/runtime.hpp>

#include <acp/acp.hpp>          // acp::InitializeParams, SessionUpdate arms, match
#include <nlohmann/json.hpp>

#include "agentty/io/http.hpp"                       // http::CancelToken
#include "agentty/mcp/client.hpp"                    // configured MCP delegation
#include "agentty/provider/acp_agents.hpp"           // resolve_acp_agent
#include "agentty/provider/acp_backend.hpp"          // TurnSink, TurnResult
#include "agentty/provider/external_acp_backend.hpp" // ExternalAcpBackend, spawn_acp_agent
#include "agentty/provider/stream_epilogue.hpp"        // StreamResult
#include "agentty/runtime/msg.hpp"                   // Stream* Msgs
#include "agentty/tool/util/fs_helpers.hpp"          // workspace_root (for cwd)
#include "agentty/util/logx.hpp"

namespace agentty::provider {

// One spawned agent + the backend that drives it. Cached per agent id: the
// subprocess is long-lived, its ACP session reused across requests.
//
// Built once by acquire(), then published read-only. Teardown is the LAST
// holder's: release_acp_agents() only drops the cache's reference, so a turn
// still inside backend->prompt() on another thread keeps both alive until it
// returns. It used to reset them in place under that turn.
struct LiveAgent {
    std::unique_ptr<ExternalAcpBackend> backend;
    SpawnedAcpAgent                     agent;   // owns the subprocess and peer; dies first
    // The peer's dispatch points at the backend, so the peer goes first.
    ~LiveAgent() { agent.reset(); }
};

} // namespace agentty::provider
// Both members are set by acquire() before the map is published and never
// reassigned; what readers call on them is the backend's prompt() (one round
// at a time, guarded inside) and the peer (Sync). Joins on destroy (the peer).
MAYA_SYNC(agentty::provider::LiveAgent);
MAYA_JOINS_ON_DESTROY(agentty::provider::LiveAgent);
namespace agentty::provider {
namespace {

namespace a = acp;
using json = nlohmann::json;

// Copy-on-write: readers look up without a lock, a spawn publishes a new map.
using AgentMap = std::unordered_map<std::string, std::shared_ptr<LiveAgent>>;
maya::published<const AgentMap>& g_cache() {
    static maya::published<const AgentMap> c;
    return c;
}

std::shared_ptr<LiveAgent> cached(const std::string& agent_id) {
    auto map = g_cache().current();
    if (!map) return nullptr;
    auto it = map->find(agent_id);
    return it == map->end() ? nullptr : it->second;
}

// Questions put to the user, by ask id, until answered. The reply function
// is the backend's (it answers the agent's deferred request once); the entry
// is dropped on answer, and a turn that ends first answers it Cancelled on
// the backend side, so a late answer here just finds the entry gone.
using AskMap = std::unordered_map<std::string, TurnSink::PermissionReply>;
maya::guarded<AskMap>& g_asks() {
    static maya::guarded<AskMap> s;
    return s;
}

std::string tool_kind_name(a::ToolKind k) {
    switch (k) {
        case a::ToolKind::Read:       return "read";
        case a::ToolKind::Edit:       return "edit";
        case a::ToolKind::Delete:     return "delete";
        case a::ToolKind::Move:       return "move";
        case a::ToolKind::Search:     return "search";
        case a::ToolKind::Execute:    return "execute";
        case a::ToolKind::Think:      return "think";
        case a::ToolKind::Fetch:      return "fetch";
        case a::ToolKind::SwitchMode: return "switch_mode";
        case a::ToolKind::Other:      break;
    }
    return "other";
}
std::string option_kind_name(a::PermissionOptionKind k) {
    switch (k) {
        case a::PermissionOptionKind::AllowOnce:    return "allow_once";
        case a::PermissionOptionKind::AllowAlways:  return "allow_always";
        case a::PermissionOptionKind::RejectOnce:   return "reject_once";
        case a::PermissionOptionKind::RejectAlways: return "reject_always";
    }
    return "reject_once";
}

// Extract the plain text of a ContentBlock (text + text-resource arms only;
// other arms contribute nothing to a streamed delta).
std::string block_text(const a::ContentBlock& b) {
    std::string out;
    a::match(b,
        [&](const a::TextContent& t) { out += t.text; },
        [&](const a::ResourceContent& r) {
            a::match(r.resource,
                [&](const a::TextResource& tr) { out += tr.text; },
                [&](const a::BlobResource&)    {});
        },
        [&](const auto&) {});
    return out;
}

// Build the client's InitializeParams. We advertise the fs + terminal client
// capabilities so a spec-conformant agent knows it may call back into us
// (our make_handlers() wires those). acp-cpp doesn't enforce this agent-side,
// but real agents check it.
a::InitializeParams make_init() {
    a::InitializeParams init;
    init.clientCapabilities.fs.readTextFile  = true;
    init.clientCapabilities.fs.writeTextFile = true;
    init.clientCapabilities.terminal         = true;
    return init;
}

// Get (or lazily spawn) the live agent for `agent_id`. Returns nullptr and
// fills `err` on any launch failure.
std::shared_ptr<LiveAgent> acquire(const maya::unlocked& cap, const std::string& agent_id, std::string& err) {
    if (auto hit = cached(agent_id)) return hit;

    auto spec = resolve_acp_agent(agent_id);
    if (!spec) {
        err = "unknown ACP agent '" + agent_id +
              "' (no built-in default and no .agentty/acp-agents.json entry)";
        return nullptr;
    }

    ExternalAcpOptions opts;
    opts.agent_command = spec->command;
    opts.cwd           = spec->cwd.empty()
                       ? tools::util::workspace_root().string()
                       : spec->cwd;
    opts.reuse_session = true;
    for (const auto& server : mcp::configured_servers_for_delegation()) {
        auto key_values = [](const auto& values) {
            a::List<a::KeyValue> out;
            for (const auto& [name, value] : values)
                out.push_back(a::KeyValue{name, value});
            return out;
        };
        switch (server.transport) {
            case mcp::ServerLaunch::Transport::Stdio:
                opts.mcp_servers.push_back(a::McpServer{a::StdioMcpServer{
                    server.name, server.command, server.args, key_values(server.env)}});
                break;
            case mcp::ServerLaunch::Transport::Http:
                opts.mcp_servers.push_back(a::McpServer{a::HttpMcpServer{
                    server.name, server.url, key_values(server.headers)}});
                break;
            case mcp::ServerLaunch::Transport::Sse:
                opts.mcp_servers.push_back(a::McpServer{a::SseMcpServer{
                    server.name, server.url, key_values(server.headers)}});
                break;
        }
    }
    opts.delegate      = default_sandbox_delegate();

    auto live     = std::make_shared<LiveAgent>();
    live->backend = std::make_unique<ExternalAcpBackend>(std::move(opts));

    std::string spawn_err;
    live->agent = spawn_acp_agent(cap, spec->argv(), make_init(),
                                  live->backend->make_dispatch(), spawn_err);
    if (!live->agent.ok()) {
        err = "failed to launch ACP agent '" + agent_id + "': " + spawn_err;
        return nullptr;
    }
    live->backend->connect(*live->agent.connection);
    live->backend->set_auth_methods(live->agent.auth_methods);

    // Another thread may have raced us; keep the first winner.
    for (;;) {
        auto cur = g_cache().current();
        if (cur) {
            if (auto it = cur->find(agent_id); it != cur->end()) return it->second;
        }
        auto next = cur ? std::make_shared<AgentMap>(*cur) : std::make_shared<AgentMap>();
        next->emplace(agent_id, live);
        if (g_cache().publish_if(cur, std::move(next))) return live;
    }
}

} // namespace

namespace {
// The selectable values of a select option, grouped or not, as (value, label).
std::vector<std::pair<std::string, std::string>> option_values(const a::ConfigOption& o) {
    std::vector<std::pair<std::string, std::string>> out;
    a::match(o.options,
        [&](const a::CSO_Ungrouped& u) {
            for (const auto& v : u.items) out.emplace_back(v.value, v.name);
        },
        [&](const a::CSO_Grouped& g) {
            for (const auto& grp : g.items)
                for (const auto& v : grp.options) out.emplace_back(v.value, v.name);
        });
    return out;
}
} // namespace

StreamResult stream_external_acp(const maya::unlocked& cap, const std::string& agent_id, Request req, EventSink sink) {
    sink(StreamStarted{});

    std::string err;
    auto live = acquire(cap, agent_id, err);
    if (!live) {
        StreamError e; e.message = err;
        sink(std::move(e));
        return StreamResult::failed(err.empty() ? "ACP agent spawn failed" : err);
    }

    // The model the user picked for this thread, when the agent offers that
    // choice and isn't already on it: switch the agent first. One place for
    // every way a model gets picked (picker, ^Tab, a reopened thread), and
    // a no-op when the agent has no model option or the id isn't one of its
    // values (a thread from another provider).
    if (!req.model.empty()) {
        if (const auto opt = live->backend->model_option();
            opt && opt->currentValue != req.model) {
            bool offered = false;
            for (const auto& [value, label] : option_values(*opt))
                if (value == req.model) { offered = true; break; }
            if (offered)
                if (auto e = live->backend->set_config_option(cap, opt->id, req.model))
                    AGT_LOG(Wire, Warn, "acp.set_model", "agent={} model={} err={}",
                            agent_id, req.model, *e);
        }
    }

    // Translate ACP's snapshot-based, agent-owned tool lifecycle into visible
    // canonical cards. rawInput is replacement state, never an append delta.
    // These calls have already been executed by the delegated agent;
    // StreamObservedToolResult settles them before finalize_turn, so the host
    // scheduler never executes them a second time.
    std::unordered_set<std::string> open_tools;
    std::unordered_set<std::string> ended_tools;
    std::unordered_map<std::string, std::string> observed_outputs;

    auto tool_name = [](const a::ToolCall& call) {
        // Claude's ACP adapter carries the executable name in metadata while
        // title is a human label ("Edit file …"). Other agents commonly use
        // title directly, so retain that as the protocol-neutral fallback.
        try {
            if (call.meta.contains("claudeCode")
                && call.meta["claudeCode"].contains("toolName")) {
                auto name = call.meta["claudeCode"]["toolName"].get<std::string>();
                if (!name.empty()) return name;
            }
        } catch (...) { /* malformed optional metadata: use title */ }
        return call.title.empty() ? call.toolCallId.value : call.title;
    };

    auto finish_tool = [&sink, &open_tools, &ended_tools, &observed_outputs](
                           const std::string& id, bool failed = false) {
        if (!open_tools.contains(id) || !ended_tools.insert(id).second) return;
        sink(StreamToolUseEnd{ToolCallId{id}});
        std::string output;
        if (auto it = observed_outputs.find(id); it != observed_outputs.end())
            output = std::move(it->second);
        sink(StreamObservedToolResult{ToolCallId{id}, failed, std::move(output)});
        open_tools.erase(id);
    };

    TurnSink tsink;
    tsink.update = [&sink, &open_tools, &ended_tools, &observed_outputs,
                    &tool_name, &finish_tool](a::SessionUpdate su) {
        a::match(su,
            [&](const a::SU_AgentMessageChunk& c) {
                std::string t = block_text(c.content);
                if (!t.empty()) sink(StreamTextDelta{std::move(t)});
            },
            [&](const a::SU_AgentThoughtChunk& c) {
                std::string t = block_text(c.content);
                if (!t.empty()) sink(StreamThinkingDelta{std::move(t), {}});
            },
            [&](const a::SU_ToolCall& tc) {
                const auto& call = tc.toolCall;
                const auto& id = call.toolCallId.value;
                if (!open_tools.contains(id) && !ended_tools.contains(id)) {
                    sink(StreamToolUseStart{ToolCallId{id}, ToolName{tool_name(call)}});
                    open_tools.insert(id);
                }
                if (call.rawInput.has_value() && !call.rawInput.value().is_null())
                    sink(StreamToolUseSnapshot{ToolCallId{id}, call.rawInput.value().dump()});
                if (call.rawOutput.has_value() && !call.rawOutput.value().is_null())
                    observed_outputs[id] = call.rawOutput.value().dump();
                if (call.status == a::ToolCallStatus::Completed
                    || call.status == a::ToolCallStatus::Failed)
                    finish_tool(id, call.status == a::ToolCallStatus::Failed);
            },
            [&](const a::SU_ToolCallUpdate& tc) {
                const auto& update = tc.update;
                const auto& id = update.toolCallId.value;
                // A conforming ACP stream announces the call first. Be
                // defensive for adapters that race a refinement ahead of it:
                // open a stable fallback card rather than dropping the update.
                if (!open_tools.contains(id) && !ended_tools.contains(id)) {
                    const std::string name = update.title.has_value()
                        ? update.title.value() : id;
                    sink(StreamToolUseStart{ToolCallId{id}, ToolName{name}});
                    open_tools.insert(id);
                }
                if (update.rawInput.has_value() && !update.rawInput.value().is_null())
                    sink(StreamToolUseSnapshot{ToolCallId{id}, update.rawInput.value().dump()});
                if (update.rawOutput.has_value() && !update.rawOutput.value().is_null())
                    observed_outputs[id] = update.rawOutput.value().dump();
                if (update.status.has_value()
                    && (update.status.value() == a::ToolCallStatus::Completed
                        || update.status.value() == a::ToolCallStatus::Failed))
                    finish_tool(id,
                        update.status.value() == a::ToolCallStatus::Failed);
            },
            [&](const a::SU_Usage& u) {
                StreamUsage su2;
                // ACP reports a single aggregate `used`; map it onto the input
                // token field so the context-window meter advances. `size` is
                // the agent's advertised window if it sent one.
                su2.input_tokens = static_cast<int>(u.used < 0 ? 0 : u.used);
                sink(std::move(su2));
            },
            [&](const auto&) { /* plan / commands / mode / info: not streamed */ });
    };

    auto cancel = std::make_shared<http::CancelToken>();
    req.cancel  = cancel;   // the runtime trips this on Esc (see below)

    // Someone is watching: the agent's permission questions go to them. Each
    // becomes an AcpPermissionAsk on the sink; the user's answer comes back
    // through answer_acp_permission() with the same ask id.
    std::vector<std::string> my_asks;   // this turn's, dropped when it ends
    // This turn's identity for its ask ids: the cancel token is a fresh
    // allocation per turn and lives until the turn returns.
    const std::string turn_tag = std::to_string(reinterpret_cast<std::uintptr_t>(cancel.get()));
    if (req.interactive_asks) {
        tsink.ask = [&sink, &my_asks, turn_tag](const maya::unlocked& c, const a::RequestPermissionParams& p,
                                      TurnSink::PermissionReply reply) -> bool {
            AcpPermissionAsk ask;
            // Unique while open: the agent's call id is, and this turn's own
            // count separates two asks about the same call.
            ask.ask_id  = "acp-ask-" + turn_tag + "-" + p.toolCall.toolCallId.value + "-"
                        + std::to_string(my_asks.size());
            ask.tool_id = ToolCallId{p.toolCall.toolCallId.value};
            ask.title   = p.toolCall.title ? *p.toolCall.title : std::string{};
            ask.kind    = p.toolCall.kind ? tool_kind_name(*p.toolCall.kind) : std::string{"other"};
            for (const auto& o : p.options)
                ask.options.push_back({o.optionId, o.name, option_kind_name(o.kind)});
            if (ask.options.empty()) return false;   // nothing to choose: policy decides
            g_asks().with(c, [](AskMap& m, std::string id, TurnSink::PermissionReply r) {
                m[std::move(id)] = std::move(r);
            }, ask.ask_id, std::move(reply));
            my_asks.push_back(ask.ask_id);
            sink(std::move(ask));
            return true;
        };
    }
    struct AskCleanup {
        const maya::unlocked& cap;
        std::vector<std::string>& ids;
        ~AskCleanup() {
            if (ids.empty()) return;
            g_asks().with(cap, [](AskMap& m, std::vector<std::string> gone) {
                for (const auto& id : gone) m.erase(id);
            }, ids);
        }
    } ask_cleanup{cap, my_asks};

    // ACP's per-round TurnResult (acp_backend.hpp) is the ADJACENT layer's
    // outcome; fold it onto the shared provider StreamResult so the ACP arm
    // reports through the same value type as the native transports.
    TurnResult res = live->backend->prompt(cap, req, tsink, cancel);

    // Some adapters omit a final status update on cancellation/early turn
    // settlement. Close each remaining card exactly once using its latest
    // snapshot so no Pending call is stranded forever.
    std::vector<std::string> still_open(open_tools.begin(), open_tools.end());
    for (const auto& id : still_open) finish_tool(id);

    if (!res.ok()) {
        if (res.error && res.error->user_cancel) {
            // A user cancel is a clean stop, not an error surface.
            sink(StreamFinished{StopReason::EndTurn});
            StreamResult sr;
            sr.end  = StreamEnd::UserCancelled;
            sr.stop = StopReason::EndTurn;
            sr.error = std::string{"cancelled"};
            return sr;
        }
        StreamError e;
        e.message = res.error ? res.error->message : "ACP agent error";
        if (res.error) e.retry_after = res.error->retry_after;
        StreamResult sr;
        sr.end         = StreamEnd::TransportError;
        sr.stop        = res.stop;
        sr.error       = e.message;
        sr.retry_after = e.retry_after;
        sink(std::move(e));
        return sr;
    }
    sink(StreamFinished{res.stop});
    StreamResult sr;
    sr.end  = StreamEnd::CleanClose;
    sr.stop = res.stop;
    return sr;
}

void release_acp_agents(const maya::unlocked&) noexcept {
    // Drop the cache. Each agent is torn down by whoever holds it last (here,
    // unless a turn is still running), bounded even for a wedged agent.
    auto drained = g_cache().take();
    (void)drained;
}

void answer_acp_permission(const maya::unlocked& cap, const std::string& ask_id,
                           const std::string& option_id) {
    TurnSink::PermissionReply reply = g_asks().with(cap, [](AskMap& m, std::string id) {
        TurnSink::PermissionReply r;
        if (auto it = m.find(id); it != m.end()) {
            r = std::move(it->second);
            m.erase(it);
        }
        return r;
    }, ask_id);
    if (!reply) return;   // already settled (the turn ended)
    if (option_id.empty()) {
        reply(cap, a::PO_Cancelled{});
        return;
    }
    a::PO_Selected sel;
    sel.optionId = option_id;
    reply(cap, sel);
}

std::vector<ModelInfo> list_acp_agent_models(const maya::unlocked& cap,
                                             const std::string& agent_id,
                                             std::string& current,
                                             std::string& error) {
    current.clear();
    error.clear();
    auto live = acquire(cap, agent_id, error);
    if (!live) return {};
    // Options arrive with session/new. Open the session now (the first turn
    // reuses it), so the picker lists models before anything is sent.
    if (live->backend->config_options().empty()) {
        std::optional<TurnError> err;
        (void)live->backend->open_session(cap, err);
        if (err) { error = err->message; return {}; }
    }
    const auto opt = live->backend->model_option();
    if (!opt) return {};
    current = opt->currentValue;
    std::vector<ModelInfo> out;
    for (const auto& [value, label] : option_values(*opt)) {
        ModelInfo mi;
        mi.id           = ModelId{value};
        mi.display_name = label.empty() ? value : label;
        mi.provider     = agent_id;
        out.push_back(std::move(mi));
    }
    return out;
}

std::optional<std::string> set_acp_agent_model(const maya::unlocked& cap,
                                               const std::string& agent_id,
                                               const std::string& model) {
    std::string err;
    auto live = acquire(cap, agent_id, err);
    if (!live) return err.empty() ? std::string{"agent not running"} : err;
    const auto opt = live->backend->model_option();
    return live->backend->set_config_option(cap, opt ? opt->id : std::string{"model"}, model);
}

} // namespace agentty::provider
