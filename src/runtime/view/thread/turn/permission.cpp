#include "agentty/runtime/view/thread/turn/permission.hpp"

#include "agentty/runtime/view/thread/turn/agent_timeline/tool_args.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_helpers.hpp"

namespace agentty::ui {

namespace {

// What the user is being asked to let happen. The chip already names the
// tool, so this says what it will do, as a question.
std::string verb_for(const std::string& n) {
    if (n == "bash" || n == "shell" || n == "diagnostics" || n == "test"
        || n == "process_start")
        return "run this command?";
    if (n == "read" || n == "list_dir" || n == "glob" || n == "grep")
        return "read this?";
    if (n == "edit" || n == "apply_patch" || n == "replace"
        || n == "rewrite_structural")
        return "change this file? (diff above)";
    if (n == "write")  return "write this file?";
    if (n == "remove") return "delete this?";
    if (n == "move")   return "move this?";
    if (n == "web_fetch")  return "open this page?";
    if (n == "web_search") return "search the web for this?";
    if (n == "git_commit") return "make this commit?";
    if (n.rfind("git_", 0) == 0) return "change the repo?";
    return "allow this?";
}

// Short `key  value` lines for tools without a dedicated shape, so a
// prompt never shows a wall of JSON. Long values are cut; nested values
// are dumped compactly.
std::string args_lines(const nlohmann::json& args) {
    std::string out;
    for (auto it = args.begin(); it != args.end(); ++it) {
        if (it.key() == "display_description") continue;
        std::string v = it->is_string() ? it->get<std::string>() : it->dump();
        if (auto nl = v.find('\n'); nl != std::string::npos)
            v = v.substr(0, nl) + " \xe2\x80\xa6";
        if (v.size() > 96) { v.resize(95); v += "\xe2\x80\xa6"; }
        if (!out.empty()) out += '\n';
        out += it.key() + "  " + v;
    }
    return out;
}

} // namespace

maya::Permission::Config inline_permission_config(const PendingPermission& pp,
                                                  const ToolUse& tc) {
    const std::string& n = tc.name.value;
    maya::Permission::Config cfg;
    cfg.tool_name         = tool_display_name(n);
    cfg.verb              = verb_for(n);
    cfg.accent            = tool_category_color(n);
    cfg.show_always_allow = true;

    std::string desc;
    if (!tc.args.is_object()) {
        desc = pp.reason;
    } else if (n == "bash" || n == "shell" || n == "diagnostics" || n == "test"
               || n == "process_start") {
        cfg.lang = "sh";
        desc = tc.args.value("command", "");
        // `command` is not the whole of what runs. `cd` moves it, and `env`
        // can hand the child LD_PRELOAD / BASH_ENV / GIT_SSH_COMMAND, so the
        // prompt has to show everything that decides what executes.
        std::string extra;
        if (const auto cd = pick_arg(tc.args, {"cd", "cwd", "directory"}); !cd.empty())
            extra += "in " + cd;
        if (auto it = tc.args.find("env");
            it != tc.args.end() && it->is_object() && !it->empty()) {
            if (!extra.empty()) extra += "  \xc2\xb7  ";
            extra += "env";
            for (auto e = it->begin(); e != it->end(); ++e) {
                std::string v = e.value().is_string()
                    ? e.value().get<std::string>() : e.value().dump();
                if (v.size() > 48) { v.resize(47); v += "\xe2\x80\xa6"; }
                extra += " " + e.key() + "=" + v;
            }
        }
        if (!extra.empty()) desc += "\n# " + extra;
    } else if (n == "read" || n == "edit" || n == "write" || n == "list_dir"
               || n == "apply_patch" || n == "remove") {
        // Alias-aware: write's canonical key is `file_path`, and models
        // routinely pick filepath/filename for the others.
        desc = pick_arg(tc.args, {"path", "file_path", "filepath", "filename"});
    } else if (n == "move") {
        desc = pick_arg(tc.args, {"source"}) + "  \xe2\x86\x92  "
             + pick_arg(tc.args, {"destination"});
    } else if (n == "web_fetch") {
        desc = tc.args.value("url", "");
    } else if (n == "web_search") {
        desc = tc.args.value("query", "");
    } else if (n == "git_commit") {
        desc = tc.args.value("message", "");
    } else if (n == "find_definition") {
        desc = tc.args.value("symbol", "");
    } else {
        desc = args_lines(tc.args);
    }

    cfg.description = desc.empty() ? pp.reason : desc;
    // A commit message or heredoc can run long; keep the prompt compact and
    // say how much is hidden. Shell commands are kept whole: every byte of
    // what runs is part of the decision.
    if (cfg.lang != "sh") {
        constexpr int kMax = 6;
        int lines = 0;
        std::size_t cut = std::string::npos;
        for (std::size_t i = 0; i < cfg.description.size(); ++i)
            if (cfg.description[i] == '\n' && ++lines == kMax) { cut = i; break; }
        if (cut != std::string::npos) {
            int rest = 0;
            for (std::size_t i = cut; i < cfg.description.size(); ++i)
                rest += cfg.description[i] == '\n';
            cfg.description.resize(cut);
            cfg.description += "\n\xe2\x8b\xaf " + std::to_string(rest) + " more lines";
        }
    }
    return cfg;
}

} // namespace agentty::ui
