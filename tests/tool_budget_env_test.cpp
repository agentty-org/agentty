// tool_budget_env_test — #98: every MCP tool stays callable, and
// $AGENTTY_MCP_TOOL_BUDGET caps how many load per request, not which exist.
//
// The old budget trimmed the CATALOG, so a tool past it was "unknown" even
// when the model called it by its exact name. Drives the real producer (the
// date_server example, 2 tools) and checks both halves: the catalog keeps
// every tool, and the per-request plan honours the env cap.

#include "agentty/tool/registry.hpp"
#include "agentty/tool/tool_loading.hpp"
#include "agentty/tool/tool_plan.hpp"
#include "agentty/mcp/client.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;
using namespace agentty;

namespace {  // fold: TU-local (bundled into agentty_standalone_tests)
static int g_fails = 0;
static void check(bool ok, const std::string& what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_fails;
}

static std::size_t demo_in_catalog() {
    std::size_t n = 0;
    for (const auto& t : (*tools::wire_tools()))
        if (t.name.value.rfind("mcp__demo__", 0) == 0) ++n;
    return n;
}

// The request an OpenAI-family turn would carry with "all" loading.
static std::pair<tools::plan::Plan, std::size_t> plan_now() {
    const auto cat = tools::wire_tools();
    provider::Selection sel;
    sel.kind = provider::Kind::OpenAI;
    tools::tool_loading::Request tr;
    for (const auto& t : *cat)
        if (!tools::withheld_by_policy(t)) tr.pool.push_back(&t);
    tr.limits = tools::tool_loading::limits_for(sel, 1'000'000);
    tr.mode = tools::plan::Loading::All;
    std::vector<provider::ToolSpec> specs;
    auto p = tools::tool_loading::plan_into(tr, specs);
    return {p, specs.size()};
}
}  // namespace (fold)

int main() {
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);
#endif
    ::setenv("AGENTTY_MCP_CONNECT_TIMEOUT_MS", "5000", 1);
    ::unsetenv("AGENTTY_MCP_TOOL_BUDGET");

    const char* bin = std::getenv("AGENTTY_DATE_SERVER");
    fs::path date_server = bin && *bin
        ? fs::path{bin}
        : fs::path{"build/third_party/mcp-cpp/examples/date-mcp/date_server"};
    if (!fs::exists(date_server)) {
        for (const char* c : {"third_party/mcp-cpp/examples/date-mcp/date_server",
                              "../build/third_party/mcp-cpp/examples/date-mcp/date_server",
                              "third_party/mcp-cpp/build/examples/date-mcp/date_server",
                              "../third_party/mcp-cpp/build/examples/date-mcp/date_server"}) {
            if (fs::exists(c)) { date_server = c; break; }
        }
    }
    if (!fs::exists(date_server)) {
        std::printf("  (skip: date_server example not built — set "
                    "AGENTTY_DATE_SERVER)\nAll tool-budget tests passed.\n");
        return 0;
    }
    date_server = fs::absolute(date_server);

    auto tmp = fs::temp_directory_path()
             / ("agentty_tool_budget_" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp / ".agentty");
    ::setenv("AGENTTY_HOME", (tmp / ".agentty").c_str(), 1);
    ::setenv("HOME", tmp.c_str(), 1);
    ::unsetenv("USERPROFILE");
    {
        std::ofstream f(tmp / ".agentty" / "mcp.json");
        f << R"({"mcpServers":{"demo":{"command":")" << date_server.string() << R"("}}})";
    }
    constexpr std::size_t kN = 2;   // current_date, days_between

    tools::warm_registry();
    (void)tools::reload_mcp_plugins();
    // The natives a request can carry: the registry minus what policy
    // withholds here (the question tool has no one to ask in a test).
    std::size_t native = 0;
    for (const auto& t : tools::native_registry())
        if (!tools::withheld_by_policy(t)) ++native;
    check(native > 0, "native registry is non-empty");

    // ── 1. Default: both tools in the catalog and on the wire ────────────
    {
        check(demo_in_catalog() == kN, "both demo tools are in the catalog");
        auto [p, wire] = plan_now();
        check(p.on_demand == 0 && !p.search, "default cap: everything loads, no search tool");
    }

    // ── 2. Cap = native + 1: one tool loads, one is on demand, both callable
    {
        ::setenv("AGENTTY_MCP_TOOL_BUDGET", std::to_string(native + 1).c_str(), 1);
        (void)tools::reload_mcp_plugins();
        check(demo_in_catalog() == kN,
              "the cap never trims the catalog (demo tools=" + std::to_string(demo_in_catalog()) + ")");
        check(tools::find("mcp__demo__days_between").def != nullptr
              && tools::find("mcp__demo__current_date").def != nullptr,
              "both tools resolve by name: callable whatever the cap");
        auto [p, wire] = plan_now();
        check(wire <= native + 1, "the request carries at most the cap (wire="
              + std::to_string(wire) + ", cap=" + std::to_string(native + 1) + ")");
        check(p.on_demand >= 1 && p.search, "past the cap, tools are on demand behind tool_search");
        check(p.capped_by_provider, "the plan says the cap, not the user, put them there");
    }

    // ── 3. 0 = no cap ───────────────────────────────────────────────────
    {
        ::setenv("AGENTTY_MCP_TOOL_BUDGET", "0", 1);
        auto [p, wire] = plan_now();
        check(p.on_demand == 0 && !p.search, "0 lifts the cap: everything loads");
    }

    // ── 4. Unparsable keeps the provider default ────────────────────────
    {
        ::setenv("AGENTTY_MCP_TOOL_BUDGET", "not-a-number", 1);
        auto [p, wire] = plan_now();
        check(p.on_demand == 0 && wire <= 128, "unparsable falls back to the provider's cap");
    }

    ::unsetenv("AGENTTY_MCP_TOOL_BUDGET");
    (void)tools::reload_mcp_plugins();
    fs::remove_all(tmp);
    if (g_fails == 0) { std::printf("\nAll tool-budget tests passed.\n"); return 0; }
    std::printf("\n%d tool-budget test(s) FAILED.\n", g_fails);
    return 1;
}
