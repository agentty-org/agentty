// settings_add_render_probe — the settings-list add-prompt, mid-edit:
//   ./agentty_standalone_tests settings_add_render_probe            short input
//   ./agentty_standalone_tests settings_add_render_probe --long     pasted long line (must WINDOW, not overflow)
//   ./agentty_standalone_tests settings_add_render_probe --mid      caret mid-string (←←←)
//   ./agentty_standalone_tests settings_add_render_probe --armed    browsing, delete armed
#include <cstdio>
#include <cstdlib>
#include <string>

#include <maya/print.hpp>
#include <maya/core/render_context.hpp>

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/panel/appearance.hpp"
#include "agentty/runtime/panel/settings/list.hpp"
#include "agentty/runtime/view/panels.hpp"

using namespace agentty;
namespace pn = agentty::ui::panel;

int main(int argc, char** argv) {
    const std::string arg = argc > 1 ? argv[1] : "";
    Model m;

    if (arg == "--pane") {
        pn::Appearance ap;
        ap.pane.form = pn::build_appearance_form(m.d.ui(), m.env.terminal);
        ap.pane.form.cursor = argc > 2 ? std::atoi(argv[2]) : 0;
        m.ui.panel.descend(std::move(ap));
        int w = 86;
        if (const char* ww = std::getenv("PROBE_WIDTH"); ww) w = std::atoi(ww);
        maya::RenderContext c2{w, 40, maya::render_generation(), true};
        maya::RenderContextGuard g2(c2);
        auto e2 = ui::appearance_panel(m);
        std::fputs(maya::render_to_string(e2, w).c_str(), stdout);
        return 0;
    }

    pn::SettingsList o;
    o.concern = settings::Category::Plugins;
    if (arg == "--plugins") {
        // A realistic pane: the #98 server (75 tools, most on demand), a
        // small healthy one, a pinned tool, a disabled server, a broken one.
        auto& pm = m.ui.plugins;
        pm.native_tool_count = 45;
        pm.loading = "auto";
        pm.context_window = 128'000;
        auto add = [&](std::string name, int n, int on_demand_from, bool connected,
                       bool disabled, std::string error, std::string command) {
            agentty::mcp::ServerState s;
            s.name = std::move(name);
            s.connected = connected;
            s.disabled = disabled;
            s.error = std::move(error);
            s.command = std::move(command);
            for (int i = 0; i < n; ++i) {
                agentty::mcp::ToolState t;
                t.name = s.name + "_tool_" + std::to_string(i);
                t.description = "does thing number " + std::to_string(i);
                t.on_demand = connected && !disabled && i >= on_demand_from;
                if (t.on_demand) ++pm.on_demand_count; else if (connected && !disabled) ++pm.loaded_count;
                s.tools.push_back(std::move(t));
            }
            pm.servers.push_back(std::move(s));
        };
        add("binary_ninja", 75, 12, true, false, "", "uvx binary-ninja-mcp");
        add("github", 6, 99, true, false, "", "npx -y @github/mcp");
        pm.servers[1].tools[2].pinned = true;
        add("slack", 9, 99, false, true, "", "npx -y @slack/mcp");
        add("jira", 0, 99, false, false, "connect: spawn failed: npx not found", "npx -y jira-mcp");
        pm.schema_tokens = 9'400;
        o.index = argc > 2 ? std::atoi(argv[2]) : 0;
        if (const char* ex = std::getenv("PROBE_EXPAND"); ex && *ex) o.expanded = {ex};
    } else if (arg == "--general") {
        o.concern = settings::Category::General;
        o.index  = argc > 2 ? std::atoi(argv[2]) : 0;
        if (const char* sw = std::getenv("PROBE_SMART"); sw && *sw == '1')
            m.d.smart.enabled = true;
    } else if (arg == "--armed") {
        o.confirm_remove = "some-server";
    } else {
        o.input_active = true;
        if (arg == "--long") {
            o.input = "my-server uvx --from git+https://github.com/example/"
                      "some-very-long-repository-name mcp-server --flag one "
                      "--flag two --project";
            o.cursor = static_cast<int>(o.input.size());
        } else if (arg == "--mid") {
            o.input  = "my-server npx -y @example/mcp-thing";
            o.cursor = 9;   // just after "my-server"
        } else {
            o.input  = "my-server npx -y @example/mcp-thing";
            o.cursor = static_cast<int>(o.input.size());
        }
    }
    m.ui.panel.descend(std::move(o));

    maya::RenderContext ctx{86, 40, maya::render_generation(), true};
    int width = 86;
    if (const char* w = std::getenv("PROBE_WIDTH"); w) width = std::atoi(w);
    ctx.width = width;
    maya::RenderContextGuard g(ctx);
    auto el  = ui::settings_list_panel(m);
    auto out = maya::render_to_string(el, width);
    std::fputs(out.c_str(), stdout);
    return 0;
}
