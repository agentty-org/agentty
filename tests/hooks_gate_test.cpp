// hooks_gate_test.cpp — the lifecycle-hooks CONSENT GATE. The security
// property this pins is the whole design: a hooks file that has not been
// explicitly approved NEVER executes, and any byte change to an approved
// file re-gates it. Also covers the pre_tool block semantics and the
// post_tool fire-and-forget contract.
//
// Runs in a temp sandbox CWD + HOME (approvals live under $HOME). Hook
// commands write marker files so execution (or its absence) is observable.

#include "agtest_cap.hpp"
#include "agentty/tool/hooks.hpp"
#include "agentty/tool/util/sandbox.hpp"
#include "agentty/auth/auth.hpp"

#include "agtest.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace hooks = agentty::tools::hooks;

namespace {

void write_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
}

[[nodiscard]] std::string read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
    return s;
}

// A hook command that creates `marker`. cmd.exe (the Windows hook shell) has
// no `touch`; `type nul >` is its spelling.
[[nodiscard]] std::string touch_cmd(const fs::path& marker) {
#ifdef _WIN32
    return "type nul > \"" + marker.string() + "\"";
#else
    return "touch " + marker.string();
#endif
}

// Approve the CURRENT bytes of the hooks file by writing the approval map
// directly (the interactive `agentty hooks approve` does the same store).
void approve_current(const fs::path& hooks_file, const fs::path& home) {
    std::string raw = read_all(hooks_file);
    std::error_code ec;
    auto abs = fs::weakly_canonical(hooks_file, ec);
    nlohmann::json j = nlohmann::json::object();
    j[(ec ? fs::absolute(hooks_file, ec) : abs).string()] =
        agentty::auth::sha256_hex(raw);
    write_file(home / ".agentty" / "hooks_approved.json", j.dump(2));
}

void unapproved_never_runs(const fs::path& sandbox) {
    std::println("--- unapproved_never_runs ---");
    const fs::path marker = sandbox / "pre_ran.marker";
    write_file(sandbox / ".agentty" / "hooks.json",
               nlohmann::json{
                   {"pre_tool", {{{"match", ""},
                                  {"run", touch_cmd(marker)}}}},
               }.dump());

    check(hooks::pending_approval(::agentty::IoAccess::grant()), "unapproved file reports pending");
    auto d = hooks::run_pre_tool("shell", nlohmann::json::object());
    check(!d.blocked(), "unapproved hooks never block");
    check(!fs::exists(marker), "unapproved hook COMMAND NEVER EXECUTED");
    std::println("PASS\n");
}

void approved_runs_and_blocks(const fs::path& sandbox, const fs::path& home) {
    std::println("--- approved_runs_and_blocks ---");
    const fs::path marker = sandbox / "post_ran.marker";
    // pre_tool guard: block any bash whose args mention "rm -rf".
    // post_tool: drop a marker (observability for fire-and-forget).
    write_file(sandbox / ".agentty" / "hooks.json",
               nlohmann::json{
                   {"pre_tool",
                    {{{"match", "shell"},
                      {"run",
#ifdef _WIN32
                       "findstr /c:\"rm -rf\" \"%AGENTTY_HOOK_PAYLOAD_FILE%\" >nul "
                       "&& (echo dangerous command blocked& exit /b 1) "
                       "|| exit /b 0"
#else
                       "grep -q 'rm -rf' \"$AGENTTY_HOOK_PAYLOAD_FILE\" "
                       "&& { echo 'dangerous command blocked'; exit 1; } "
                       "|| exit 0"
#endif
                      }}}},
                   {"post_tool", {{{"match", "shell"},
                                   {"run", touch_cmd(marker)}}}},
               }.dump());
    approve_current(sandbox / ".agentty" / "hooks.json", home);
    check(!hooks::pending_approval(::agentty::IoAccess::grant()), "approved file no longer pending");

    // Benign call: allowed, post fires.
    auto ok = hooks::run_pre_tool("shell", nlohmann::json{{"command", "ls -la"}});
    check(!ok.blocked(), "benign bash allowed");
    (void)hooks::run_post_tool("shell", nlohmann::json{{"command", "ls"}}, "listing…");
    check(fs::exists(marker), "post_tool hook executed for approved file");

    // Dangerous call: blocked with the hook's stdout as reason.
    auto bad = hooks::run_pre_tool("shell", nlohmann::json{{"command", "rm -rf /tmp/x"}});
    check(bad.blocked(), "matching pre_tool guard blocks");
    check(bad.reason.find("dangerous command blocked") != std::string::npos,
          "hook stdout becomes the block reason (got '" + bad.reason + "')");

    // Non-matching tool: guard does not fire.
    auto other = hooks::run_pre_tool("read", nlohmann::json{{"path", "rm -rf"}});
    check(!other.blocked(), "match=bash guard ignores the read tool");
    std::println("PASS\n");
}

void byte_change_regates(const fs::path& sandbox) {
    std::println("--- byte_change_regates ---");
    const fs::path marker = sandbox / "regate.marker";
    // Mutate the approved file (append a space — ANY byte change).
    const fs::path hf = sandbox / ".agentty" / "hooks.json";
    write_file(hf, nlohmann::json{
                       {"pre_tool", {{{"match", ""},
                                      {"run", touch_cmd(marker)}}}},
                   }.dump() + " ");
    check(hooks::pending_approval(::agentty::IoAccess::grant()),
          "byte change to an approved file re-gates (pending again)");
    (void)hooks::run_pre_tool("shell", nlohmann::json::object());
    check(!fs::exists(marker), "re-gated hook did not execute");
    std::println("PASS\n");
}

void kill_switch(const fs::path& sandbox, const fs::path& home) {
    std::println("--- kill_switch ---");
    const fs::path marker = sandbox / "killswitch.marker";
    write_file(sandbox / ".agentty" / "hooks.json",
               nlohmann::json{
                   {"pre_tool", {{{"match", ""},
                                  {"run", touch_cmd(marker)}}}},
               }.dump());
    approve_current(sandbox / ".agentty" / "hooks.json", home);
#ifdef _WIN32
    _putenv_s("AGENTTY_NO_HOOKS", "1");
#else
    ::setenv("AGENTTY_NO_HOOKS", "1", 1);
#endif
    check(!hooks::pending_approval(::agentty::IoAccess::grant()), "kill switch silences pending notice");
    (void)hooks::run_pre_tool("shell", nlohmann::json::object());
    check(!fs::exists(marker), "AGENTTY_NO_HOOKS=1 disables approved hooks");
#ifdef _WIN32
    _putenv_s("AGENTTY_NO_HOOKS", "");   // empty value removes it
#else
    ::unsetenv("AGENTTY_NO_HOOKS");
#endif
    std::println("PASS\n");
}

// An oversized hooks.json must not read as "no hooks file".
//
// read_all returned empty for "past the cap" exactly as it did for
// "missing", so `agentty hooks` printed "no hooks file" while the user's
// file sat right there -- a PreToolUse hook written to BLOCK something was
// silently inert and nothing said so. Of all the caps in this tree this is
// the one that matters most: it gates command execution, and the failure is
// a user believing a guard is armed when it is not.
void oversized_file_is_reported(const fs::path& sandbox) {
    std::println("--- oversized_file_is_reported ---");
    const fs::path cfg = sandbox / ".agentty" / "hooks.json";
    // A real, valid hook -- padded past the 64 KiB cap.
    nlohmann::json j{
        {"pre_tool", {{{"match", ""}, {"run", "exit 2"}}}},
        {"pad", std::string(70u * 1024u, 'x')},
    };
    write_file(cfg, j.dump());
    check(fs::file_size(cfg) > 64u * 1024u, "fixture is past the cap");

    // The hook does NOT run -- that part was always true and stays true.
    check(!hooks::pending_approval(::agentty::IoAccess::grant()),
          "an unloadable file offers nothing to approve");

    // `agentty hooks` must SAY the file was too big rather than claim there
    // is none. Exit 1 so CI notices.
    const int rc = hooks::cli(::agentty::IoAccess::grant(), agtest::cap(), "list");
    check(rc == 1, "hooks list exits non-zero for an oversized file");
    std::println("PASS\n");
}

} // namespace

#ifndef _WIN32
namespace {

// A hooks file in Claude Code's shape, approved.
void cc_hooks(const fs::path& sandbox, const fs::path& home, nlohmann::json hooks) {
    write_file(sandbox / ".agentty" / "hooks.json", nlohmann::json{{"hooks", std::move(hooks)}}.dump());
    approve_current(sandbox / ".agentty" / "hooks.json", home);
}
nlohmann::json one(const char* matcher, std::string cmd) {
    return nlohmann::json::array({{{"matcher", matcher},
                                   {"hooks", {{{"type", "command"}, {"command", std::move(cmd)}}}}}});
}

void claude_code_protocol(const fs::path& sandbox, const fs::path& home) {
    std::println("--- claude_code_protocol ---");

    // The payload is JSON on stdin, in Claude Code's shape.
    const fs::path got = sandbox / "stdin.json";
    cc_hooks(sandbox, home, {{"PreToolUse", one("Bash", "cat > '" + got.string() + "'")}});
    (void)hooks::run_pre_tool("shell", nlohmann::json{{"command", "ls"}, {"timeout", 5}}, "call_7", "thr_1");
    const auto p = nlohmann::json::parse(read_all(got), nullptr, false);
    check(p.is_object(), "payload arrives on stdin as JSON");
    check(p.value("hook_event_name", "") == "PreToolUse", "hook_event_name");
    check(p.value("tool_name", "") == "Bash", "shell is Bash to a Claude Code hook");
    check(p["tool_input"].value("command", "") == "ls", "tool_input.command");
    check(p["tool_input"].value("timeout", 0) == 5000, "timeout in ms, like Claude Code's Bash");
    check(p.value("tool_use_id", "") == "call_7" && p.value("session_id", "") == "thr_1", "ids");

    // Exit 2 denies with stderr; matcher "Bash" picks the shell tool.
    cc_hooks(sandbox, home, {{"PreToolUse", one("Bash", "echo 'no network here' >&2; exit 2")}});
    auto d = hooks::run_pre_tool("shell", nlohmann::json{{"command", "curl x"}});
    check(d.blocked() && d.reason == "no network here", "exit 2 denies, stderr is the reason");
    check(!hooks::run_pre_tool("read", nlohmann::json{{"path", "a"}}).blocked(), "matcher misses Read");

    // Any other non-zero exit is a broken hook, not a block.
    cc_hooks(sandbox, home, {{"PreToolUse", one("Bash", "exit 1")}});
    check(!hooks::run_pre_tool("shell", nlohmann::json{{"command", "ls"}}).blocked(),
          "exit 1 from a Claude Code hook doesn't block");

    // updatedInput rewrites the command (RTK's whole mechanism), and the
    // decision, reason and context come through.
    const std::string rewrite =
        R"(cat >/dev/null; printf '%s' '{"hookSpecificOutput":{"hookEventName":"PreToolUse",)"
        R"("permissionDecision":"allow","permissionDecisionReason":"rtk",)"
        R"("updatedInput":{"command":"rtk git status","description":"st"},)"
        R"("additionalContext":"filtered by rtk"}}')";
    cc_hooks(sandbox, home, {{"PreToolUse", one("Bash", rewrite)}});
    d = hooks::run_pre_tool("shell", nlohmann::json{{"command", "git status"}});
    check(d.permission == hooks::PreToolDecision::Permission::Allow, "allow");
    check(d.updated_args && (*d.updated_args).value("command", "") == "rtk git status",
          "updatedInput rewrites the command");
    check(d.updated_args && (*d.updated_args).value("display_description", "") == "st",
          "description maps back to display_description");
    check(d.additional_context == "filtered by rtk", "additionalContext");

    // Two hooks: the second sees the first one's rewrite, and deny beats allow.
    nlohmann::json chain = nlohmann::json::array();
    chain.push_back(one("Bash", rewrite)[0]);
    chain.push_back(one("Bash", "grep -q 'rtk git status' && { echo seen >&2; exit 2; }; exit 0")[0]);
    cc_hooks(sandbox, home, {{"PreToolUse", chain}});
    d = hooks::run_pre_tool("shell", nlohmann::json{{"command", "git status"}});
    check(d.blocked() && d.reason == "seen", "hooks chain in order; deny wins over allow");

    // ask, and continue:false.
    cc_hooks(sandbox, home, {{"PreToolUse", one("*",
        R"(cat >/dev/null; echo '{"continue":false,"stopReason":"done for today",)"
        R"("hookSpecificOutput":{"permissionDecision":"ask","permissionDecisionReason":"prod db"}}')")}});
    d = hooks::run_pre_tool("write", nlohmann::json{{"path", "a"}});
    check(d.permission == hooks::PreToolDecision::Permission::Ask && d.reason == "prod db", "ask");
    check(d.stop && d.stop_reason == "done for today", "continue:false stops");

    // PostToolUse: updatedToolOutput replaces the output (RTK's filter),
    // and the hook gets the plain stdout, not the model's ``` fence.
    const fs::path post_in = sandbox / "post.json";
    cc_hooks(sandbox, home, {{"PostToolUse", one("Bash",
        "cat > '" + post_in.string() + "'; "
        R"(echo '{"hookSpecificOutput":{"hookEventName":"PostToolUse",)"
        R"("updatedToolOutput":{"stdout":"3 files changed","stderr":""}}}')")}});
    auto post = hooks::run_post_tool("shell", nlohmann::json{{"command", "git diff --stat"}},
                                     "```\na | 1 +\nb | 1 +\nc | 1 +\n```");
    check(post.updated_output && *post.updated_output == "3 files changed", "updatedToolOutput");
    const auto pp = nlohmann::json::parse(read_all(post_in), nullptr, false);
    check(pp.is_object() && pp["tool_response"].value("stdout", "") == "a | 1 +\nb | 1 +\nc | 1 +\n",
          "tool_response.stdout is the command's own output");

    // PostToolUse doesn't fire on a failure; PostToolUseFailure does.
    const fs::path fail_mark = sandbox / "failure.marker";
    cc_hooks(sandbox, home, {{"PostToolUse", one("", "echo '{\"systemMessage\":\"ok\"}'")},
                             {"PostToolUseFailure", one("", touch_cmd(fail_mark))}});
    post = hooks::run_post_tool("shell", nlohmann::json{{"command", "false"}}, "exit 1", true);
    check(post.system_message.empty(), "PostToolUse skips a failure");
    check(fs::exists(fail_mark), "PostToolUseFailure fires on one");

    // Arg translation round-trips for the file tools.
    const auto in = hooks::to_cc_input("edit", nlohmann::json{{"path", "x.cpp"},
        {"edits", {{{"old_text", "a"}, {"new_text", "b"}}}}});
    check(in.value("file_path", "") == "x.cpp" && in.value("old_string", "") == "a",
          "Edit gets file_path / old_string");
    auto back = in;
    back["new_string"] = "c";
    const auto out = hooks::from_cc_input("edit", back, nlohmann::json::object());
    check(out.value("path", "") == "x.cpp" && out["edits"][0].value("new_text", "") == "c",
          "a hook's edit maps back to agentty's shape");

    // Matchers, Claude Code's rules.
    check(hooks::pre_tool_matches({"Edit|Write"}, "write"), "exact list");
    check(!hooks::pre_tool_matches({"Edit"}, "edit_notebook"), "exact means exact");
    check(hooks::pre_tool_matches({"^mcp__"}, "mcp__git__log"), "regex");
    check(hooks::pre_tool_matches({"*"}, "anything"), "star");
    std::println("PASS\n");
}

} // namespace
#endif

TEST_CASE("hooks_gate") {
    const fs::path sandbox =
        fs::temp_directory_path() / ("agentty_hooks_test_" +
                                     std::to_string(::getpid()));
    const fs::path home = sandbox / "home";
    fs::create_directories(home);
    fs::current_path(sandbox);
    // User-scope config resolves via util::user_root() ($AGENTTY_HOME, else
    // home_dir()/.agentty, where home_dir() is $HOME then $USERPROFILE) —
    // point ALL of them at this test's home so the files written under
    // home/.agentty are the ones the loader reads. Setting only USERPROFILE
    // on Windows lost to an inherited $HOME (MSYS2, Git Bash shells).
    ::setenv("HOME", home.string().c_str(), 1);
#ifdef _WIN32
    _putenv_s("USERPROFILE", home.string().c_str());
#endif
    ::unsetenv("AGENTTY_HOME");   // fall back to home_dir()/.agentty
    // Hooks run through the sandbox, whose mode and workspace are process-wide
    // and may be left set by an earlier test case (with a workspace that is
    // not this temp dir). This test is about the consent gate, not the walls.
    (void)agentty::tools::util::sandbox::init(agentty::tools::util::sandbox::Mode::Off);

    std::println("=== hooks_gate_test ===");
    unapproved_never_runs(sandbox);
    approved_runs_and_blocks(sandbox, home);
    byte_change_regates(sandbox);
    kill_switch(sandbox, home);
    oversized_file_is_reported(sandbox);
#ifndef _WIN32
    claude_code_protocol(sandbox, home);
#endif

    std::error_code ec;
    fs::current_path(fs::temp_directory_path(), ec);
    fs::remove_all(sandbox, ec);
}
