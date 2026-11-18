// The trust-handoff gate. See the header for why this sits on the tool call
// rather than inside the sandbox.

#include "agentty/tool/util/handoff_gate.hpp"

#include <maya/runtime.hpp>

#include <algorithm>
#include <ranges>
#include <unordered_set>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "agentty/tool/util/sandbox.hpp"
#include "agentty/util/logx.hpp"

namespace agentty::tools::util::handoff {

namespace fs = std::filesystem;

namespace {

// Same storage shape and the same reasoning as the broker's blocked feed: see
// sandbox_broker.cpp. A lock-free `maya::snapshot`: a small value replaced whole
// by `update()` and copied out by readers, so no rank and no capability;
// function-local static because the first handoff can arrive from a tool worker
// during startup and a file-scope global would race its own constructor.
maya::snapshot<std::vector<sandbox_cfg::TrustHandoff>>& feed() {
    static maya::snapshot<std::vector<sandbox_cfg::TrustHandoff>> f;
    return f;
}

// Whether the latest snapshot hit its cap. A snapshot, like feed(), so the
// pane reads it without a lock.
maya::snapshot<bool>& last_truncated() {
    static maya::snapshot<bool> t{false};
    return t;
}

// Whether the latest snapshot found a core.hooksPath dir too large to pin
// whole. Read by the Sandbox pane, like last_truncated().
maya::snapshot<bool>& last_large_hook_dir() {
    static maya::snapshot<bool> t{false};
    return t;
}

// Bounded for the same reason the broker's feed is: a loop writing to a trusted
// path would otherwise grow this without limit, which is a denial of service the
// gate inflicts on its own host. Smaller than the broker's 64 because a handoff
// is a far rarer and far louder event -- if there are 32 distinct ones, the
// count stopped being the information a while ago.
constexpr std::size_t kMaxHandoffs = 32;

[[nodiscard]] std::uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

void remember(const sandbox_cfg::TrustHandoff& h) {
    (void)feed().update([](const std::vector<sandbox_cfg::TrustHandoff>& old,
                           const sandbox_cfg::TrustHandoff& incoming) {
        std::vector<sandbox_cfg::TrustHandoff> all = old;
        // Collapse a repeat of the same path. Unlike the broker's feed this
        // checks the WHOLE list rather than just the back: a handoff is
        // identified by its path, and the same file rewritten twice in a turn
        // is one fact about one file, not a sequence worth preserving.
        for (auto& existing : all) {
            if (existing.write.path == incoming.write.path) {
                existing.write.at_ms = incoming.write.at_ms;
                existing.write.command = incoming.write.command;
                return all;
            }
        }
        all.push_back(incoming);
        // Drop the oldest when full, keeping the newest -- flushing an early
        // handoff out of view costs an attacker 32 distinct ones, each of which
        // is itself recorded and (by default) refused.
        if (all.size() > kMaxHandoffs) all.erase(all.begin());
        return all;
    }, h);
}

}  // namespace

namespace {
// Is `path` a file the nearest repo's git config makes git run? Defined
// beside the config parser below.
[[nodiscard]] bool is_git_config_target(const fs::path& path);
}  // namespace

Verdict check_with(std::string_view path, std::string_view tool,
                   sandbox_cfg::HandoffPolicy policy) {
    Verdict v;

    sandbox_cfg::TrustKind kind{};
    bool trusted = sandbox_cfg::is_host_trusted(path, &kind);

    // If the SPELLING is innocent, try the RESOLVED path before believing it.
    //
    // is_host_trusted() is a pure shape walk, so it sees only what it was
    // handed. A symlinked directory therefore hides a trusted shape entirely:
    //
    //     ln -s .git/hooks innocuous
    //     write innocuous/post-merge      <- installs a real git hook
    //
    // Measured: "innocuous/post-merge" reads as PLAIN, and the same file via
    // ".git/hooks/post-merge" reads as trusted. The write lands in the hook
    // directory either way, so matching only the spelling is a gate the model
    // can step around by creating one symlink -- which it is allowed to do,
    // since a symlink inside the workspace is ordinary work.
    //
    // Resolution is the second check rather than a replacement for the first:
    // weakly_canonical touches the filesystem, and the common case is an
    // ordinary path that must stay a few string compares. Only a path that
    // looks innocent pays for the resolve, and `trusted` short-circuits the
    // one that already matched.
    if (!trusted) {
        std::error_code ec;
        const fs::path resolved = fs::weakly_canonical(fs::path{path}, ec);
        if (!ec && !resolved.empty() && resolved != fs::path{path}) {
            sandbox_cfg::TrustKind rk{};
            if (sandbox_cfg::is_host_trusted(resolved.string(), &rk)) {
                trusted = true;
                kind = rk;
                AGT_LOG(Tool, Warn, "sandbox.handoff",
                        "path resolves into a trusted location: {} -> {}",
                        logx::body(std::string{path}),
                        logx::body(resolved.string()));
            }
        }
    }

    // Not a trusted SHAPE, but maybe a target: core.hooksPath (.husky) and the
    // scripts git config names have ordinary names. Costs a walk up to the
    // nearest repo and its config, so it runs only after the cheap checks and
    // never under Allow, which records nothing.
    if (!trusted && policy != sandbox_cfg::HandoffPolicy::Allow
        && is_git_config_target(fs::path{path})) {
        trusted = true;
        kind = sandbox_cfg::TrustKind::GitConfig;
    }

    if (!trusted) {
        // The common case, and it must stay cheap: this runs on every write the
        // agent makes. `is_host_trusted` is a table walk over path shapes with
        // no I/O, so an ordinary path costs a few string compares.
        return v;
    }

    v.is_handoff = true;
    v.kind = kind;

    // Record BEFORE branching on the policy. A handoff that was allowed under
    // `Allow` is still the single most interesting thing that happened to the
    // workspace this turn, and a feed that only lists refusals would go silent
    // exactly when the user has turned the gate off -- which is when they most
    // need to see it.
    sandbox_cfg::TrustHandoff h;
    h.write.path = std::string{path};
    h.write.command = std::string{tool};
    h.write.at_ms = now_ms();
    h.kind = kind;
    remember(h);

    const auto why = sandbox_cfg::explain(kind);

    // Logged at every outcome, allow included, for the same reason the broker
    // logs allows: the trail is what makes the boundary teachable. The path is
    // behind logx::body() because it is workspace content.
    AGT_LOG(Tool, Info, "sandbox.handoff", "{} {} ({}) policy={}",
            policy == sandbox_cfg::HandoffPolicy::Refuse ? "REFUSED" : "allowed",
            logx::body(std::string{path}), why,
            sandbox_cfg::to_string(policy));

    switch (policy) {
        case sandbox_cfg::HandoffPolicy::Allow:
            return v;

        case sandbox_cfg::HandoffPolicy::Warn:
            // Proceeds, but says so. The reason rides along so the caller can
            // surface it without re-deriving the explanation.
            v.reason = std::string{"wrote "} + std::string{why};
            return v;

        case sandbox_cfg::HandoffPolicy::Refuse:
            break;
    }

    v.allowed = false;
    // Phrased for the MODEL as much as the user: a refusal that only says "no"
    // gets retried, and a retry loop on a security gate is worse than either
    // outcome. Naming the mechanism and the alternative ends the loop in one
    // turn.
    v.reason =
        "refused: " + std::string{why} + ". The sandbox confines this process, "
        "but a file the host later executes escapes it by being run outside. "
        "Write the content somewhere inert and ask the user to wire it up, or "
        "change Trust handoff in the Sandbox pane.";
    return v;
}

Verdict check(std::string_view path, std::string_view tool) {
    // `--sandbox off` turns this off as well.
    //
    // The gate is not a sandbox WALL (it lives on the tool call, §14) but it is
    // part of the same control, and a user who disabled sandboxing did not opt
    // into one surviving piece of it refusing their writes. "off" has to mean
    // off, or the flag is a lie in the other direction -- the same honesty rule
    // the rest of this subsystem is built on.
    //
    // Checked through requested_mode() rather than is_active(): a host with no
    // usable backend still gets the gate, because that is a host that FAILED to
    // sandbox rather than a user who asked not to be. Those are different
    // answers and conflating them would quietly drop the gate on exactly the
    // machines with the fewest other walls.
    if (sandbox::requested_mode() == sandbox::Mode::Off) return {};

    // Reads the SEALED config, so the policy cannot change under a running
    // turn -- the same guarantee every other wall here has.
    return check_with(path, tool, sandbox::config_snapshot()->handoff);
}

std::vector<sandbox_cfg::TrustHandoff> handoff_feed() {
    return feed().get();
}

void clear_handoff_feed() {
    feed().store({});
}

// ── Detection for the shell path ──────────────────────────────────

namespace {

// Directories the snapshot will not descend into.
//
// Deliberately NOT the masking walk's kNeverWalk list, and the difference is
// the point: that list skips .git because it holds no secrets, while .git is
// one of the most important things to WATCH here (.git/config carries a
// credential helper, .git/hooks runs on the next commit). What is skipped here
// is only the bulk that cannot plausibly contain a trusted path we care about
// -- build output and dependency trees. node_modules is skipped despite
// `node_modules/.bin` being a trusted shape: walking it costs more than the
// detection is worth, and a .bin shim is reached through package.json, which
// IS watched.
constexpr std::string_view kSkipDirs[] = {
    "node_modules", "target", "build", "dist", "out", "__pycache__",
    ".mypy_cache", ".pytest_cache", ".cache", ".next", ".gradle", ".tox",
    "_deps", "CMakeFiles",
};

// Build trees come under many names (build-rel, build-tsan, cmake-build-debug,
// testbuild, fuzzbuild). A name prefix/suffix catches the common spellings and
// a CMakeCache.txt catches the rest. These held ~7000 of the ~7500 entries the
// walk visited on the agentty repo, on every shell call.
// A dir holding CMakeCache.txt is skipped too, once its listing shows it.
[[nodiscard]] bool skip_name(std::string_view name) {
    if (std::ranges::find(kSkipDirs, name) != std::ranges::end(kSkipDirs)) return true;
    return name.starts_with("build") || name.starts_with("cmake-build")
        || name.ends_with("build");
}

// Inside a .git directory, only these subtrees are worth watching.
//
// Measured on the agentty repo: without this, .git contributed 1605 of 1824
// "trusted" paths -- almost all of it the object store, packed refs and logs.
// The ShapeRule for .git is a COMPONENT match, so every blob under
// .git/objects/ reads as trusted, and none of it is: git executes hooks and
// honours config, it does not run its own object database. Watching 1605 inert
// files to catch writes to three is how a detector becomes noise, and noise is
// how a security feature gets ignored.
//
// Everything a host tool actually RUNS from .git lives in one of these.
constexpr std::string_view kGitWatch[] = {"hooks", "info"};

// The files directly in a git dir that git reads as configuration. The rest
// (HEAD, index, packed-refs, COMMIT_EDITMSG, ORIG_HEAD, ...) is state git
// rewrites by renaming a lock file over it, which a read-only bind breaks.
constexpr std::string_view kGitWatchFiles[] = {"config", "config.worktree"};

// Is `p` inside a git dir, and if so is it a part we care about? `is_dir`
// says whether p itself is a directory: a FILE directly in the git dir is
// watched only if it is config, a directory there (objects, logs) is noise
// unless it is hooks/ or info/, and in info/ only attributes is watched.
//
// A submodule's git dir lives at .git/modules/<name>[/modules/<name>...] and
// has the same layout, so the filter applies there too.
[[nodiscard]] bool git_noise(const fs::path& p, bool is_dir = false) {
    auto it = std::find(p.begin(), p.end(), fs::path{".git"});
    if (it == p.end()) return false;
    auto next = std::next(it);
    // Step through modules/<name> pairs to the innermost git dir.
    while (next != p.end() && *next == "modules" && std::next(next) != p.end()) {
        it = std::next(next);
        next = std::next(it);
    }
    if (next == p.end()) return false;                    // the git dir itself
    if (*next == "modules") return false;                // leads to submodule git dirs
    const std::string seg = next->string();
    if (std::next(next) == p.end() && !is_dir)                  // a file in it
        return std::ranges::find(kGitWatchFiles, seg) == std::ranges::end(kGitWatchFiles);
    // info/attributes selects the filter and diff drivers git runs. The rest
    // of info/ (exclude, sparse-checkout) is state git and its tools rewrite.
    if (seg == "info") {
        const auto rest = std::next(next);
        if (rest == p.end()) return false;                      // info/ itself: walk it
        return !(*rest == "attributes" && std::next(rest) == p.end());
    }
    return std::ranges::find(kGitWatch, seg) == std::ranges::end(kGitWatch);
}

// agentty's own data under .agentty (threads, logs, ...). Nothing there gets
// run by a host tool, and ~/.agentty/threads alone can hold thousands of files.
constexpr std::string_view kAgenttyData[] = {"threads", "logs", "cache", "state", "sessions"};

[[nodiscard]] bool agentty_noise(const fs::path& dir) {
    if (dir.parent_path().filename() != ".agentty") return false;
    const std::string name = dir.filename().string();
    return std::ranges::find(kAgenttyData, name) != std::ranges::end(kAgenttyData);
}

// Depth cap. A trusted path matters because a HOST tool finds and runs it, and
// the tools that do that (editors, git, direnv, build drivers) look near the
// root. Eight levels covers every real case while keeping a pathological tree
// from turning a shell call into a full walk.
constexpr int kMaxDepth = 8;

// Entries scanned before we stop. A bound, not a tuning knob: the snapshot runs
// on every shell call, so an unbounded walk would make a big repo pay for the
// gate on each one. Hitting it degrades detection on that call rather than
// stalling the turn -- logged, because silently scanning less is the kind of
// thing that makes a wall quietly stop working.
constexpr std::size_t kMaxEntries = 20000;

// What the walk needs from one directory. Everything in it depends only on
// the entry names and their paths, so it stays valid while the dir's mtime
// does (adding, removing or renaming an entry bumps it). Trusted files are
// still stat'ed fresh on every snapshot.
struct Listing {
    struct Sub {
        std::string name;
        bool link  = false;   // symlinked dir: never descended
        bool noise = false;   // git_noise: never walked
        bool root  = false;   // a new trusted file could appear in it
    };
    struct File { std::string path; sandbox_cfg::TrustKind kind{}; };
    fs::file_time_type mtime{};
    std::size_t entries = 0;
    bool has_cmake_cache = false;
    std::vector<Sub> dirs;
    std::vector<File> trusted;   // trusted-shaped files, git noise dropped
};
using ListingPtr = std::optional<maya::shared<Listing>>;

using ListingCache = std::unordered_map<std::string, maya::shared<Listing>>;
// A lock-free snapshot rather than a guarded map: snapshot_trusted() is
// called from spawn paths (sandbox posture) that carry no lock capability.
// One walk loads the map ONCE, collects the listings it had to (re)build in a
// private batch, and publishes the batch in a single update() at the end, so
// the copy-on-write cost is paid once per walk that found a change, not per
// directory.
maya::snapshot<ListingCache>& listing_cache() {
    static maya::snapshot<ListingCache> c;
    return c;
}
struct ListingWalk {
    maya::shared<ListingCache> seen = listing_cache().load();
    ListingCache               fresh;
    ~ListingWalk();
};
constexpr std::size_t kMaxCachedDirs = 200000;
// A dir changed this recently may change again within the same mtime tick,
// so its listing isn't cached (git's "racy" rule).
constexpr auto kRacyWindow = std::chrono::seconds{2};

[[nodiscard]] bool is_trusted_dir(const std::string& p) {
    sandbox_cfg::TrustKind k{};
    return sandbox_cfg::is_host_trusted(p + "/probe", &k);
}

// The listing of `dir`, from the cache when its mtime hasn't moved. One stat
// on a hit. No lock is held while touching the filesystem.
ListingPtr list_dir(ListingWalk& walk, const fs::path& dir) {
    std::error_code ec;
    const auto mtime = fs::last_write_time(dir, ec);
    if (ec) return std::nullopt;
    std::string key = dir.string();
    ListingPtr hit;
    if (auto it = walk.seen->find(key); it != walk.seen->end()) hit = it->second;
    if (hit && (*hit)->mtime == mtime) return hit;

    Listing built;
    Listing* l = &built;
    l->mtime = mtime;
    fs::directory_iterator it{dir, fs::directory_options::skip_permission_denied, ec};
    if (ec) return std::nullopt;
    while (it != fs::directory_iterator{}) {
        const fs::directory_entry& e = *it;
        ++l->entries;
        std::error_code tec;
        if (e.is_directory(tec)) {
            const fs::path& p = e.path();
            l->dirs.push_back({p.filename().string(), e.is_symlink(tec),
                               git_noise(p, true) || agentty_noise(p),
                               is_trusted_dir(p.string())});
        } else {
            const fs::path& p = e.path();
            if (p.filename() == "CMakeCache.txt") l->has_cmake_cache = true;
            std::string ps = p.string();
            sandbox_cfg::TrustKind kind{};
            if (sandbox_cfg::is_host_trusted(ps, &kind) && !git_noise(p))
                l->trusted.push_back({std::move(ps), kind});
        }
        it.increment(ec);
        if (ec) {
            AGT_LOG(General, Warn, "handoff", "listing {} aborted: {}",
                    key, ec.message());
            return maya::shared<Listing>::make(std::move(built));   // partial: never cached
        }
    }
    auto frozen = maya::shared<Listing>::make(std::move(built));
    if (fs::file_time_type::clock::now() - mtime >= kRacyWindow)
        walk.fresh.insert_or_assign(std::move(key), frozen);
    return frozen;
}

// Publish what this walk built. A destructor so every return path of
// snapshot_trusted() keeps what it learned.
ListingWalk::~ListingWalk() {
    if (fresh.empty()) return;
    (void)listing_cache().update(
        [](const ListingCache& old, const ListingCache& add) {
            ListingCache m;
            if (old.size() + add.size() <= kMaxCachedDirs) m = old;
            for (const auto& [k, v] : add) {
                if (m.size() >= kMaxCachedDirs) m.clear();
                m.insert_or_assign(k, v);
            }
            return m;
        }, std::move(fresh));
}

// What a git config makes git run from the worktree. A pinned .git/config
// protects the pointer; these are the targets it points at.
struct GitConfigTargets {
    std::vector<fs::path> files;     // fsmonitor, filters, textconv, sshCommand, credential helper
    std::vector<fs::path> dirs;      // core.hooksPath (husky, lefthook, .githooks)
    std::vector<fs::path> includes;  // include.path, includeIf.*.path, relative to the config
    std::optional<fs::path> worktree;  // core.worktree, relative to the git dir
};

// Entries a hooksPath dir may hold and still be pinned whole. Above it the
// dir is ordinary source (hooksPath = src), so only hook-named files are
// pinned, and the pane says why.
constexpr std::size_t kMaxPinnedDirFiles = 256;

// How deep includes are followed; git's own limit is 10.
constexpr int kMaxIncludeDepth = 10;

// The names git runs from a hooks dir (githooks(5)).
constexpr std::string_view kGitHookNames[] = {
    "applypatch-msg", "pre-applypatch", "post-applypatch", "pre-commit",
    "pre-merge-commit", "prepare-commit-msg", "commit-msg", "post-commit",
    "pre-rebase", "post-checkout", "post-merge", "pre-push", "pre-receive",
    "update", "proc-receive", "post-receive", "post-update",
    "reference-transaction", "push-to-checkout", "pre-auto-gc", "post-rewrite",
    "sendemail-validate", "fsmonitor-watchman", "p4-changelist",
    "p4-prepare-changelist", "p4-post-changelist", "p4-pre-submit",
    "post-index-change",
};

[[nodiscard]] bool is_git_hook_name(std::string_view name) {
    return std::ranges::find(kGitHookNames, name) != std::ranges::end(kGitHookNames);
}

// GitConfigTargets after includes are followed and paths placed: every path
// absolute, symlinks resolved where they exist. A target may not exist yet.
struct ResolvedTargets {
    std::vector<fs::path> files;
    std::vector<fs::path> dirs;
    std::vector<fs::path> includes;
};

// The targets one repo's config makes git run. `git_dir` is the dir holding
// `config` (.git, or .git/modules/<name>); `worktree`, when known, is where
// relative targets resolve.
[[nodiscard]] ResolvedTargets resolve_git_targets(const fs::path& config, const fs::path& git_dir,
                                                  std::optional<fs::path> worktree = {});

[[nodiscard]] std::string_view trim(std::string_view s) {
    const auto b = s.find_first_not_of(" \t\r");
    if (b == std::string_view::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\r") - b + 1);
}

[[nodiscard]] std::string ascii_lower(std::string_view s) {
    std::string out{s};
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

// A raw value decoded per git-config(1): quotes removed with their spaces
// kept, \n \t \b and \\ \" unescaped, and an unquoted # or ; ending it.
[[nodiscard]] std::string config_value(std::string_view raw) {
    std::string out;
    bool quoted = false;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '\\' && i + 1 < raw.size()) {
            const char e = raw[++i];
            out += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'b' ? '\b' : e;
            continue;
        }
        if (c == '"') { quoted = !quoted; continue; }
        if (!quoted && (c == '#' || c == ';')) break;
        out += c;
    }
    return out;
}

// The program a command value runs: git hands it to the shell, so the first
// shell word, honouring '...' and "..." quoting. A leading `!` (a helper
// written as a shell snippet) is dropped first.
[[nodiscard]] std::string command_program(std::string_view v) {
    v = trim(v);
    if (!v.empty() && v.front() == '!') v = trim(v.substr(1));
    if (v.empty()) return {};
    if (v.front() == '\'' || v.front() == '"') {
        const auto close = v.find(v.front(), 1);
        return std::string{v.substr(1, close == std::string_view::npos ? v.npos : close - 1)};
    }
    return std::string{v.substr(0, v.find_first_of(" \t\n"))};
}

[[nodiscard]] GitConfigTargets git_config_targets(const fs::path& config) {
    GitConfigTargets t;
    std::ifstream in(config, std::ios::binary);
    if (!in) return t;
    std::string section;   // lower-case; the subsection (filter "lfs") is dropped
    std::string line;
    while (std::getline(in, line)) {
        const std::string_view l = trim(line);
        if (l.empty() || l.front() == '#' || l.front() == ';') continue;
        if (l.front() == '[') {
            const std::string_view h = trim(l.substr(1, l.find(']') - 1));
            section = ascii_lower(h.substr(0, h.find_first_of(" \t.\"")));
            continue;
        }
        const auto eq = l.find('=');
        if (eq == std::string_view::npos) continue;   // a bare key is a boolean
        const std::string key = ascii_lower(trim(l.substr(0, eq)));
        const std::string value = config_value(trim(l.substr(eq + 1)));
        if ((section == "include" || section == "includeif") && key == "path") {
            if (const auto v = trim(value); !v.empty()) t.includes.emplace_back(std::string{v});
            continue;
        }
        if (section == "core" && key == "hookspath") {
            if (const auto v = trim(value); !v.empty()) t.dirs.emplace_back(std::string{v});
            continue;
        }
        if (section == "core" && key == "worktree") {
            if (const auto v = trim(value); !v.empty()) t.worktree = fs::path{std::string{v}};
            continue;
        }
        const bool cmd = (section == "core" && (key == "fsmonitor" || key == "sshcommand"))
            || (section == "filter" && (key == "clean" || key == "smudge" || key == "process"))
            || (section == "diff" && key == "textconv")
            || (section == "credential" && key == "helper");
        if (!cmd) continue;
        // A program without a '/' is found on PATH (git-lfs, `store` as
        // git-credential-store, `true` for fsmonitor), never in the worktree.
        const std::string prog = command_program(value);
        if (prog.find('/') != std::string::npos) t.files.emplace_back(prog);
    }
    return t;
}

// `v` as git would find it: ~/ expanded, relative to `from`, symlinks
// resolved as far as the path exists. Callers that need it to exist check.
[[nodiscard]] std::optional<fs::path> resolve_config_path(const fs::path& from,
                                                          const fs::path& v) {
    fs::path p = v;
    const std::string s = v.string();
    if (s.starts_with("~/"))
        if (const char* h = std::getenv("HOME"); h && *h) p = fs::path{h} / s.substr(2);
    std::error_code ec;
    fs::path real = fs::weakly_canonical(p.is_absolute() ? p : from / p, ec);
    if (ec) return std::nullopt;
    return real;
}

ResolvedTargets resolve_git_targets(const fs::path& config, const fs::path& git_dir,
                                    std::optional<fs::path> worktree) {
    // Relative targets resolve against the worktree git runs in: the caller's
    // when it knows it, else the git dir's parent for a main repo, else
    // core.worktree. Without one, only absolute targets can be placed.
    if (!worktree && git_dir.filename() == ".git") worktree = git_dir.parent_path();

    ResolvedTargets out;
    GitConfigTargets t;
    std::vector<fs::path> todo{config};
    std::vector<fs::path> read_files;
    for (int depth = 0; !todo.empty() && depth <= kMaxIncludeDepth; ++depth) {
        std::vector<fs::path> next;
        for (const auto& file : todo) {
            if (std::ranges::find(read_files, file) != read_files.end()) continue;
            read_files.push_back(file);
            GitConfigTargets one = git_config_targets(file);
            if (!worktree && one.worktree)
                if (auto w = resolve_config_path(git_dir, *one.worktree)) worktree = *w;
            for (auto& f : one.files) t.files.push_back(std::move(f));
            for (auto& d : one.dirs) t.dirs.push_back(std::move(d));
            for (const auto& inc : one.includes) {
                const auto real = resolve_config_path(file.parent_path(), inc);
                std::error_code iec;
                if (!real || !fs::is_regular_file(*real, iec)) continue;
                out.includes.push_back(*real);
                next.push_back(*real);
            }
        }
        todo = std::move(next);
    }
    const auto place = [&](const fs::path& v) -> std::optional<fs::path> {
        if (!worktree && !v.is_absolute()) return std::nullopt;
        return resolve_config_path(worktree.value_or(fs::path{}), v);
    };
    for (const auto& f : t.files) if (auto r = place(f)) out.files.push_back(*r);
    for (const auto& d : t.dirs)  if (auto r = place(d)) out.dirs.push_back(*r);
    return out;
}

// The git dir of the repo whose worktree holds `dir`, nearest first, and that
// worktree. A `.git` FILE (a submodule or linked worktree) points at the git
// dir with `gitdir:`; its worktree is the dir holding the file, which git
// itself uses rather than any core.worktree.
struct NearestRepo { fs::path git_dir; fs::path worktree; };

[[nodiscard]] std::optional<NearestRepo> nearest_repo(fs::path dir) {
    // Bounded: a write outside any repo would otherwise stat up to `/`.
    // Deeper than any real source tree; the snapshot's own scan stops at 3.
    constexpr int kMaxUp = 64;
    for (int up = 0; !dir.empty() && up < kMaxUp; dir = dir.parent_path(), ++up) {
        std::error_code ec;
        const fs::path dot = dir / ".git";
        if (fs::is_directory(dot, ec)) return NearestRepo{dot, dir};
        if (fs::is_regular_file(dot, ec)) {
            std::ifstream in(dot, std::ios::binary);
            std::string line;
            if (std::getline(in, line) && line.starts_with("gitdir:"))
                if (auto g = resolve_config_path(
                        dir, std::string{trim(std::string_view{line}.substr(7))}))
                    return NearestRepo{*g, dir};
            return std::nullopt;
        }
        if (dir == dir.root_path()) break;
    }
    return std::nullopt;
}

// The configs git reads for a git dir: its own, and for a linked worktree
// (.git/worktrees/<n>) the main repo's, which `commondir` names.
[[nodiscard]] std::vector<fs::path> repo_configs(const fs::path& git_dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    fs::path common = git_dir;
    if (std::ifstream in{git_dir / "commondir", std::ios::binary}; in) {
        std::string line;
        if (std::getline(in, line))
            if (auto c = resolve_config_path(git_dir, std::string{trim(line)})) common = *c;
    }
    for (const fs::path& dir : {common, git_dir})
        for (const char* name : {"config", "config.worktree"})
            if (const fs::path f = dir / name;
                fs::is_regular_file(f, ec) && std::ranges::find(out, f) == out.end())
                out.push_back(f);
    return out;
}

bool is_git_config_target(const fs::path& path) {
    std::error_code ec;
    const fs::path target = fs::weakly_canonical(path, ec);
    if (ec || target.empty()) return false;
    const auto repo = nearest_repo(target.parent_path());
    if (!repo) return false;
    for (const fs::path& cfg : repo_configs(repo->git_dir)) {
        const ResolvedTargets t = resolve_git_targets(cfg, repo->git_dir, repo->worktree);
        if (std::ranges::find(t.files, target) != t.files.end()) return true;
        if (std::ranges::find(t.includes, target) != t.includes.end()) return true;
        // A hooks dir: every file in it, or only hook names when the dir is
        // ordinary source the snapshot would not pin whole either.
        const fs::path parent = target.parent_path();
        if (std::ranges::find(t.dirs, parent) == t.dirs.end()) continue;
        std::size_t entries = 0;
        for (fs::directory_iterator it{parent, fs::directory_options::skip_permission_denied, ec};
             !ec && it != fs::directory_iterator{} && entries <= kMaxPinnedDirFiles;
             it.increment(ec))
            ++entries;
        if (entries <= kMaxPinnedDirFiles || is_git_hook_name(target.filename().string()))
            return true;
    }
    return false;
}

// Directories that are small, entirely trusted and worth guaranteeing, so the
// snapshot walks them first and uncapped (see snapshot_trusted). Named here
// rather than derived from the rule table because the table is match SHAPES,
// not locations. is_host_trusted() stays the one authority on whether a path
// counts -- this list only decides walk ORDER.
constexpr std::string_view kPriorityDirs[] = {
    ".git", ".vscode", ".claude", ".agentty", ".cursor", ".idea",
    ".direnv", ".codex", ".gemini",
};

// How deep the shallow scan looks: deep enough to find .vscode in a
// sub-package, and every real layout keeps these near the root.
constexpr int kShallowDepth = 3;

struct ShallowHit { fs::path path; bool link = false; };

// Every priority-named dir within kShallowDepth of `base`, by the walk's skip
// rules. The snapshot and review() both find git dirs through this, so the
// dirs one anchors and the other compares cannot drift apart.
[[nodiscard]] std::vector<ShallowHit> shallow_scan(ListingWalk& walk, const fs::path& base) {
    std::vector<ShallowHit> out;
    std::vector<std::pair<fs::path, int>> stack{{base, 0}};
    std::size_t seen = 0;
    while (!stack.empty() && seen <= kMaxEntries) {
        auto [dir, depth] = std::move(stack.back());
        stack.pop_back();
        const auto lp = list_dir(walk, dir);
        if (!lp) continue;
        const Listing* l = &**lp;
        if (depth > 0 && l->has_cmake_cache) continue;
        seen += l->entries;
        for (const auto& s : l->dirs) {
            fs::path p = dir / s.name;
            if (std::ranges::find(kPriorityDirs, s.name) != std::ranges::end(kPriorityDirs)) {
                out.push_back({std::move(p), s.link});
                continue;
            }
            if (depth < kShallowDepth && !s.link && !skip_name(s.name))
                stack.push_back({std::move(p), depth + 1});
        }
    }
    return out;
}

// Every `.git` dir the shallow scan finds under `root`, sorted. review() runs
// it again after a command that may have moved them; the listing cache is
// keyed by mtime, and a rename bumps the parent's.
[[nodiscard]] std::vector<std::string> scan_git_dirs(const fs::path& root) {
    ListingWalk walk;
    std::vector<std::string> out;
    for (const auto& h : shallow_scan(walk, root))
        if (h.path.filename() == ".git" && !h.link) out.push_back(h.path.string());
    std::ranges::sort(out);
    return out;
}

}  // namespace

TrustedSnapshot snapshot_trusted(std::string_view root) {
    TrustedSnapshot snap;
    if (root.empty()) return snap;
    ListingWalk walk;

    // Allow means "do not gate", so there is nothing to report and no reason to
    // pay for the stats. Same early-out as check(), including --sandbox off.
    if (sandbox::requested_mode() == sandbox::Mode::Off) return snap;
    const auto cfg = sandbox::config_snapshot();
    if (cfg->handoff == sandbox_cfg::HandoffPolicy::Allow) return snap;

    std::error_code ec;
    fs::path base{root};
    if (!fs::is_directory(base, ec)) return snap;

    std::size_t seen = 0;
    bool truncated = false;
    std::unordered_set<std::string> already;

    // Remember the directories we walked, so review() can re-scan them and
    // notice a trusted file that did not exist at snapshot time.
    //
    // Without this the gate only catches MODIFICATIONS: `echo x >
    // .git/hooks/post-commit` creating a brand-new hook was invisible, which
    // is the more dangerous half -- a fresh hook is the classic escape, and an
    // empty repo has no hooks to modify. Verified live before fixing.
    snap.roots.push_back(base.string());

    // Record one trusted file. Shared by the priority pass below and the
    // general walk, so the two cannot disagree about what an entry holds.
    auto record = [&snap, &already](const std::string& p, sandbox_cfg::TrustKind kind) {
        if (!already.insert(p).second) return;
        TrustedSnapshot::Entry entry;
        entry.path    = p;
        entry.kind    = kind;
        entry.existed = true;
        std::error_code sec;
        entry.size    = fs::file_size(p, sec);
        if (sec) entry.size = 0;
        std::error_code tec;
        const auto t = fs::last_write_time(p, tec);
        entry.mtime_ns = tec ? 0
            : std::chrono::duration_cast<std::chrono::nanoseconds>(
                  t.time_since_epoch()).count();
        snap.entries.push_back(std::move(entry));
    };

    // ── Priority pass: the trusted DIRECTORIES, before the cap can bite ──
    //
    // The general walk below is capped at kMaxEntries, and on a tree bigger
    // than that it stops early. recursive_directory_iterator is unordered, so
    // WHICH trusted paths made it inside the budget was arbitrary and could
    // differ between runs on one repo -- `.git/hooks/post-commit`, the escape
    // the whole gate was built for, had no better odds than a vendored
    // Makefile 40k entries deep. A wall whose coverage is decided by walk
    // order is not a wall (#78).
    //
    // So the ShapeMatch::Component rules get walked FIRST and uncapped. Those
    // are whole directories the host later trusts -- .git, .vscode, .claude,
    // .agentty, .cursor, .idea, .direnv -- and they are small: a few dozen
    // files each, versus the tens of thousands a monorepo's source tree runs
    // to. Walking them in full costs almost nothing and makes the high-risk
    // set unconditional.
    //
    // Truncation still exists and can still drop a deep Basename match (a
    // vendored Makefile). That is the right thing to lose: it is bounded
    // work, and the directories that carry every exploit in the Pillar series
    // are covered before the budget is spent. The directories are kPriorityDirs.
    //
    // The same idea for paths that only exist under $HOME and need two
    // components to name: run at login or found on PATH. Small, and the
    // most dangerous thing to lose to the cap when the workspace is ~.
    static constexpr std::string_view kPriorityHomeDirs[] = {
        ".config/fish", ".config/environment.d", ".config/autostart",
        ".config/systemd", ".config/hypr", ".config/sway", ".config/i3",
        ".config/plasma-workspace", ".local/bin",
    };
    // Directories the priority pass walked in full. The general walk below
    // skips them, or it records their roots twice and walks .git again.
    std::unordered_set<std::string> walked;
    {
        // Find the priority directories with a SHALLOW scan (they live at or
        // near the workspace root in every real layout), then walk each fully.
        std::vector<fs::path> targets;
        for (auto rel : kPriorityHomeDirs) {
            std::error_code hec;
            fs::path p = base / rel;
            if (fs::is_directory(p, hec) && !fs::is_symlink(fs::symlink_status(p, hec)))
                targets.push_back(std::move(p));
        }
        for (auto& h : shallow_scan(walk, base)) {
            if (h.path.filename() == ".git" && !h.link) snap.git_dirs.push_back(h.path.string());
            targets.push_back(std::move(h.path));
        }
        std::ranges::sort(snap.git_dirs);
        // Uncapped. Pruning .git/objects and friends is what keeps it cheap.
        for (const auto& t : targets) {
            std::vector<fs::path> st{t};
            while (!st.empty()) {
                const fs::path d = std::move(st.back());
                st.pop_back();
                const auto lp = list_dir(walk, d);
                if (!lp) continue;
                const Listing* l = &**lp;
                for (const auto& f : l->trusted) record(f.path, f.kind);
                for (const auto& s : l->dirs) {
                    if (s.noise) continue;
                    fs::path p = d / s.name;
                    if (s.root) snap.roots.push_back(p.string());
                    if (!s.link) st.push_back(std::move(p));
                }
            }
        }
        if (!targets.empty())
            AGT_LOG(General, Debug, "handoff",
                    "priority pass covered {} trusted dir(s), {} entry(s)",
                    targets.size(), snap.entries.size());
        for (const auto& t : targets) {
            walked.insert(t.string());
            snap.roots.push_back(t.string());   // a new file directly inside
        }
    }
    const std::size_t priority_count = snap.entries.size();

    // General walk, capped. Visits whole directories, so the cap is checked
    // per directory.
    std::vector<std::pair<fs::path, int>> stack{{base, 0}};
    while (!stack.empty()) {
        auto [dir, depth] = std::move(stack.back());
        stack.pop_back();
        const auto lp = list_dir(walk, dir);
        if (!lp) continue;
        const Listing* l = &**lp;
        if (depth > 0 && l->has_cmake_cache) continue;
        seen += l->entries;
        if (seen > kMaxEntries) { truncated = true; break; }
        for (const auto& f : l->trusted) record(f.path, f.kind);
        for (const auto& s : l->dirs) {
            if (skip_name(s.name) || s.noise) continue;
            fs::path p = dir / s.name;
            std::string ps = p.string();
            if (walked.contains(ps)) continue;
            // A trusted-shaped dir (.git/hooks, .vscode) is where a NEW
            // trusted file would appear, so review() re-lists it.
            if (s.root) snap.roots.push_back(ps);
            if (depth < kMaxDepth && !s.link) stack.push_back({std::move(p), depth + 1});
        }
    }

    last_truncated().store(truncated);

    // Targets named by each repo's .git/config, pinned like the config itself.
    // A relative value resolves against the worktree, where git runs hooks and
    // commands. Only existing paths strictly inside the workspace count, and
    // they are re-rooted onto `base` so the spelling matches the sandbox's binds.
    {
        std::error_code wec;
        const fs::path ws = fs::canonical(base, wec);
        // Every config git reads for a repo in the workspace: the main one,
        // config.worktree, and each submodule's under .git/modules.
        std::vector<std::pair<fs::path, fs::path>> configs;   // {file, git dir}
        for (const auto& e : snap.entries) {
            const fs::path p{e.path};
            if (p.filename() != "config" && p.filename() != "config.worktree") continue;
            const fs::path dir = p.parent_path();
            const bool main = dir.filename() == ".git";
            const bool sub = dir.parent_path().filename() == "modules"
                && std::ranges::find(dir, fs::path{".git"}) != dir.end();
            if (main || sub) configs.emplace_back(p, dir);
        }
        // Inside the workspace, re-rooted onto `base`. `allow_git` admits paths
        // under a git dir, which an include may legitimately be.
        const auto in_workspace = [&](const fs::path& real, bool allow_git)
                -> std::optional<fs::path> {
            if (wec || real == ws) return std::nullopt;
            const fs::path rel = real.lexically_relative(ws);
            if (rel.empty() || *rel.begin() == "..") return std::nullopt;
            if (!allow_git && std::ranges::find(rel, fs::path{".git"}) != rel.end())
                return std::nullopt;
            return base / rel;
        };
        bool large_dir = false;
        for (const auto& [file, git_dir] : configs) {
            const ResolvedTargets t = resolve_git_targets(file, git_dir);
            for (const auto& inc : t.includes)
                if (auto p = in_workspace(inc, /*allow_git=*/true))
                    record(p->string(), sandbox_cfg::TrustKind::GitConfig);
            for (const auto& f : t.files) {
                std::error_code fec;
                if (auto p = in_workspace(f, false); p && fs::is_regular_file(*p, fec))
                    record(p->string(), sandbox_cfg::TrustKind::GitConfig);
            }
            for (const auto& d : t.dirs) {
                std::error_code dec;
                const auto p = in_workspace(d, false);
                if (!p || !fs::is_directory(*p, dec)) continue;
                if (std::ranges::find(snap.pinned_dirs, p->string()) != snap.pinned_dirs.end())
                    continue;
                // Count first: a hooksPath at a big source dir (src) must not
                // make it read-only. Then only the names git runs are pinned.
                std::vector<fs::path> files;
                std::size_t entries = 0;
                fs::directory_iterator it{*p, fs::directory_options::skip_permission_denied, dec};
                for (; !dec && it != fs::directory_iterator{}; it.increment(dec)) {
                    ++entries;
                    std::error_code tec;
                    if (it->is_regular_file(tec) && !tec) files.push_back(it->path());
                }
                const bool whole = entries <= kMaxPinnedDirFiles;
                if (whole) snap.pinned_dirs.push_back(p->string());
                else large_dir = true;
                for (const auto& f : files)
                    if (whole || is_git_hook_name(f.filename().string()))
                        record(f.string(), sandbox_cfg::TrustKind::GitConfig);
            }
        }
        last_large_hook_dir().store(large_dir);
        if (large_dir)
            AGT_LOG(General, Warn, "handoff",
                    "a core.hooksPath dir under {} holds more than {} entries -- only "
                    "its hook-named files are pinned, so a new hook there is not "
                    "prevented", std::string{root}, kMaxPinnedDirFiles);
    }
    if (truncated)
        AGT_LOG(General, Warn, "handoff",
                "trusted snapshot truncated at {} entries under {} -- a write "
                "below the cut would not be reported (the {} high-risk "
                "trusted dir entries were covered first)",
                kMaxEntries, std::string{root}, priority_count);

    AGT_LOG(General, Debug, "handoff", "snapshot {} trusted path(s) under {}",
            snap.entries.size(), std::string{root});
    return snap;
}

bool last_snapshot_truncated() noexcept { return *last_truncated().load(); }

bool last_snapshot_large_hook_dir() noexcept { return *last_large_hook_dir().load(); }

namespace {

// A hook git would run: any file in hooks/ that is not a shipped sample.
[[nodiscard]] bool has_live_hook(const fs::path& git_dir) {
    std::error_code ec;
    for (fs::directory_iterator it{git_dir / "hooks", ec}; !ec && it != fs::directory_iterator{};
         it.increment(ec)) {
        std::error_code tec;
        if (it->is_regular_file(tec) && !it->path().filename().string().ends_with(".sample"))
            return true;
    }
    return false;
}

}  // namespace

std::size_t review_trusted(const TrustedSnapshot& before, std::string_view tool) {
    if (before.empty()) return 0;

    std::size_t recorded = 0;

    // A new git dir has no protection until the next spawn, so one that git
    // would act on is reported here. A git dir moved or deleted needs no
    // report of its own: its config is an entry, reported below as deleted.
    if (!before.roots.empty()) {
        const auto& was = before.git_dirs;
        const auto now = scan_git_dirs(before.roots.front());
        const auto report = [&](const std::string& p, const char* what) {
            sandbox_cfg::TrustHandoff h;
            h.kind = sandbox_cfg::TrustKind::GitConfig;
            h.write.path = p;
            h.write.command = std::string{tool};
            h.write.at_ms = now_ms();
            h.write.created = std::string_view{what} == "created";
            remember(h);
            ++recorded;
            AGT_LOG(General, Warn, "handoff", "{} a git dir outside the gate: {}", what, p);
        };
        for (const auto& g : now) {
            if (std::ranges::binary_search(was, g)) continue;
            std::error_code cec;
            const fs::path cfg = fs::path{g} / "config";
            const bool targets = fs::is_regular_file(cfg, cec)
                && [&] {
                       const auto t = resolve_git_targets(cfg, g);
                       return !t.files.empty() || !t.dirs.empty();
                   }();
            if (has_live_hook(g) || targets) report(g, "created");
        }
    }
    for (const auto& e : before.entries) {
        std::error_code ec;
        const bool exists = fs::is_regular_file(e.path, ec);

        const char* what = nullptr;
        if (!exists) {
            what = "deleted";
        } else {
            std::error_code sec, tec;
            const auto sz = fs::file_size(e.path, sec);
            const auto t = fs::last_write_time(e.path, tec);
            const auto ns = tec ? std::int64_t{0}
                                : std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      t.time_since_epoch())
                                      .count();
            // Size OR mtime: a same-length rewrite is the case a size check
            // alone misses, and flipping one character in a hook script is
            // exactly that.
            if ((!sec && sz != e.size) || ns != e.mtime_ns) what = "modified";
        }
        if (!what) continue;

        sandbox_cfg::TrustHandoff h;
        h.kind = e.kind;
        h.write.path = e.path;
        // The command, not the tool name: "shell" tells a user nothing, and
        // WriteRecord exists to answer "who asked". This is the one place the
        // opaque string is genuinely useful -- not for deciding, which is why
        // parsing it was rejected, but for EXPLAINING after the fact.
        h.write.command = std::string{tool};
        h.write.at_ms = now_ms();
        h.write.created = !e.existed;
        remember(h);
        ++recorded;

        AGT_LOG(General, Warn, "handoff",
                "{} a host-trusted path outside the gate: {} ({})",
                what, e.path, sandbox_cfg::explain(e.kind));
    }

    // Second pass: a trusted file that did not exist when we snapshotted.
    //
    // `entries` can only report on paths it already saw, so without this the
    // gate catches an overwritten hook and misses a brand-new one -- the
    // easier and more dangerous move. Bounded the same way the snapshot is:
    // only the directories it actually walked, non-recursively, so this costs
    // one readdir per trusted-shaped directory rather than another tree walk.
    std::vector<std::string> known;
    known.reserve(before.entries.size());
    for (const auto& e : before.entries) known.push_back(e.path);
    std::ranges::sort(known);

    // A pinned hook dir: every new file is a hook, so no shape test applies.
    for (const auto& dir : before.pinned_dirs) {
        std::error_code dec;
        fs::directory_iterator dit{dir, fs::directory_options::skip_permission_denied, dec};
        for (; !dec && dit != fs::directory_iterator{}; dit.increment(dec)) {
            std::error_code fec;
            if (!dit->is_regular_file(fec) || fec) continue;
            const std::string p = dit->path().string();
            if (std::ranges::binary_search(known, p)) continue;
            sandbox_cfg::TrustHandoff h;
            h.kind = sandbox_cfg::TrustKind::GitConfig;
            h.write.path = p;
            h.write.command = std::string{tool};
            h.write.at_ms = now_ms();
            h.write.created = true;
            remember(h);
            ++recorded;
            AGT_LOG(General, Warn, "handoff",
                    "created a file in a git hook dir outside the gate: {}", p);
        }
    }

    for (const auto& dir : before.roots) {
        std::error_code dec;
        fs::directory_iterator dit{
            dir, fs::directory_options::skip_permission_denied, dec};
        if (dec) continue;
        for (; dit != fs::directory_iterator{}; ) {
            std::error_code fec;
            if (dit->is_regular_file(fec) && !fec) {
                const std::string p = dit->path().string();
                if (!std::ranges::binary_search(known, p)) {
                    sandbox_cfg::TrustKind kind{};
                    if (sandbox_cfg::is_host_trusted(p, &kind) && !git_noise(dit->path())) {
                        sandbox_cfg::TrustHandoff h;
                        h.kind = kind;
                        h.write.path = p;
                        h.write.command = std::string{tool};
                        h.write.at_ms = now_ms();
                        h.write.created = true;
                        remember(h);
                        ++recorded;

                        AGT_LOG(General, Warn, "handoff",
                                "created a host-trusted path outside the gate: {} ({})",
                                p, sandbox_cfg::explain(kind));
                    }
                }
            }

            std::error_code iec;
            dit.increment(iec);
            if (iec) {
                AGT_LOG(General, Warn, "handoff",
                        "trusted review walk aborted under {}: {}",
                        dir, iec.message());
                break;
            }
        }
    }
    return recorded;
}

}  // namespace agentty::tools::util::handoff
