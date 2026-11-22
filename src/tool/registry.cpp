#include "agentty/tool/registry.hpp"

#include "agentty/mcp/client.hpp"
#include "agentty/tool/mcp_tools_bridge.hpp"
#include "agentty/tool/question_tool.hpp"
#include "agentty/tool/web_search_policy.hpp"
#include "agentty/tool/tool_loading.hpp"
#include "agentty/tool/tool_plan.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <format>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace agentty::tools {

std::string_view to_string(ErrorKind k) noexcept {
    switch (k) {
        case ErrorKind::InvalidArgs:    return "invalid args";
        case ErrorKind::NotFound:       return "not found";
        case ErrorKind::NotAFile:       return "not a file";
        case ErrorKind::NotADirectory:  return "not a directory";
        case ErrorKind::TooLarge:       return "too large";
        case ErrorKind::Binary:         return "binary";
        case ErrorKind::Ambiguous:      return "ambiguous";
        case ErrorKind::NoMatch:        return "no match";
        case ErrorKind::InvalidRegex:   return "invalid regex";
        case ErrorKind::Network:        return "network";
        case ErrorKind::Spawn:          return "spawn failed";
        case ErrorKind::Subprocess:     return "subprocess failed";
        case ErrorKind::Io:             return "io";
        case ErrorKind::OutOfWorkspace: return "out of workspace";
        case ErrorKind::Denied:         return "denied";
        case ErrorKind::Unknown:        return "unknown";
    }
    return "unknown";
}

std::string ToolError::render() const {
    return std::format("[{}] {}", to_string(kind), detail);
}

std::string_view to_string(Effect e) noexcept {
    switch (e) {
        case Effect::ReadFs:  return "ReadFs";
        case Effect::WriteFs: return "WriteFs";
        case Effect::Net:     return "Net";
        case Effect::Exec:    return "Exec";
    }
    return "?";
}

std::string to_string(EffectSet e) {
    if (e.empty()) return "Pure";
    std::string out;
    auto add = [&](Effect bit) {
        if (!e.has(bit)) return;
        if (!out.empty()) out += ", ";
        out += to_string(bit);
    };
    add(Effect::Exec);
    add(Effect::WriteFs);
    add(Effect::Net);
    add(Effect::ReadFs);
    return out;
}

namespace {

// Assemble every tool. Order matters: the protocol treats the set as
// unordered but the model has a strong recall bias toward earlier-listed
// tools. Putting `edit` ahead of `write` is the cheapest single nudge to
// stop the model from rewriting whole files when a targeted substitution
// would do — and edit's tiny input_json_delta bodies sidestep the long
// mid-stream pause Anthropic's edge applies to multi-KB tool_use content.
// Assemble the local tool set. The implementations live in mcp-cpp's
// batteries-included toolset (mcp::tools::make_provider): build_mcp_tool_defs()
// re-wraps each advertised tool as a ToolDef whose execute() dispatches into
// the provider and reads the typed effects + FileChange off each Result back
// into ToolOutput. The host-coupled SHELLS (remember/forget/wipe/todo/skill/
// search_docs/task) are backed by agentty adapters injected via HostServices.
// mcp-cpp is the SOLE source of truth for tools — there is no native path.
std::vector<ToolDef> build_native_registry() {
    auto tools = build_mcp_tool_defs();
    // The one native tool mcp-cpp has no shell for: only the host has a user.
    tools.push_back(question_tool::def());
    return tools;
}

// Connect external MCP exactly once, on first catalog access. The returned
// ToolDefs are not made part of the immutable native baseline: every later
// generation is rebuilt from the MCP pool's current authoritative snapshot.
std::vector<ToolDef> connect_initial_mcp() {
    if (!mcp::mcp_config_present()) return {};
    static mcp::PoolHandle s_pool;
    return mcp::mcp_tools(s_pool);
}

} // namespace

const std::vector<ToolDef>& native_registry() {
    static const std::vector<ToolDef> r = build_native_registry();
    return r;
}

namespace {
// One build of the catalog. Immutable once published; readers hold the
// Catalog handle for as long as they use a ToolDef, so a rebuild never
// invalidates anything and the old build dies with its last reader.
struct Snapshot {
    Catalog tools;
    std::unordered_map<std::string, std::size_t> idx;   // name -> index in *tools
    unsigned long generation = 0;
    bool stale = false;   // marker published by invalidation; never served
};

enum class Phase { Idle, Connecting, Connected };
// Lock-free: the catalog is read from everywhere (views, lock bodies, tool
// workers), so the connect claim is one atomic and the first connect's
// result is published once. No capability on the catalog path.
std::atomic<Phase>& connect_phase() { static std::atomic<Phase> p{Phase::Idle}; return p; }
// The two slots are leaked: a tool worker abandoned at the shutdown grace
// may still read them while statics are torn down. What they point at is
// refcounted and dies with its last reader, so this is one allocation each,
// not a growing chain.
maya::published<const std::vector<ToolDef>>& initial_mcp() {
    static auto* s = new maya::published<const std::vector<ToolDef>>;
    return *s;
}
maya::published<const Snapshot>& catalog() {
    static auto* c = new maya::published<const Snapshot>;
    return *c;
}

std::shared_ptr<const Snapshot> make_snapshot(std::vector<ToolDef> tools, unsigned long generation) {
    // tool_search is always dispatchable (#98). Whether it rides the wire is
    // the planner's call; whether it RUNS must not depend on what was sent,
    // or a model that saw it last turn gets "unknown tool".
    if (std::none_of(tools.begin(), tools.end(), [](const ToolDef& t) {
            return t.name.value == plan::kSearchToolName; }))
        tools.push_back(tool_loading::search_tool());
    std::unordered_map<std::string, std::size_t> idx;
    idx.reserve(tools.size());
    for (std::size_t i = 0; i < tools.size(); ++i) idx.emplace(tools[i].name.value, i);
    return std::make_shared<const Snapshot>(
        Snapshot{Catalog::make(std::move(tools)), std::move(idx), generation, false});
}

// Served while another thread is mid-connect, so nobody waits on it.
std::shared_ptr<const Snapshot> native_only() {
    static const std::shared_ptr<const Snapshot> s = make_snapshot(native_registry(), 0);
    return s;
}

// Whether this call may be the one that connects external MCP (spawns every
// configured server, up to 15 s). find() is called from reducers on the UI
// thread (tool scheduling, the permission prompt), so it must never be the
// caller that connects: it froze the UI when it beat init's warm-up task to
// the first access. A find() before the connect sees the native tools, the
// same as any caller arriving mid-connect. Every other access may connect:
// on ACP and `agentty run` the first wire_tools() on a turn worker is
// what brings MCP up.
enum class Connect { May, Never };

std::shared_ptr<const Snapshot> refresh_wire_cache(Connect may = Connect::May) {
    // The first caller connects external MCP; anyone arriving meanwhile gets
    // the native tools right away (it may be the UI thread, and the connect
    // can take up to 15 s).
    enum class Claim { Mine, Busy, Done };
    const Claim claim = [may] {
        Phase expect = Phase::Idle;
        if (may == Connect::Never) {
            expect = connect_phase().load();
            return expect == Phase::Connected ? Claim::Done : Claim::Busy;
        }
        if (connect_phase().compare_exchange_strong(expect, Phase::Connecting))
            return Claim::Mine;
        return expect == Phase::Connected ? Claim::Done : Claim::Busy;
    }();
    if (claim == Claim::Busy) return native_only();
    if (claim == Claim::Mine) {
        // Back to Idle on ANY throw between the claim and Connected (the
        // connect or the publish), or no later call would ever connect.
        try {
            auto ext = connect_initial_mcp();
            initial_mcp().publish(
                std::make_shared<const std::vector<ToolDef>>(std::move(ext)));
        } catch (...) {
            connect_phase().store(Phase::Idle);
            throw;
        }
        connect_phase().store(Phase::Connected);
    }

    for (;;) {
        auto cur = catalog().current();
        const unsigned long generation = mcp::mcp_generation();
        if (cur && !cur->stale && cur->generation == generation) return cur;

        std::vector<ToolDef> external;
        if (generation != 0 && may == Connect::Never && cur && !cur->stale) {
            // Only the generation moved: a server sent list_changed, and the
            // rebuild would run tools/list on it. The UI thread doesn't wait
            // on that; it keeps this snapshot and the next worker access
            // rebuilds. (An explicit invalidation, a tool toggled off, still
            // rebuilds here: that reads cached lists, no network.)
            return cur;
        }
        if (generation == 0) {
            if (auto init = initial_mcp().current()) external = *init;
        } else {
            external = mcp::mcp_tools_live();
        }

        std::vector<ToolDef> tools;
        tools.reserve(native_registry().size() + external.size());
        tools.insert(tools.end(), native_registry().begin(), native_registry().end());

        // External names are namespaced, but still drop duplicates rather
        // than send ambiguous schemas to a provider.
        std::unordered_map<std::string, bool> names;
        names.reserve(tools.capacity());
        for (const auto& tool : tools) names.emplace(tool.name.value, true);
        for (auto& tool : external) {
            if (!names.emplace(tool.name.value, true).second) continue;
            tools.push_back(std::move(tool));
        }
        auto out = make_snapshot(std::move(tools), generation);
        if (catalog().publish_if(cur, out)) return out;
        // Someone else published (a rebuild or an invalidation); re-check.
    }
}

// Mark the current catalog stale so the next access rebuilds it.
void invalidate_catalog() {
    for (;;) {
        auto cur = catalog().current();
        if (!cur) return;
        auto marker = std::make_shared<const Snapshot>(
            Snapshot{cur->tools, cur->idx, cur->generation, /*stale=*/true});
        if (catalog().publish_if(cur, std::move(marker))) return;
    }
}
} // namespace

// Legacy tool-name aliases → canonical registry name. The exec tool was renamed
// bash → shell, but `bash` is so dominant in model training data (it is Claude
// Code's actual tool name — commonly emitted as "Bash", capital B) that a model
// reliably calls it by the old name on the FIRST turn, before it internalises
// our schema. Without this the call 404s ("unknown tool: Bash"), the model
// burns a wasted failed call, then retries as `shell`. Canonicalising here —
// the ONE lookup every dispatch path (agent loop, subagent, ACP, mcp-serve,
// permission check) funnels through — absorbs that misfire so the legacy name
// just works.
//
// The match is CASE-INSENSITIVE and whitespace-trimmed: models emit "bash",
// "Bash", and "BASH" interchangeably, and the exact-case check was the bug that
// let "Bash" slip through to a hard failure.
[[nodiscard]] static std::string_view canonical_tool_name(std::string_view name) noexcept {
    // Trim surrounding whitespace without allocating.
    auto b = name.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return name;
    auto e = name.find_last_not_of(" \t\r\n");
    std::string_view trimmed = name.substr(b, e - b + 1);
    auto ieq = [](std::string_view s, std::string_view lit) {
        if (s.size() != lit.size()) return false;
        for (std::size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != lit[i]) return false;
        }
        return true;
    };
    if (ieq(trimmed, "bash")) return "shell";
    // Renamed off the `mcp_` prefix Anthropic OAuth rejects; saved threads
    // and habit still call the old names.
    if (ieq(trimmed, "mcp_read_resource")) return "mcp__resources__read";
    if (ieq(trimmed, "mcp_get_prompt")) return "mcp__prompts__get";
    return name;
}

std::string unknown_tool_error(std::string_view name) {
    std::string msg = "unknown tool: " + std::string{name}
        + ". This tool is not part of agentty's toolset — if a proxy or "
          "gateway advertised it, call it through that channel, not here. "
          "Available tools:";
    for (const auto& td : *wire_tools()) {
        msg += ' ';
        msg += td.name.value;
        msg += ',';
    }
    if (msg.back() == ',') msg.pop_back();
    msg += '.';
    return msg;
}

Found find(std::string_view name) {
    const auto snapshot = refresh_wire_cache(Connect::Never);
    if (auto it = snapshot->idx.find(std::string{name}); it != snapshot->idx.end())
        return Found{snapshot->tools, &(*snapshot->tools)[it->second]};
    // Miss: retry once under the canonical name so a legacy alias resolves.
    if (auto canon = canonical_tool_name(name); canon != name)
        if (auto it = snapshot->idx.find(std::string{canon}); it != snapshot->idx.end())
            return Found{snapshot->tools, &(*snapshot->tools)[it->second]};
    return Found{};
}

Catalog wire_tools() { return refresh_wire_cache()->tools; }

bool withheld_by_policy(const ToolDef& tool, bool web_search_enabled) {
    return withheld_by_policy(tool, web_search_enabled, question_tool::enabled());
}

bool withheld_by_policy(const ToolDef& tool, bool web_search_enabled,
                        bool questions_enabled) {
    if (tool.origin != ToolOrigin::Native) return false;
    if (!web_search_enabled && tool.name.value == "web_search") return true;
    // No one to ask (agentty run, acp, mcp serve), or the user turned it off.
    if (!questions_enabled && tool.name.value == question_tool::kName) return true;
    return false;
}

bool withheld_by_policy(const ToolDef& tool) {
    return withheld_by_policy(
        tool, web_search_cfg::mode(web_search_policy::current()) != web_search_cfg::Mode::Off);
}

unsigned long mcp_generation() noexcept {
    return mcp::mcp_generation();
}

std::size_t reload_mcp_plugins() {
    // One reload at a time. Each plugin toggle fires this on its own thread
    // and a reconnect can take seconds, so a request that lands mid-reload
    // just sets `pending` and returns; the running reload loops once more to
    // pick up the newer config. Checking `pending` and clearing `running`
    // happen in one CAS, so no request is lost.
    // Both flags live in one atomic word, so each step is a single CAS and
    // no lock (or capability) is needed on this path.
    constexpr unsigned kRunning = 1u, kPending = 2u;
    static std::atomic<unsigned> state{0};

    unsigned cur = state.load();
    for (;;) {
        const unsigned want = (cur & kRunning) ? (cur | kPending) : kRunning;
        if (state.compare_exchange_weak(cur, want)) break;
    }
    if (cur & kRunning) return 0;   // someone else is reloading; they'll loop

    // A throw out of the reload must still clear `running`, or every later
    // reload sees one in progress and returns without doing anything.
    struct Clear {
        std::atomic<unsigned>& s;
        bool armed = true;
        ~Clear() { if (armed) s.store(0); }
    } clear{state};

    std::size_t n = 0;
    for (;;) {
        state.fetch_and(~kPending);
        n = mcp::mcp_reload();
        invalidate_catalog();
        unsigned s = state.load();
        bool again = false;
        for (;;) {
            again = (s & kPending) != 0;
            if (again) break;   // keep running; the next pass clears pending
            if (state.compare_exchange_weak(s, s & ~kRunning)) break;
        }
        if (!again) break;
    }
    clear.armed = false;
    return n;
}

void invalidate_mcp_catalog(Io) {
    // No re-spawn: bump the pool generation and mark the catalog stale.
    // project_tools re-reads tools.exclude on the rebuild, so an
    // enable/disable toggle lands on the next access with no server churn.
    mcp::mcp_bump_generation();
    invalidate_catalog();
}

} // namespace agentty::tools
