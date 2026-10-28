#pragma once
// agentty::tools::plan — which tools ride a request, and how the model finds
// the rest (#98).
//
// Three states per enabled tool:
//   Loaded     its schema is on the wire every turn.
//   On demand  not on the wire; the model finds it with tool_search, and from
//              then on it is Loaded for the rest of the thread.
//   Off        the user disabled it (never reaches this layer).
//
// Every enabled tool stays CALLABLE whatever its state: dispatch resolves the
// whole catalog. This layer only decides what is advertised.
//
// Pure: no IO, no globals. The callers (turn launch, subagents, ACP) pass the
// catalog, the thread's loaded set and the limits in.

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "agentty/domain/conversation.hpp"
#include "agentty/tool/registry.hpp"

namespace agentty::tools::plan {

// The user setting (Settings → Tools → MCP tool loading).
enum class Loading : std::uint8_t {
    Auto,      // load everything that fits, the rest on demand
    All,       // load everything up to the provider's hard limit
    OnDemand,  // load natives, pins and what the thread used; search the rest
};

[[nodiscard]] std::string_view to_string(Loading l) noexcept;
[[nodiscard]] Loading loading_from(std::string_view s) noexcept;   // unknown → Auto

// How the model reaches an on-demand tool on this wire.
enum class Discovery : std::uint8_t {
    // Anthropic: every tool goes in `tools`, on-demand ones with
    // defer_loading; our tool_search answers with tool_reference blocks and
    // the API loads the schema itself. Prompt cache untouched.
    DeferredReferences,
    // Everyone else: on-demand tools are left out; tool_search returns their
    // schemas as text, and they ride the wire from the next request on.
    ClientSide,
};

// What the wire can take, per provider + model.
struct Limits {
    // Hard cap on tools in one request. OpenAI chat and Gemini reject >128;
    // 0 = no known cap. For DeferredReferences this bounds only the
    // NON-deferred tools (Anthropic takes up to 10,000 deferred).
    std::size_t max_tools = 0;
    // The model's context window in tokens (0 = unknown → treated as 128k).
    std::size_t context_window = 0;
    // Share of the window PLUGIN tool schemas may take before Auto defers the
    // rest. Natives don't count against it. Claude Code uses 10%.
    double schema_share = 0.10;
    Discovery discovery = Discovery::ClientSide;
    // False for a model that doesn't search for tools reliably (small local
    // models): everything that fits is loaded and nothing is on demand.
    bool search_capable = true;
};

// Tools a thread has loaded, oldest use first. When the hard cap can't take
// them all, the most recently used ones are kept.
struct Loaded {
    std::vector<std::string> order;            // last use, oldest first
    std::unordered_set<std::string> names;

    Loaded() = default;
    Loaded(std::initializer_list<std::string> init) { for (const auto& n : init) touch(n); }
    Loaded(const std::unordered_set<std::string>& s) { for (const auto& n : s) touch(n); }

    void touch(std::string_view name);         // mark used now
    [[nodiscard]] bool contains(std::string_view n) const { return names.contains(std::string{n}); }
    [[nodiscard]] std::size_t size() const noexcept { return order.size(); }
};

// One tool's place in this request.
struct Placement {
    const ToolDef* tool = nullptr;
    bool deferred = false;   // DeferredReferences only: on the wire, defer_loading
};

struct Plan {
    std::vector<Placement> wire;   // what goes in `tools`, catalog order
    std::size_t loaded = 0;        // tools whose schema the model sees now
    std::size_t on_demand = 0;     // enabled, reachable only through tool_search
    std::size_t schema_tokens = 0; // estimated tokens of the loaded schemas
    bool search = false;           // tool_search is on the wire
    // Set when a HARD limit (not the context budget) forced tools on demand
    // under Loading::All; the UI says so instead of hiding it.
    bool capped_by_provider = false;
};

// Rough token cost of one tool's schema on the wire (name + description +
// JSON schema, bytes/3.5 like the rest of agentty's estimators).
[[nodiscard]] std::size_t schema_tokens(const ToolDef& t);

// Decide the request's tools.
//
//   catalog   every enabled tool, in catalog order (natives first)
//   loaded    names this thread already loaded (tool_search hits, calls)
//   search    the tool_search ToolDef, added when anything is on demand
//
// Rules, in order:
//   1. Natives and pinned (always_expose) tools are always loaded.
//   2. Thread-loaded tools come next, newest first, past the schema budget
//      but within max_tools; the oldest go back on demand when they don't fit.
//   3. Auto: the rest load while the schema budget and max_tools allow.
//      All: the rest load while max_tools allows.
//      OnDemand: the rest go on demand.
//   4. Anything left over is on demand, and tool_search joins the wire.
// Nothing past max_tools ever goes out, so any number of tools works.
// Order on the wire is catalog order, never score order, so the tools block
// (the head of the prompt-cache prefix) is byte-stable while the set is.
[[nodiscard]] Plan make_plan(const std::vector<ToolDef>& catalog,
                             const Loaded& loaded,
                             const ToolDef* search, Loading mode,
                             const Limits& limits);

// ── Search ───────────────────────────────────────────────────────────────

struct Hit {
    const ToolDef* tool = nullptr;
    double score = 0;
};

// Rank tools for a query. BM25 over name, server and description, with
// identifiers split (snake_case, camelCase, mcp__srv__tool) and a typo-
// tolerant fallback, so "decompile functon" still finds decompile_function.
// `select:a,b` loads those exact names (what Claude Code calls direct
// selection). Natives are excluded: they are always loaded.
[[nodiscard]] std::vector<Hit> search(const std::vector<ToolDef>& catalog,
                                      std::string_view query, std::size_t k);

// ── Thread state ─────────────────────────────────────────────────────────

// Tool names this thread has loaded: every tool it CALLED, and every tool a
// tool_search in it returned. Derived from the transcript, so it survives a
// restart, a fork and a resume without new persisted state.
[[nodiscard]] Loaded loaded_in(const Thread& thread);
[[nodiscard]] Loaded loaded_in(const std::vector<Message>& messages);

// The name and the marker tool_search writes into its result, so loaded_in
// can read hits back out. One line per hit: "<marker><name>".
inline constexpr std::string_view kSearchToolName = "tool_search";
inline constexpr std::string_view kLoadedMarker = "loaded: ";

} // namespace agentty::tools::plan
