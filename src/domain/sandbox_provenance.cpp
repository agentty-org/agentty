// sandbox_provenance.cpp — WHICH path shapes the host later trusts.
//
// The matching moved to claybin (clay::shape_matches, policy/path_shape.hpp)
// because it is general mechanism with a sharp edge: whole-component rather
// than substring, or a rule fires on ".gitignore" and gets ignored; and
// shape rather than exact name, or `git --git-dir=.foo` walks past it, which
// is the bug Cursor shipped and fixed in 3.0.0.
//
// What stays here is the only part that is agentty's opinion: the TABLE. Every
// entry is a place a documented escape landed, or the obvious sibling of one.
// claybin has no view on whether ".vscode/tasks.json" matters -- that is a
// statement about how developer tooling behaves, not about the kernel.

#include "agentty/domain/sandbox_provenance.hpp"
#include "agentty/domain/sandbox_config.hpp"   // Config, kAlwaysMasked*, mask_paths

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#if defined(__linux__)
#include <sys/inotify.h>   // SweepWatch: change detection for the mask sweep
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#include <maya/runtime.hpp>

// The matcher lives in claybin, which is a required submodule -- so this is a
// plain include, not a guarded one.
//
// This used to carry a second, local copy of the same shape logic for builds
// without claybin, on the reasoning that the trust-handoff TABLE is agentty's
// opinion and should compile either way. That reasoning was sound and the
// conclusion was wrong: it meant sixty lines of duplicated matching, two
// configurations where a classification could differ, and a comment admitting
// the duplication was "deliberate". With claybin required there is exactly one
// matcher, which is what the boundary audit wanted in the first place.
#include "claybin/policy/path_shape.hpp"

namespace agentty::sandbox_cfg {

namespace {

using clay::ShapeMatch;
using clay::ShapeRule;
using clay::shape_matches;

// tag == TrustKind, cast through the opaque integer claybin carries.
constexpr std::uint32_t tag(TrustKind k) { return static_cast<std::uint32_t>(k); }

// Ordered: first match wins, so the more specific reason comes first where a
// path could plausibly hit two rules.
constexpr std::array kRules{
    // ── lifecycle hooks: CVE-2026-48124 (Cursor, CVSS 8.5) ──────────────
    ShapeRule{"hooks.json", tag(TrustKind::HookConfig), ShapeMatch::Basename},
    ShapeRule{".claude", tag(TrustKind::HookConfig), ShapeMatch::Component},
    ShapeRule{".agentty", tag(TrustKind::HookConfig), ShapeMatch::Component},
    ShapeRule{".cursor", tag(TrustKind::HookConfig), ShapeMatch::Component},

    // ── editor automation: Antigravity's ".vscode time bomb" ────────────
    ShapeRule{".vscode", tag(TrustKind::EditorTask), ShapeMatch::Component},
    ShapeRule{"tasks.json", tag(TrustKind::EditorTask), ShapeMatch::Basename},
    ShapeRule{"launch.json", tag(TrustKind::EditorTask), ShapeMatch::Basename},
    ShapeRule{".idea", tag(TrustKind::EditorTask), ShapeMatch::Component},

    // ── git: fsmonitor + hooks + config (Cursor 3.0.0) ──────────────────
    //
    // ".git" as a COMPONENT covers the git dir at any depth, submodules
    // included. Bare `config`, `HEAD` and `hooks` rules are gone: they matched
    // src/hooks/ and any file named config, and pinned git's own state files.
    ShapeRule{".git", tag(TrustKind::GitConfig), ShapeMatch::Component},

    // ── interpreters and entry points: GHSA-p9g2-cr55-cw9c ──────────────
    //
    // The Python extension executed a venv interpreter during discovery. Any
    // bin/ under the workspace has the same property.
    ShapeRule{"node_modules/.bin", tag(TrustKind::Interpreter), ShapeMatch::Component},
    ShapeRule{"pyvenv.cfg", tag(TrustKind::Interpreter), ShapeMatch::Basename},
    ShapeRule{".venv", tag(TrustKind::Interpreter), ShapeMatch::Component},
    ShapeRule{"venv", tag(TrustKind::Interpreter), ShapeMatch::Component},

    // ── shell init: runs on the user's next shell here ──────────────────
    ShapeRule{".envrc", tag(TrustKind::ShellInit), ShapeMatch::Basename},
    ShapeRule{".bashrc", tag(TrustKind::ShellInit), ShapeMatch::Basename},
    ShapeRule{".zshrc", tag(TrustKind::ShellInit), ShapeMatch::Basename},
    ShapeRule{".profile", tag(TrustKind::ShellInit), ShapeMatch::Basename},
    ShapeRule{".bash_profile", tag(TrustKind::ShellInit), ShapeMatch::Basename},
    ShapeRule{".bash_login", tag(TrustKind::ShellInit), ShapeMatch::Basename},
    ShapeRule{".bash_logout", tag(TrustKind::ShellInit), ShapeMatch::Basename},
    ShapeRule{".zprofile", tag(TrustKind::ShellInit), ShapeMatch::Basename},
    ShapeRule{".zshenv", tag(TrustKind::ShellInit), ShapeMatch::Basename},
    ShapeRule{".zlogin", tag(TrustKind::ShellInit), ShapeMatch::Basename},
    ShapeRule{".xprofile", tag(TrustKind::ShellInit), ShapeMatch::Basename},
    ShapeRule{".xinitrc", tag(TrustKind::ShellInit), ShapeMatch::Basename},
    ShapeRule{".xsession", tag(TrustKind::ShellInit), ShapeMatch::Basename},
    ShapeRule{".pam_environment", tag(TrustKind::ShellInit), ShapeMatch::Basename},
    ShapeRule{"activate", tag(TrustKind::ShellInit), ShapeMatch::Basename},
    ShapeRule{".direnv", tag(TrustKind::ShellInit), ShapeMatch::Component},
    // fish, and per-user environment.d, read at every login
    ShapeRule{".config/fish", tag(TrustKind::ShellInit), ShapeMatch::Component},
    ShapeRule{".config/environment.d", tag(TrustKind::ShellInit), ShapeMatch::Component},

    // ── session start: run at the next login, outside any sandbox ───────
    ShapeRule{".config/autostart", tag(TrustKind::SessionStart), ShapeMatch::Component},
    ShapeRule{".config/systemd", tag(TrustKind::SessionStart), ShapeMatch::Component},
    ShapeRule{".config/hypr", tag(TrustKind::SessionStart), ShapeMatch::Component},
    ShapeRule{".config/sway", tag(TrustKind::SessionStart), ShapeMatch::Component},
    ShapeRule{".config/i3", tag(TrustKind::SessionStart), ShapeMatch::Component},
    ShapeRule{".config/plasma-workspace", tag(TrustKind::SessionStart), ShapeMatch::Component},

    // ── PATH: a file here shadows a real command for every later shell ──
    ShapeRule{".local/bin", tag(TrustKind::Interpreter), ShapeMatch::Component},

    // ── other agents: config that names commands they run unsandboxed ───
    ShapeRule{".codex", tag(TrustKind::HookConfig), ShapeMatch::Component},
    ShapeRule{".gemini", tag(TrustKind::HookConfig), ShapeMatch::Component},

    // ── build scripts: run on the next build, which is soon ─────────────
    ShapeRule{"Makefile", tag(TrustKind::BuildScript), ShapeMatch::Basename},
    ShapeRule{"package.json", tag(TrustKind::BuildScript), ShapeMatch::Basename},
    ShapeRule{"build.rs", tag(TrustKind::BuildScript), ShapeMatch::Basename},
    ShapeRule{"CMakeLists.txt", tag(TrustKind::BuildScript), ShapeMatch::Basename},
    ShapeRule{"conftest.py", tag(TrustKind::BuildScript), ShapeMatch::Basename},

    // ── daemon sockets: GHSA-v4xv-rqh3-w9mc ─────────────────────────────
    //
    // "One Docker socket to rule them all" escaped Codex, Cursor AND Gemini
    // CLI. A privileged local daemon is a second execution environment.
    ShapeRule{"docker.sock", tag(TrustKind::DaemonSocket), ShapeMatch::Basename},
    ShapeRule{"containerd.sock", tag(TrustKind::DaemonSocket), ShapeMatch::Basename},
    ShapeRule{"podman.sock", tag(TrustKind::DaemonSocket), ShapeMatch::Basename},
};

}  // namespace

bool is_host_trusted(std::string_view path, TrustKind* out_kind) {
    std::uint32_t t = 0;
#if defined(_WIN32)
    // claybin's shape rules split on '/'. A Windows path from
    // std::filesystem ("C:\\repo\\.git\\hooks\\post-commit") had no '/' at
    // all, so NO trusted file ever matched and the handoff gate reported
    // nothing on Windows. Match on the generic form; '\\' is never a legal
    // filename byte there, so this cannot create a false match.
    std::string generic{path};
    std::ranges::replace(generic, '\\', '/');
    path = generic;
#endif
    if (!shape_matches(path, std::span<const ShapeRule>{kRules}, &t)) return false;
    if (out_kind) *out_kind = static_cast<TrustKind>(t);
    return true;
}

namespace {

// Directories never worth walking for a credential file.
//
// Two properties, and BOTH are needed to justify skipping one: it is reliably
// enormous (so walking it costs real time on every spawn), and a credential
// file does not legitimately live there. The second is the safety argument --
// if a name on this list ever becomes somewhere secrets DO live, skipping it
// stops being a performance decision and becomes a hole.
//
// .git is the interesting case: it holds no `.env`, but it DOES hold
// `.git/config`, which can carry a credential helper. That is covered by the
// trust-handoff rules above (TrustKind::GitConfig) rather than by masking, so
// skipping it here loses nothing.
//
// `vendor` is not here: Go and PHP check vendored code into the repo, and a
// .env or key committed with it is exactly what the masks exist for.
constexpr std::string_view kNeverWalk[] = {
    ".git", "node_modules", "target", "build", "dist", ".venv", "venv",
    "__pycache__", ".mypy_cache", ".pytest_cache", ".cache", ".next",
    ".gradle", ".tox", "Pods", ".terraform",
};

[[nodiscard]] bool never_walk(std::string_view name) {
    for (auto d : kNeverWalk) if (name == d) return true;
    return false;
}

// Does this basename name a credential file?
//
// Two kinds of rule, told apart by an explicit marker:
//
//   SUFFIX  `*.pem`, `*.tfvars` — matches any file ENDING in the extension.
//           These are why the walk had to exist at all: a suffix cannot be
//           turned into a path without looking at what is actually on disk.
//   NAME    everything else matches the whole basename, exactly.
//
// The marker is explicit rather than inferred from shape. Inferring "starts
// with a dot and has no second dot" looks tidy and is wrong: it silently
// promotes `.env`, `.npmrc` and `.netrc` to suffixes, so `prod.env` and
// `scoped.npmrc` start being masked too. Widening what a SECURITY rule covers
// as a side effect of adding an unrelated entry is exactly the kind of
// accident this list must not have -- a mask that appears from nowhere is as
// confusing as one that goes missing, and nobody reviewing the list would see
// it.
[[nodiscard]] bool is_masked_name(std::string_view name) {
    for (const char* n : kAlwaysMaskedNames) {
        std::string_view rule{n};
        if (rule.starts_with("*")) {
            rule.remove_prefix(1);                 // "*.pem" -> ".pem"
            // `>` not `>=`, so the rule never matches a file that IS the bare
            // suffix: a file called ".pem" is not a key.
            if (name.size() > rule.size() && name.ends_with(rule)) return true;
            continue;
        }
        if (name == rule) return true;
    }
    return false;
}

// Walk `dir` to `depth_left` levels, appending every file whose basename is a
// credential name.
//
// Recursive, and safe to be: the depth is bounded by mask_scan_depth before
// the first call, and directory symlinks are never followed, so no input can
// drive this deeper than the bound.
void sweep(const std::filesystem::path& dir, int depth_left,
           std::vector<std::string>& out) {
    std::error_code ec;
    // skip_permission_denied: a workspace can contain directories we cannot
    // read, and a masking sweep must not fail the whole spawn over one of
    // them. Directory symlinks are NOT followed -- following one out of the
    // workspace would mask host paths the user never asked about, and one
    // pointing back inside would walk the tree twice.
    std::filesystem::directory_iterator it{
        dir, std::filesystem::directory_options::skip_permission_denied, ec};
    if (ec) return;

    for (const auto& entry : it) {
        std::error_code sec;
        const auto name = entry.path().filename().string();
        // A symlink NAMED .env is masked as the link rather than followed:
        // following it would mask wherever it points and leave the link
        // itself readable. The entry's type comes from readdir's d_type.
        const bool link = entry.is_symlink(sec);
        if (sec) continue;
        if (!link && entry.is_directory(sec) && !sec) {
            if (depth_left > 0 && !never_walk(name))
                sweep(entry.path(), depth_left - 1, out);
            continue;
        }
        if (is_masked_name(name)) out.push_back(entry.path().string());
    }
}

// A fingerprint of the DIRECTORY SHAPE that `sweep` above traverses: the
// mtime of every directory it would descend into, folded together.
//
// Why this is the right key for sweep's cache, and why it is exact rather
// than approximate: creating, deleting or renaming an entry updates the
// mtime of the directory CONTAINING it. That is the only mutation that can
// change which files sweep returns -- editing a file's CONTENTS cannot add
// or remove it from the list, and that is the operation whose parent mtime
// does not move. So "same stamp" really does imply "same answer".
//
// It must mirror sweep's traversal EXACTLY -- same depth arithmetic, same
// never_walk skips, same no-follow rule. A stamp that visited fewer
// directories than the sweep would be blind to changes inside the ones it
// skipped, which is a stale mask, which is a hole. Both functions therefore
// take the same shape deliberately; change one and you must change the other.
//
// Cheap on purpose: one status() per DIRECTORY, no readdir of its entries
// beyond what the iterator already yields, and no per-file symlink_status.
std::uint64_t dir_stamp(const std::filesystem::path& dir, int depth_left) {
    std::error_code ec;
    std::uint64_t h = 1469598103934665603ull;          // FNV-1a offset basis
    const auto mix = [&h](std::uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
    };

    const auto mt = std::filesystem::last_write_time(dir, ec);
    if (!ec)
        mix(static_cast<std::uint64_t>(mt.time_since_epoch().count()));

    std::filesystem::directory_iterator it{
        dir, std::filesystem::directory_options::skip_permission_denied, ec};
    if (ec) return h;

    for (const auto& entry : it) {
        if (depth_left <= 0) break;
        // is_symlink()/is_directory() on the ENTRY use the d_type readdir
        // already returned (no syscall on Linux/macOS filesystems that
        // report it), and fall back to lstat only when d_type is unknown.
        // entry.symlink_status() always lstat'ed: ~2,000 syscalls per
        // spawn on this tree, ~15% of a sandboxed `true`.
        std::error_code sec;
        if (entry.is_symlink(sec) || sec) continue;
        if (!entry.is_directory(sec) || sec) continue;
        const auto name = entry.path().filename().string();
        if (never_walk(name)) continue;
        mix(dir_stamp(entry.path(), depth_left - 1));
    }
    return h;
}

#if defined(__linux__)
// "Has anything under the swept dirs changed?" -- answered without walking.
//
// dir_stamp re-walks every directory down to mask_scan_depth on EVERY spawn
// (~400 openat+getdents+stat on this tree, the largest share left in a
// sandboxed `true`) to learn, almost always, that nothing changed. An
// inotify watch over those same directories answers with one ioctl.
//
// Race-free by construction:
//   - the queue is only PEEKED (FIONREAD) on the read path, so a pending
//     event is seen by every reader until a re-arm;
//   - a re-arm bumps the generation BEFORE it drains and re-watches, and
//     the cache trusts only an entry taken under the CURRENT generation, so a
//     reader racing a re-arm fails the generation check and walks;
//   - the walk happens AFTER the watches are in place, so a change landing
//     mid-walk is already queued and the next reader walks again.
// Anything else -- no inotify, too many dirs, queue overflow (itself an
// event) -- leaves the generation at 0 and falls back to dir_stamp: slower,
// never stale. One inotify fd for the process; never closed.
struct WatchState {
    std::string   root;
    int           depth = -1;
    std::uint64_t gen = 0;   // 0: not armed
};

class SweepWatch {
public:
    // The current generation when nothing is pending for (root, depth);
    // 0 when the caller must walk.
    [[nodiscard]] std::uint64_t quiet(std::string_view root, int depth) const {
        if (fd_ < 0) return 0;
        const auto st = state_.load();
        if (st->gen == 0 || st->root != root || st->depth != depth) return 0;
        int pending = 0;
        if (::ioctl(fd_, FIONREAD, &pending) != 0 || pending != 0) return 0;
        return st->gen;
    }

    // Re-watch every dir dir_stamp visits under `root`. Returns the new
    // generation, or 0 when the tree can't be watched (then: always walk).
    std::uint64_t arm(const std::filesystem::path& root, int depth) {
        if (fd_ < 0) return 0;
        // Invalidate first: any reader from here on sees gen 0 and walks.
        const auto gen = ++gen_counter_;   // only arm() touches it (lane-serialised)
        state_.store(WatchState{});
        // Drop the previous watches and drain what they queued.
        for (int wd : wds_) (void)::inotify_rm_watch(fd_, wd);
        wds_.clear();
        alignas(inotify_event) char buf[8192];
        while (::read(fd_, buf, sizeof buf) > 0) {}
        bool ok = true;
        constexpr std::size_t kMaxWatches = 4096;
        std::vector<std::pair<std::filesystem::path, int>> stack{{root, depth}};
        while (ok && !stack.empty()) {
            auto [d, left] = std::move(stack.back());
            stack.pop_back();
            const int wd = wds_.size() >= kMaxWatches ? -1
                : ::inotify_add_watch(fd_, d.c_str(),
                      IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO
                      | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR | IN_DONT_FOLLOW);
            if (wd < 0) { ok = false; break; }
            wds_.push_back(wd);
            if (left <= 0) continue;
            std::error_code ec;
            std::filesystem::directory_iterator it{
                d, std::filesystem::directory_options::skip_permission_denied, ec};
            if (ec) continue;
            for (const auto& e : it) {
                std::error_code sec;
                if (e.is_symlink(sec) || sec || !e.is_directory(sec) || sec) continue;
                if (never_walk(e.path().filename().string())) continue;
                stack.emplace_back(e.path(), left - 1);
            }
        }
        if (!ok) return 0;
        state_.store(WatchState{root.string(), depth, gen});
        return gen;
    }

private:
    // arm() is serialised by try_rearm's lane; it alone touches wds_.
    int fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    std::vector<int> wds_;
    std::uint64_t gen_counter_ = 0;
    maya::snapshot<WatchState> state_;
};

// The one watch, and a try-lock lane so a spawn that finds another already
// re-arming just walks instead of waiting (or racing wds_). The lane is a
// snapshot<int> taken by CAS: update() returns what it stored, so the caller
// whose unique ticket landed there owns the re-arm.
struct SweepWatchSlot {
    SweepWatch           watch;
    maya::snapshot<int>  arming;   // 0 idle; otherwise the owner's ticket
    maya::snapshot<int>  tickets;
};
SweepWatchSlot& sweep_watch_slot() {
    static auto* s = new SweepWatchSlot;   // leaked: outlives every spawner
    return *s;
}

// Re-arm if no one else is; 0 when another spawn holds the lane.
std::uint64_t try_rearm(const std::filesystem::path& root, int depth) {
    auto& s = sweep_watch_slot();
    const int me = *s.tickets.update([](const int& t) { return t + 1; });
    const int got = *s.arming.update(
        [](const int& cur, int mine) { return cur == 0 ? mine : cur; }, me);
    if (got != me) return 0;
    const auto gen = s.watch.arm(root, depth);
    s.arming.store(0);
    return gen;
}
#endif

}  // namespace

std::vector<std::string> mask_paths(const Config& cfg, std::string_view workspace,
                                   std::string_view home) {
    std::vector<std::string> out;

    // 1. $HOME credentials. Not configurable, by design: a control the user
    //    can switch off to make their build work is a control that is off.
    if (!home.empty()) {
        const std::string h{home};
        for (const char* m : kAlwaysMasked) out.push_back(h + m);
    }

    // 2. Credential names inside the workspace, found by walking. The
    //    workspace is bound READ-WRITE by both backends, so scope cannot save
    //    anything in here and the mask is the only wall.
    if (!workspace.empty()) {
        // CACHED, because this is in the latency path of EVERY command.
        //
        // The header already warned that an unbounded walk here "would put a
        // full directory scan in the latency path of every shell command" and
        // bounded the depth in response. Bounding was not enough: measured on
        // the agentty tree at the shipped depth=3, the sweep costs 3.7 ms and
        // runs again for every single spawn -- re-deriving a byte-identical
        // answer each time. Against a ~10 ms sandboxed `true`, that is most
        // of the floor, and it grows with the repo rather than with the work.
        //
        // THE KEY IS THE TREE'S OWN SHAPE, not a clock.
        //
        // A time-to-live was the obvious first try and it is WRONG here: it
        // means a credential that appears inside the TTL is readable by the
        // next command. sandbox_live_check caught it immediately -- it plants
        // `services/api/key.pem` and runs a command against it in well under
        // a second, and the stale list let the body through. A mask that is
        // merely eventually-correct is not a wall, and a 2-second hole is
        // exactly long enough for the `write file` → `run tool` pair an agent
        // does constantly.
        //
        // So key on the mtimes of every DIRECTORY the sweep descends into.
        // Creating, deleting or renaming a file bumps its parent's mtime --
        // that is the one thing POSIX guarantees here, and it is precisely
        // the event that can change this list. Stat-ing the directories costs
        // a fraction of walking their entries (one syscall per directory, no
        // readdir, no per-file symlink_status), so the common case stays
        // cheap while staying exact.
        // maya::snapshot rather than a lock: commands spawn from tool worker
        // threads, so this really is shared, but it is a read-mostly value
        // keyed by (ws, depth, stamp). Readers take one atomic load; a miss
        // sweeps OUTSIDE any lock and publishes the result. Two concurrent
        // misses both sweep and the last store wins -- both results are
        // correct for their key, so that costs a walk, never correctness.
        struct Cache {
            std::string              ws;
            int                      depth = -1;
            std::uint64_t            stamp = 0;
            std::uint64_t            gen = 0;   // SweepWatch generation it was taken under
            std::vector<std::string> paths;
        };
        // Function-local static: the first command can land from a worker
        // during startup, and a file-scope global would race its own ctor.
        static maya::snapshot<Cache> cache;

        const int depth = static_cast<int>(cfg.mask_scan_depth);
        std::vector<std::string> hits;
        bool done = false;
#if defined(__linux__)
        // Fast path: a quiet watch that vouches for exactly this sweep.
        if (const auto gen = sweep_watch_slot().watch.quiet(workspace, depth); gen != 0) {
            auto cur = cache.load();
            if (cur->gen == gen && cur->ws == workspace && cur->depth == depth) {
                hits = cur->paths;
                done = true;
            }
        }
#endif
        if (!done) {
            // Arm FIRST, so whatever changes while we walk is already queued.
            std::uint64_t gen = 0;
#if defined(__linux__)
            gen = try_rearm(std::filesystem::path{workspace}, depth);
#endif
            // OUTSIDE any lock: stat-ing the tree is the slow part, and
            // holding a lock across it would serialise every concurrent
            // spawn behind one filesystem walk.
            const std::uint64_t stamp =
                dir_stamp(std::filesystem::path{workspace}, depth);
            auto cur = cache.load();
            if (cur->ws == workspace && cur->depth == depth && cur->stamp == stamp) {
                hits = cur->paths;
            } else {
                Cache fresh;
                sweep(std::filesystem::path{workspace}, depth, fresh.paths);
                fresh.ws    = std::string{workspace};
                fresh.depth = depth;
                fresh.stamp = stamp;
                hits = fresh.paths;
                cur = maya::shared<Cache>::make(std::move(fresh));
            }
            Cache next = *cur;
            next.gen = gen;
            cache.store(std::move(next));
        }
        out.insert(out.end(), std::make_move_iterator(hits.begin()),
                   std::make_move_iterator(hits.end()));
    }

    // 3. The user's own denials, last, so an explicit deny cannot be undone
    //    by one of ours.
    for (const auto& d : cfg.deny_paths)
        if (auto r = resolve_user_path(d, workspace, home); !r.empty())
            out.push_back(std::move(r));

    // Deduplicate: a path can arrive from the sweep and from deny_paths, and
    // emitting the same mount twice is at best noise in the plan. Sorting is
    // also what makes the argv stable across runs -- a test asserting order
    // would otherwise be flaky on directory-iteration order.
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string resolve_user_path(std::string_view path, std::string_view workspace,
                              std::string_view home) {
    if (path.empty()) return {};
    std::filesystem::path p;
    if (path == "~" || path.starts_with("~/")) {
        if (home.empty()) return {};
        p = std::filesystem::path{std::string{home}} / std::string{path.substr(path == "~" ? 1 : 2)};
    } else {
        p = std::filesystem::path{std::string{path}};
        if (p.is_relative()) {
            if (workspace.empty()) return {};
            p = std::filesystem::path{std::string{workspace}} / p;
        }
    }
    std::string s = p.lexically_normal().string();
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}

namespace {

// Names every scope but Host passes. Names only, never values: PATH reaches
// the command exactly as agentty has it, because a rebuilt PATH is a second
// copy of the toolchain table that drifts from the user's real one.
constexpr std::string_view kEnvEssential[] = {
    "PATH", "HOME", "USER", "LOGNAME", "SHELL", "LANG", "LANGUAGE", "TZ",
    "TMPDIR", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME",
    "XDG_STATE_HOME",
    "LC_ALL", "LC_CTYPE", "LC_NUMERIC", "LC_TIME", "LC_COLLATE", "LC_MONETARY",
    "LC_MESSAGES", "LC_PAPER", "LC_NAME", "LC_ADDRESS", "LC_TELEPHONE",
    "LC_MEASUREMENT", "LC_IDENTIFICATION",
    // Who a commit is from. Identity, not a secret, and git ranks these above
    // every config file, so a user who exports them means it.
    "GIT_AUTHOR_NAME", "GIT_AUTHOR_EMAIL", "GIT_COMMITTER_NAME", "GIT_COMMITTER_EMAIL",
#if defined(__APPLE__)
    // TMPDIR above points at the /var/folders dir the sandbox grants.
    "__CF_USER_TEXT_ENCODING",
#elif defined(_WIN32)
    // PATHEXT resolves `git` to git.exe; without SYSTEMROOT, Winsock and
    // crypto fail to initialise in the child. MSYS* drive Git Bash's PATH.
    "PATHEXT", "SYSTEMROOT", "WINDIR", "SYSTEMDRIVE", "COMSPEC", "TEMP", "TMP",
    "USERPROFILE", "HOMEDRIVE", "HOMEPATH", "APPDATA", "LOCALAPPDATA",
    "PROGRAMDATA", "PROGRAMFILES", "PROGRAMFILES(X86)", "PROGRAMW6432",
    "COMMONPROGRAMFILES", "COMMONPROGRAMFILES(X86)", "USERNAME",
    "NUMBER_OF_PROCESSORS", "PROCESSOR_ARCHITECTURE", "OS",
    "MSYSTEM", "MSYS", "MSYS2_PATH_TYPE",
#endif
};

// Toolchain adds these names...
//
// The bar for a name here: a build or test run reads it to find a compiler,
// a library, an activated environment or a trust store, and a missing one
// FAILS that build (or worse, silently builds against the system copy). An
// activated venv whose VIRTUAL_ENV vanishes inside the sandbox is the canonical
// way an allowlist reads as "the sandbox broke my project".
constexpr std::string_view kEnvToolchain[] = {
    // C/C++ toolchain selection and flags (make, cmake, autotools, cc-rs, cgo).
    "CC", "CXX", "CPP", "LD", "AR", "AS", "NM", "RANLIB", "STRIP", "OBJCOPY",
    "OBJDUMP", "FC", "F77", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS",
    "LDLIBS", "FFLAGS", "ARFLAGS", "MAKEFLAGS", "MFLAGS", "GNUMAKEFLAGS",
    // Where the linker and the compiler look.
    "LD_LIBRARY_PATH", "LIBRARY_PATH", "CPATH", "C_INCLUDE_PATH",
    "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH", "SOURCE_DATE_EPOCH", "PKG_CONFIG",
    "JAVA_HOME", "JDK_HOME", "JAVA_OPTS", "M2_HOME", "KOTLIN_HOME",
    "VIRTUAL_ENV", "VIRTUAL_ENV_PROMPT", "PNPM_HOME", "GEM_HOME", "GEM_PATH",
    "OPAMROOT", "OPAMSWITCH", "STACK_ROOT", "CABAL_DIR",
    "BUNDLE_GEMFILE", "BUNDLE_PATH", "BUNDLE_WITHOUT", "BUNDLE_WITH",
    "BUNDLE_DEPLOYMENT", "BUNDLE_JOBS", "BUNDLE_BIN", "BUNDLE_APP_CONFIG",
    "NIX_PATH", "NIX_PROFILES", "NIX_SSL_CERT_FILE", "IN_NIX_SHELL",
    // Go's variables have no common prefix that excludes GOOGLE_*.
    "GOPATH", "GOROOT", "GOBIN", "GOCACHE", "GOMODCACHE", "GOFLAGS",
    "GOPROXY", "GOPRIVATE", "GONOSUMDB", "GONOPROXY", "GOSUMDB", "GOINSECURE",
    "GOTOOLCHAIN", "GOENV", "GOOS", "GOARCH", "GOAMD64", "GOARM", "GOWORK",
    "GOEXPERIMENT", "GOTMPDIR", "GODEBUG",
    // Trust stores: a corporate CA bundle missing is every https fetch failing.
    "SSL_CERT_FILE", "SSL_CERT_DIR", "REQUESTS_CA_BUNDLE", "CURL_CA_BUNDLE",
    "GIT_SSL_CAINFO", "GIT_SSL_CAPATH",
    // Output shape. Test runners and linters key colour and width off these,
    // and CI flips a good share of test suites into their non-interactive mode.
    "COLORTERM", "NO_COLOR", "FORCE_COLOR", "CLICOLOR", "CLICOLOR_FORCE",
    "COLUMNS", "LINES", "CI",
#if defined(__APPLE__)
    // xcrun reads these.
    "DEVELOPER_DIR", "SDKROOT", "MACOSX_DEPLOYMENT_TARGET",
    "DYLD_LIBRARY_PATH", "DYLD_FALLBACK_LIBRARY_PATH",
    "HOMEBREW_PREFIX", "HOMEBREW_CELLAR", "HOMEBREW_REPOSITORY",
#elif defined(_WIN32)
    // MSVC's developer prompt: without these cl.exe finds no headers or libs.
    "INCLUDE", "LIB", "LIBPATH", "VCINSTALLDIR", "VCTOOLSINSTALLDIR",
    "VSINSTALLDIR", "WINDOWSSDKDIR", "WINDOWSSDKVERSION", "UCRTVERSION",
    "UNIVERSALCRTSDKDIR", "VCPKG_ROOT",
#endif
};

// ...and whole families. Prefix matches are where secrets hide (a registry
// token is CARGO_REGISTRIES_<NAME>_TOKEN), so every one of these is also run
// past kEnvSecretMarks below; an exact name above is not, because it was read.
constexpr std::string_view kEnvToolchainPrefixes[] = {
    // The version managers whose dirs kHomeToolSubdirs makes readable.
    "CARGO_", "RUSTUP_", "NVM_", "PYENV_", "RBENV_", "ASDF_",
    "BUN_", "DENO_", "DOTNET_", "SDKMAN_", "VOLTA_", "FNM_", "GHCUP_",
    // Language runtimes and their package managers.
    "RUST",            // RUSTFLAGS, RUSTC_WRAPPER, RUST_BACKTRACE, RUST_LOG
    "CGO_", "PYTHON",  // PYTHONPATH, PYTHONHOME, PYTHONUTF8, ...
    "PIP_", "UV_", "POETRY_", "PIPX_", "CONDA_", "MAMBA_",
    "NODE_", "npm_config_", "NPM_CONFIG_", "YARN_", "COREPACK_",
    "MAVEN_", "GRADLE_", "ANDROID_", "NUGET_",
    "RUBY", "PERL", "COMPOSER_", "MIX_", "HEX_", "OPAM", "JULIA_", "ZIG_",
    // Build drivers and compiler caches.
    "CMAKE_", "CTEST_", "MESON_", "NINJA_", "PKG_CONFIG", "CCACHE_",
    "SCCACHE_", "DISTCC_", "CONAN_", "VCPKG_", "BAZEL_",
};

// Secret-shaped names that an allowed prefix would otherwise let through.
// Matched case-blind on every OS: npm reads npm_config__authToken, so a
// case-sensitive "AUTH" would wave the npm registry token straight through.
constexpr std::string_view kEnvSecretMarks[] = {
    "TOKEN", "SECRET", "PASSWORD", "PASSWD", "PASSPHRASE", "API_KEY", "APIKEY",
    "ACCESS_KEY", "PRIVATE_KEY", "CREDENTIAL", "AUTH", "COOKIE", "SESSION",
};
// ...and the same as a final word: CONAN_PASS, NUGET_KEY, GRADLE_..._PAT.
constexpr std::string_view kEnvSecretSuffixes[] = {"_KEY", "_PAT", "_PASS", "_PWD"};

[[nodiscard]] constexpr char ascii_up(char c) noexcept {
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c;
}

[[nodiscard]] bool ieq(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (ascii_up(a[i]) != ascii_up(b[i])) return false;
    return true;
}

[[nodiscard]] bool looks_secret(std::string_view key) noexcept {
    for (auto mark : kEnvSecretMarks)
        for (std::size_t i = 0; i + mark.size() <= key.size(); ++i)
            if (ieq(key.substr(i, mark.size()), mark)) return true;
    for (auto suf : kEnvSecretSuffixes)
        if (key.size() > suf.size() && ieq(key.substr(key.size() - suf.size()), suf))
            return true;
    return false;
}

[[nodiscard]] bool env_name_eq(std::string_view a, std::string_view b) noexcept {
#if defined(_WIN32)
    return ieq(a, b);
#else
    return a == b;
#endif
}

}  // namespace

bool env_value_has_credentials(std::string_view value) noexcept {
    // scheme://userinfo@host. A password (user:pw@) is a credential under any
    // scheme; a bare user is one under http(s), where it is how a token rides
    // (https://ghp_xxx@github.com) -- but not under ssh, where `git@` is just
    // the account name.
    for (std::size_t at = value.find("://"); at != std::string_view::npos;
         at = value.find("://", at + 3)) {
        std::size_t s0 = at;
        while (s0 > 0) {
            const char c = value[s0 - 1];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                  || c == '+' || c == '-' || c == '.'))
                break;
            --s0;
        }
        const auto scheme = value.substr(s0, at - s0);   // "sparse+https", "git+ssh"
        const auto start = at + 3;
        const auto end = value.find_first_of("/?# \t,;", start);
        const auto auth = value.substr(start, end == std::string_view::npos
                                                  ? std::string_view::npos : end - start);
        const auto amp = auth.rfind('@');
        if (amp == std::string_view::npos || amp == 0) continue;
        if (auth.substr(0, amp).find(':') != std::string_view::npos) return true;
        const bool http = scheme.size() >= 4 &&
            (ieq(scheme.substr(scheme.size() - 4), "http") ||
             (scheme.size() >= 5 && ieq(scheme.substr(scheme.size() - 5), "https")));
        if (http) return true;
    }
    return false;
}

bool env_allowed(EnvScope scope, std::string_view key,
                 std::span<const std::string> keep, std::string_view value) noexcept {
    if (scope == EnvScope::Host) return true;
    for (const auto& k : keep)
        if (env_name_eq(key, k)) return true;
    for (auto n : kEnvEssential)
        if (env_name_eq(key, n)) return true;
    if (scope == EnvScope::Essential) return false;
    // PIP_INDEX_URL=https://user:token@pypi.corp/ is an ordinary-looking name
    // carrying a credential; GOPROXY and npm's registry do the same.
    if (env_value_has_credentials(value)) return false;
    for (auto n : kEnvToolchain)
        if (env_name_eq(key, n)) return true;
    for (auto pre : kEnvToolchainPrefixes)
        if (key.size() > pre.size() && env_name_eq(key.substr(0, pre.size()), pre))
            return !looks_secret(key);
    return false;
}

Config apply_posture(const Config& base, Posture p) {
    Config c = base;
    c.configured = true;

    // Deliberately NOT touched: backend, read_paths, write_paths, deny_paths,
    // env_keep.
    // A posture is a statement about how tight the walls are; the engine and
    // the user's project-specific path list are not the posture's business.
    // See the header.

    switch (p) {
        case Posture::Permissive:
            // Off, not "loose". There is no filter, no caps and no netns --
            // only the mount walls, which both backends always build. This
            // exists so that a user whose toolchain breaks has somewhere to go
            // that is not `--sandbox off`: the masks and the mount namespace
            // still hold, which is strictly more than nothing.
            c.fs_scope = FsScope::Toolchain;
            c.net_mode = NetMode::Full;
            c.syscall_mode = SyscallMode::Off;
            c.wx_protect = false;
            c.memory_mb = 0;
            // The fork-bomb cap SURVIVES Permissive, and it is the one
            // resource wall that does.
            //
            // memory/cpu/nofile/time all go to 0 here for the reason the field
            // comments give: the right ceiling is a property of the machine and
            // the build, so a wrong guess turns a working `cargo build` into an
            // OOM kill that looks like agentty's fault. Dropping them is what
            // makes Permissive a real answer to "my toolchain breaks".
            //
            // max_procs is different, and the field's own comment says why:
            // "unlike memory, there IS a number that is safe everywhere. No
            // legitimate build needs 4096 concurrent processes; `make -j` on a
            // 64-core box peaks in the low hundreds, and the pathological case
            // is not 'a big build' but `:(){ :|:& };:`, which wants millions."
            //
            // It was being zeroed with the rest, which handed every Permissive
            // user an unbounded fork bomb from any approved command -- the
            // exact finding tests/sandbox_audit.cpp was written for (it saw
            // resource.pids as `none` and that is why 4096 became the default),
            // reintroduced through the preset. claybin treats 0 as unlimited
            // and sets no rlimit and no pids.max at all, so this was not a
            // softer cap; it was no cap.
            //
            // Permissive means "the walls are off", not "the machine is
            // forfeit". A fork bomb is not a toolchain compatibility problem,
            // so there is nothing for this to unbreak.
            c.max_procs = 4096;
            c.cpu_percent = 0;
            c.max_open_files = 0;
            c.wall_clock_secs = 0;
            c.cpu_secs = 0;
            c.scope_ipc = false;
            c.close_inherited_fds = true;   // never a compatibility problem
            c.fake_hostname = false;
            c.mask_scan_depth = 3;          // secrets stay masked REGARDLESS
            // A token in the environment is the one way to push over https
            // from the box; Permissive is where a user goes to get that back.
            c.env_scope = EnvScope::Host;
            // Handoff stays at the caller's value rather than being loosened.
            // Permissive is about the walls around the process; the handoff
            // gate is about what the agent writes for the HOST to run, and
            // those are different questions (§14). A loose sandbox is a
            // choice; letting the agent author your git hooks as a side effect
            // of that choice is not one anybody made.
            break;

        case Posture::Balanced:
            // Exactly the struct defaults, by construction: this is what
            // `configured = false` already means, so a user who picks Balanced
            // explicitly gets the same boundary as one who never opened the
            // pane. Writing them out rather than default-constructing keeps
            // the four postures readable side by side.
            c.fs_scope = FsScope::Toolchain;
            c.net_mode = NetMode::Full;
            c.syscall_mode = SyscallMode::Compiler;
            c.wx_protect = true;
            c.memory_mb = 0;        // the machine's business, not ours
            c.max_procs = 4096;     // fork-bomb cap; the audit found this none
            c.cpu_percent = 0;
            c.max_open_files = 0;
            c.wall_clock_secs = 0;
            c.cpu_secs = 0;
            c.scope_ipc = true;
            c.close_inherited_fds = true;
            c.fake_hostname = false;
            c.mask_scan_depth = 3;
            c.env_scope = EnvScope::Toolchain;
            c.handoff = HandoffPolicy::Refuse;
            break;

        case Posture::Hardened:
            // Every wall claybin can build, with caps a real build survives.
            //
            // The numbers are the interesting part. They are chosen to be
            // survivable rather than impressive: 8 GB and 400% (four cores)
            // because a linker peaks high and an OOM kill mid-build reads as
            // an agentty bug, and no wall-clock cap at all because a long
            // build is not an attack and a timeout that fires on one teaches
            // the user to turn the sandbox off.
            c.fs_scope = FsScope::Minimal;
            c.net_mode = NetMode::Ports;   // 443/80/22/53 by default
            c.syscall_mode = SyscallMode::Strict;
            c.wx_protect = true;
            c.memory_mb = 8192;
            c.max_procs = 2048;
            c.cpu_percent = 400;
            c.max_open_files = 4096;
            c.wall_clock_secs = 0;
            c.cpu_secs = 0;
            c.scope_ipc = true;
            c.close_inherited_fds = true;
            c.fake_hostname = true;        // keeps the host name out of logs
            c.mask_scan_depth = 5;         // deeper sweep; it is a cost row
            c.env_scope = EnvScope::Essential;
            c.handoff = HandoffPolicy::Refuse;
            break;

        case Posture::Airgapped:
            // The posture that makes §11's admission untrue.
            //
            // Everywhere else in this subsystem, network is open and the
            // documentation says plainly that read access plus network is read
            // plus exfiltrate. This is the one setting where that stops being
            // true, which is why it is worth a preset even though it breaks
            // most workflows: for reading and editing code it costs nothing,
            // and it is the only configuration here where a leaked credential
            // cannot leave the machine.
            c.fs_scope = FsScope::Minimal;
            c.net_mode = NetMode::None;
            c.syscall_mode = SyscallMode::Strict;
            c.wx_protect = true;
            c.memory_mb = 8192;
            c.max_procs = 2048;
            c.cpu_percent = 400;
            c.max_open_files = 4096;
            c.wall_clock_secs = 0;
            c.cpu_secs = 0;
            c.scope_ipc = true;
            c.close_inherited_fds = true;
            c.fake_hostname = true;
            c.mask_scan_depth = 5;
            c.env_scope = EnvScope::Essential;
            c.handoff = HandoffPolicy::Refuse;
            break;

        case Posture::Custom:
            // Not selectable: it is what detect_posture REPORTS, not a thing
            // to apply. Returning the config untouched is the only sound
            // answer -- there is nothing to write.
            break;
    }
    return c;
}

Posture detect_posture(const Config& cfg) {
    // By COMPARISON against what each posture would produce, not by inspecting
    // fields one at a time.
    //
    // This is why the label cannot be wrong. apply_posture is the single
    // definition of what a posture means, and detect asks it rather than
    // re-encoding the same knowledge in a second place that can drift. Add a
    // field to Config and this keeps working; add a field and hand-write the
    // comparison, and the day you forget one the pane starts claiming
    // "Hardened" for a config that is not.
    //
    // `configured` is normalised out of the comparison on both sides. It is
    // bookkeeping about whether the user has ever touched the pane, not part of
    // the boundary -- and apply_posture always sets it, so without this a
    // never-opened config (configured = false) would report Custom even though
    // its walls are exactly Balanced. The pane would open on "Custom" for every
    // new user, which is both wrong and the least useful thing it could say.
    Config probe = cfg;
    probe.configured = true;

    // Order matters only for display: Balanced is checked first because it is
    // the overwhelmingly common answer, so the common case costs one compare.
    for (auto p : {Posture::Balanced, Posture::Hardened, Posture::Airgapped,
                   Posture::Permissive}) {
        if (probe == apply_posture(probe, p)) return p;
    }
    return Posture::Custom;
}

}  // namespace agentty::sandbox_cfg
