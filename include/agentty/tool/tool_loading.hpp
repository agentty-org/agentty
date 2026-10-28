#pragma once
// agentty::tools::tool_loading — the live MCP tool-loading setting and the
// tool_search tool (#98).
//
// Same shape as web_search_policy: the setting is persisted with the rest of
// settings.json and edited in the TUI, but read on turn workers (TUI, `agentty
// run`, subagents, ACP), so it lives in one process-wide slot.

#include <string>
#include <unordered_set>
#include <vector>

#include "agentty/mcp/plugin_model.hpp"
#include "agentty/provider/provider.hpp"
#include "agentty/tool/registry.hpp"
#include "agentty/tool/tool_plan.hpp"
#include "agentty/util/io.hpp"

namespace agentty::tools::tool_loading {

// Load the setting from settings.json unless one is already installed.
void seed(Io io);
[[nodiscard]] plan::Loading current();
void install(plan::Loading l);

// The tool_search ToolDef. Searches the live catalog when called, so it sees
// tools from a server that connected after the request was built.
[[nodiscard]] const ToolDef& search_tool();

// What a provider + model can take. `anthropic` picks deferred references;
// `context_window` 0 = unknown. `model` decides whether on-demand loading is
// used at all: a small model that won't search gets everything that fits.
[[nodiscard]] plan::Limits limits_for(const provider::Selection& sel,
                                      std::size_t context_window,
                                      std::string_view model = {});

// The one entry point every request builder uses: plan the tools for this
// turn and append them to `out` as wire specs.
//
//   catalog   the selected tools (live catalog, after any per-caller filter)
//   loaded    tools::plan::loaded_in(thread)
//
// Returns the plan so the caller can log it and the UI can show counts. The
// caller keeps the Catalog that `pool` points into alive for the call.
struct Request {
    std::vector<const ToolDef*> pool;          // the tools this caller may use
    plan::Loaded loaded;
    plan::Limits limits;
    plan::Loading mode = plan::Loading::Auto;
    // Each server's handshake instructions, shown in tool_search's
    // description next to that server (mcp::mcp_server_instructions()).
    std::vector<std::pair<std::string, std::string>> server_instructions;
};
plan::Plan plan_into(const Request& r, std::vector<provider::ToolSpec>& out);

// Fill the Plugins pane's view of loading (#98): counts, cost, and which
// tools are on demand, as a fresh thread on the ACTIVE provider would see
// them. Runs the same planner the request builders do.
void annotate(mcp::PluginModel& m);

} // namespace agentty::tools::tool_loading
