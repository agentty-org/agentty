// plugin_disabled_tools_test — a disabled server keeps its tool list.
//
// The bug: disabling a plugin whose tools were showing made the WHOLE tool
// list vanish (a disabled server advertises nothing live), while a server that
// started disabled, or one with individually-excluded tools, kept them. The
// fix caches each server's last-advertised tools (s_seen in plugin_model) and
// falls back to it when the server isn't connected — so the tree stays
// populated across a disable→enable round-trip.
//
// This drives the REAL producer: connect a working stdio server (populating
// the cache), snapshot, disable it, re-snapshot, and assert the tools persist.

#include "agtest_cap.hpp"
#include "agentty/tool/registry.hpp"
#include "agentty/tool/tool_loading.hpp"
#include "agentty/mcp/client.hpp"
#include "agentty/tool/plugin.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

namespace fs = std::filesystem;
using namespace agentty;

namespace {  // fold: TU-local (bundled into agentty_standalone_tests)
static int g_fails = 0;
static void check(bool ok, const std::string& what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_fails;
}

static std::size_t tool_count_for(const std::string& server) {
    auto m = mcp::plugin_model();
    for (const auto& s : m.servers)
        if (s.name == server) return s.tools.size();
    return static_cast<std::size_t>(-1);   // server absent
}

}  // namespace (fold)

int main() {
#ifdef SIGPIPE   // Windows has no SIGPIPE: a broken pipe is a write error
    std::signal(SIGPIPE, SIG_IGN);
#endif
    ::setenv("AGENTTY_MCP_CONNECT_TIMEOUT_MS", "5000", 1);

    // Use the real, always-connecting date_server example binary. A scripted
    // stdio server proved flaky to connect under the test's spawn env; the
    // built example is the reliable source of a genuine handshake. Skip
    // cleanly if it isn't built (e.g. a partial checkout).
    const char* bin = std::getenv("AGENTTY_DATE_SERVER");
    fs::path date_server = bin && *bin
        ? fs::path{bin}
        : fs::path{"build/third_party/mcp-cpp/examples/date-mcp/date_server"};
    if (!fs::exists(date_server)) {
        // Try relative to the source tree root guesses. The submodules live
        // under third_party/, so agentty's build tree mirrors that path;
        // mcp-cpp's own standalone build tree is the second family.
        for (const char* c : {"third_party/mcp-cpp/examples/date-mcp/date_server",
                              "../build/third_party/mcp-cpp/examples/date-mcp/date_server",
                              "third_party/mcp-cpp/build/examples/date-mcp/date_server",
                              "../third_party/mcp-cpp/build/examples/date-mcp/date_server"}) {
            if (fs::exists(c)) { date_server = c; break; }
        }
    }
    if (!fs::exists(date_server)) {
        std::printf("  (skip: date_server example not built — set "
                    "AGENTTY_DATE_SERVER)\nAll disabled-tools tests passed.\n");
        return 0;
    }
    date_server = fs::absolute(date_server);

    auto tmp = fs::temp_directory_path()
             / ("agentty_disabled_tools_" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp / ".agentty");
    // Isolate via AGENTTY_HOME, NOT $HOME. user_root() resolves
    // $AGENTTY_HOME FIRST and only falls back to $HOME — and the folded
    // standalone main sets $AGENTTY_HOME to its own sandbox for the whole
    // binary. So a test that repoints only $HOME writes its mcp.json into a
    // directory nothing ever reads: the server never appears in the model,
    // and every assertion here reports the "server absent" sentinel
    // (size_t)-1 rather than a real tool count. Set both, so the test is
    // correct whether or not the harness pins AGENTTY_HOME.
    ::setenv("AGENTTY_HOME", (tmp / ".agentty").c_str(), 1);
    ::setenv("HOME", tmp.c_str(), 1);
    ::unsetenv("USERPROFILE");

    const fs::path cfg = tmp / ".agentty" / "mcp.json";
    {
        std::ofstream f(cfg);
        f << R"({"mcpServers":{"demo":{"command":")" << date_server.string()
          << R"("}}})";
    }
    // date_server advertises exactly 2 tools (current_date, days_between).
    constexpr std::size_t kN = 2;

    // 1. Connect → the server advertises its tools; snapshot records them.
    tools::warm_registry();          // triggers connect_initial_mcp
    (void)tools::reload_mcp_plugins(); // ensure a fresh connected pool
    const std::size_t connected = tool_count_for("demo");
    check(connected == kN,
          "connected server advertises its tools (got "
          + std::to_string(connected) + ")");

    // 2. Disable the whole server, reload (it no longer connects).
    check(tools::plugin::set_server_disabled(::agentty::IoAccess::grant(), agtest::cap(), cfg, "demo", true)
              == tools::plugin::EditResult::Ok, "disable succeeds");
    (void)tools::reload_mcp_plugins();

    // 3. THE FIX: the disabled server still lists its tools (from cache),
    //    instead of collapsing to zero.
    const std::size_t disabled = tool_count_for("demo");
    check(disabled == kN,
          "disabled server KEEPS its tools in the snapshot (got "
          + std::to_string(disabled) + ") — no vanishing tree");

    // 4. Re-enable → back to live, still kN.
    check(tools::plugin::set_server_disabled(::agentty::IoAccess::grant(), agtest::cap(), cfg, "demo", false)
              == tools::plugin::EditResult::Ok, "re-enable succeeds");
    (void)tools::reload_mcp_plugins();
    const std::size_t reenabled = tool_count_for("demo");
    check(reenabled == kN,
          "re-enabled server shows its tools again (got "
          + std::to_string(reenabled) + ")");

    // 5. A DISABLED server's cached tools are display-only: they never count
    //    as loaded or on demand (#98), and annotate never marks them.
    {
        auto m_on = mcp::plugin_model();
        tools::tool_loading::annotate(m_on);
        (void)tools::plugin::set_server_disabled(::agentty::IoAccess::grant(), agtest::cap(), cfg, "demo", true);
        (void)tools::reload_mcp_plugins();
        auto m_off = mcp::plugin_model();
        tools::tool_loading::annotate(m_off);
        check(m_off.enabled_mcp() + kN == m_on.enabled_mcp(),
              "disabling drops the plugin tool count by its tool count "
              "(on=" + std::to_string(m_on.enabled_mcp()) + " off="
              + std::to_string(m_off.enabled_mcp()) + ")");
        bool any_marked = false;
        for (const auto& s : m_off.servers)
            if (s.name == "demo")
                for (const auto& t : s.tools) if (t.on_demand) any_marked = true;
        check(!any_marked, "a disabled server's tools are never marked on demand");

        // 5b. The cap accounting. Two valid tools are never over any cap, so
        //     both skip counters must be zero — and the loaded count can
        //     never exceed the cap that produced it. This is the invariant
        //     the pane's "N left out by the cap" line is built on.
        check(m_on.cap.skipped_by_schema == 0,
              "a server with valid schemas skips none of them (got "
              + std::to_string(m_on.cap.skipped_by_schema) + ")");
        check(m_on.cap.skipped_by_budget == 0,
              "two tools fit any cap, so nothing is left out (got "
              + std::to_string(m_on.cap.skipped_by_budget) + ")");
        check(m_on.cap.deferred <= m_on.on_demand_count,
              "deferred tools are a subset of on-demand (deferred "
              + std::to_string(m_on.cap.deferred) + " > on demand "
              + std::to_string(m_on.on_demand_count) + ")");
        check(m_on.cap.max_tools == 0 || m_on.loaded_count <= m_on.cap.max_tools,
              "loaded never exceeds the cap (loaded "
              + std::to_string(m_on.loaded_count) + ", max "
              + std::to_string(m_on.cap.max_tools) + ")");
        (void)tools::plugin::set_server_disabled(::agentty::IoAccess::grant(), agtest::cap(), cfg, "demo", false);
        (void)tools::reload_mcp_plugins();
    }

    // 6. A command-less server surfaces an ERROR, not an eternal "connecting…".
    {
        std::ofstream f(cfg);
        f << R"({"mcpServers":{"broken":{}}})";
        f.close();
        (void)tools::reload_mcp_plugins();
        auto m = mcp::plugin_model();
        bool found = false;
        for (const auto& s : m.servers)
            if (s.name == "broken") { found = true;
                check(!s.error.empty() && !s.connected,
                      "command-less server is an error, not connecting forever");
            }
        check(found, "command-less server still appears in the model");
    }

    fs::remove_all(tmp);
    if (g_fails == 0) { std::printf("\nAll disabled-tools tests passed.\n"); return 0; }
    std::printf("\n%d disabled-tools test(s) FAILED.\n", g_fails);
    return 1;
}
