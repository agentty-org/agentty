// sandbox_escape_test — the macOS sandbox-exec (SBPL) profile injection guard.
//
// The workspace root is interpolated into the SBPL profile's
// (subpath "<path>") write clause. That path is user/attacker-influenceable
// (--workspace / cwd) and macOS/APFS permits ", \, and control chars in
// directory names. Without escaping, a path like  /tmp/ws")(allow default)("
// would close the string early and inject SBPL syntax. sbpl_escape() must make
// the path inert (escape \ and ") or, for a path that can't be represented in
// an SBPL string at all (control chars), return empty so the caller omits the
// clause and FAILS CLOSED (no workspace write access) rather than emitting a
// broken/injected profile.

#include "agtest.hpp"

#include "agentty/tool/util/sandbox.hpp"
#include "agentty/tool/util/fs_helpers.hpp"   // workspace_root

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

namespace sb = agentty::tools::util::sandbox;

TEST_CASE("sandbox: JIT commands are recognised for the W^X exemption") {
    for (const char* c : {"node -e 1", "npm install", "cd web && pnpm test",
                          "/usr/bin/node22 x.js", "FOO=1 npx tsc -p .", "(java -jar a.jar)",
                          "./gradlew build", "bun run dev",
                          "git status | node fmt.js", "sh -c 'node x.js'",
                          "bash -lc \"cd a && npm test\"", "env NODE_ENV=1 node a.js",
                          "timeout 30s npm test", "nohup nice -n 5 node srv.js &",
                          "echo $(node -v)", "exec java -jar x.jar", "x=1; node a",
                          "if true; then node a; fi"})
        check(sb::command_needs_jit(c), c);
#if defined(_WIN32)
    check(sb::command_needs_jit("C:\\x\\node.exe a.js"), "windows path");
#endif
    for (const char* c : {"ls -la", "python3 x.py", "cargo build", "make -j8",
                          "cat node_modules/x", "grep -r nodejs.org .", "echo javascript", "",
                          // a JIT name that is only an argument must not lift W^X
                          "echo node; ./exploit", "echo \"npm\" | cat", "ls node java",
                          "python3 -c 'print(\"node\")'", "grep node *.js",
                          "git commit -m 'bump node'", "./run.sh node"})
        check(!sb::command_needs_jit(c), c);
}


TEST_CASE("sandbox escape") {
    std::printf("=== sandbox_escape_test ===\n");

    // Ordinary paths pass through unchanged.
    check(sb::sbpl_escape("/Users/me/project") == "/Users/me/project",
          "plain path is unchanged");
    check(sb::sbpl_escape("/tmp/a-b_c.1") == "/tmp/a-b_c.1",
          "safe punctuation is unchanged");
    check(sb::sbpl_escape("") == "", "empty path stays empty");

    // A double quote is backslash-escaped — the injection vector. Without
    // this, everything after the " would be reparsed as SBPL.
    check(sb::sbpl_escape("/tmp/ws\"x") == "/tmp/ws\\\"x",
          "double quote is backslash-escaped");
    check(sb::sbpl_escape("/tmp/a\"b\"c") == "/tmp/a\\\"b\\\"c",
          "every double quote is escaped");

    // The actual attack shape: a path that tries to close the subpath string
    // and inject an (allow default). After escaping, the quotes are inert, so
    // the whole thing stays a single string literal.
    {
        const std::string evil = "/tmp/ws\")(allow default)(\"";
        const std::string esc  = sb::sbpl_escape(evil);
        check(esc.find("\\\"") != std::string::npos, "attack quotes are escaped");
        // No UNescaped quote survives: every " in the result is preceded by \.
        bool clean = true;
        for (std::size_t i = 0; i < esc.size(); ++i)
            if (esc[i] == '"' && (i == 0 || esc[i - 1] != '\\')) clean = false;
        check(clean, "no unescaped quote can terminate the SBPL string");
    }

    // Backslash is escaped too (so it can't escape our escaping).
    check(sb::sbpl_escape("/tmp/a\\b") == "/tmp/a\\\\b",
          "backslash is doubled");
    // A backslash right before a quote must not let the quote slip through:
    // "\\" + "\"" → "\\\\" + "\\\"" (both escaped independently).
    check(sb::sbpl_escape("a\\\"b") == "a\\\\\\\"b",
          "backslash-then-quote: both escaped, quote stays inert");

    // Control characters can't live in an SBPL string → fail closed (empty).
    check(sb::sbpl_escape("/tmp/a\nb").empty(), "newline → empty (fail closed)");
    check(sb::sbpl_escape("/tmp/a\rb").empty(), "carriage return → empty");
    check(sb::sbpl_escape("/tmp/a\tb").empty(), "tab → empty");
    check(sb::sbpl_escape(std::string("/tmp/a\0b", 8)).empty(),
          "embedded NUL → empty");

#if defined(__linux__)
    // The claybin posture carries the equivalent walls, and sandbox_pane_test
    // asserts them against claybin_posture_for_test(). The bwrap argv checks
    // that used to live here went with bwrap itself.
#endif

}
