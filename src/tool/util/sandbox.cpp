#include <maya/core/target.hpp>
#include "agentty/tool/util/sandbox.hpp"

#include "agentty/tool/util/fs_helpers.hpp"
#include "agentty/io/shared_file.hpp"   // write_atomic: the identity file
#include "agentty/tool/util/subprocess.hpp"   // host_git_identity
#include "agentty/util/home_dir.hpp"   // git_global_stamp
#include "agentty/util/logx.hpp"   // AGT_LOG — the sealed-policy refusal
#include "agentty/util/user_root.hpp"   // trust_roots: own config read-only
#include "agentty/tool/util/sandbox_broker.hpp"   // record: file denials in the blocked feed

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>   // shared_ptr — the config snapshot
#include <optional>
#include <string_view>
#if MAYA_OS_LINUX
#include <fcntl.h>    // pipe2, open: spawn_long_lived's pipes
#include <unistd.h>
#endif

#include <maya/runtime.hpp>
#include <set>
#include <string>
#include <vector>

// The policy type is a PRODUCT concept, not a claybin one: a build without
// claybin still has a sandbox posture, it just cannot enforce most of it. So
// this include is unconditional and the enforcement is what is guarded --
// otherwise "what is my policy" would be a question only some builds could
// answer, and the settings pane has to work in both.
#include "agentty/domain/sandbox_config.hpp"

// claybin is a required submodule, so the only question left is PLATFORM: it
// compiles a plan anywhere, and can APPLY one on Linux (namespaces, landlock,
// seccomp, cgroup2) and on macOS (seatbelt, rlimits). Windows compiles the
// policy and refuses to apply it, so the include stays out there.
#if MAYA_OS_LINUX || MAYA_OS_APPLE || MAYA_OS_WINDOWS
#include "agentty/tool/util/sandbox_claybin.hpp"
#include "agentty/tool/util/handoff_gate.hpp"   // snapshot_trusted: ONE path table
#endif

namespace agentty::tools::util::sandbox {

namespace fs = std::filesystem;

// Escape a path for safe interpolation into the macOS sandbox-exec (SBPL)
// profile string. The workspace root is user/attacker-influenceable (via
// --workspace / cwd) and macOS/APFS permits ", \, and control chars in
// directory names — interpolating such a path raw would terminate the
// (subpath "…") string early and let the remainder be reparsed as SBPL
// (profile rejection at best, an injected (allow …) clause at worst). Escaping
// \ and " makes the literal inert; a control char can't be represented in an
// SBPL string at all, so such a path yields empty → the caller omits the
// clause and fails closed. Exported (not anon-namespace) so it's unit-tested.
std::string sbpl_escape(std::string_view path) {
    std::string out;
    out.reserve(path.size() + 8);
    for (char c : path) {
        if (c == '\0' || c == '\n' || c == '\r' || c == '\t')
            return {};                       // uncleanable → fail closed
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

namespace {

// Single-process state. Set by init() at startup, read by every bash
// call afterwards. Atomics aren't strictly needed (set once, never
// flipped after main has handed off to maya), but they cost nothing
// and document the read-many lifecycle.
std::atomic<Mode>    g_mode{Mode::Auto};
std::atomic<Backend> g_backend{Backend::None};

// Which Linux backend to prefer when both could work.
//
// claybin is the default. The fallback below means this is a PREFERENCE and
// not a requirement -- a host that cannot build claybin's walls still gets
// bwrap, never None.
//
// It used to be bwrap, on the reasoning that a decade of upstream hardening
// beat days and that an upgrade should not silently move anyone's boundary.
// The measurement retired that argument: agentty's bwrap path emits no
// --seccomp, no cgroup limits and no landlock, so against claybin it is
// strictly weaker on syscall.filter (none vs strong), all three resource caps
// (none vs strong), filesystem.exec (binds vs landlock) and brokering (none vs

// The user's saved sandbox policy. Written ONCE, at startup, then frozen.
//
// Sealed rather than republishable, and that seal is the pane's security
// model -- see set_config() for the full reasoning. The short version: a
// boundary that can move mid-session only ever moves usefully in the
// weakening direction, the weakening is invisible and cannot be undone, and
// it makes the sandbox unauditable because two commands in one session get
// two different walls.
//
// This is the ONLY setting in agentty that behaves this way. Everything else
// in the settings store is live by design; a theme is judged by looking at
// it. The sandbox is judged by reading what it promises, so it trades
// immediacy for a property nothing else here needs.
//
// Still a published shared_ptr-to-const rather than a plain global: written
// once but READ from tool worker threads and the UI thread at the same time,
// and this is how a reader gets a coherent view without a lock.
//
// Defaults reproduce today's behaviour, so a user who never opens the pane
// gets exactly the sandbox they already had.
maya::published<const sandbox_cfg::Config> g_cfg;

// Has the policy been sealed? Set by the first set_config() call.
//
// A plain atomic bool rather than "g_cfg != nullptr", because the two are
// not the same question: sealing must latch even if someone seals with a
// default-constructed config, and conflating them would let a second call
// through whenever the first happened to publish something falsy.
std::atomic<bool> g_cfg_sealed{false};

// What init() learned compiling the SHIPPED posture on this host: why it was
// refused (empty when it compiled), and which load-bearing walls came out
// below strong. Read by describe_state() so the banner reports this host, not
// "a backend exists". Published values, not locks (the concurrency banlist).
maya::published<const std::string> g_posture_error;
maya::published<const std::vector<DegradedWall>> g_posture_degraded;
maya::published<const std::string> g_degraded_reason;

// Only the macOS sandbox-exec backend needs the "can we run this binary?"
// probe: Linux asks bwrap for a real namespace instead (see below), and the
// Windows/unsupported branch just hard-codes Backend::None.
#if MAYA_OS_APPLE

// One-shot probe: try to spawn `<exe> --version` and observe the
// outcome. Replaces a fragile PATH walk with the actual semantic
// check we care about ("can we run this binary?"). The result is
// thrown away — exit code 0 just means the binary exists and starts.
[[nodiscard]] bool can_invoke(const char* exe) {
    SubprocessOptions opts;
    opts.command = SubprocessOptions::Argv{{exe, "--version"}};
    opts.timeout = std::chrono::seconds{2};
    opts.max_bytes = 4096;
    auto r = Subprocess::run(std::move(opts));
    return r.started && r.exit_code == 0;
}

#endif // MAYA_OS_APPLE

#if MAYA_OS_LINUX

// A GENUINE minimal-sandbox probe. `bwrap --version` (the old check via
// can_invoke) only proved the BINARY EXISTS — it never creates a namespace, so
// it passed on hosts where bwrap is installed but unprivileged user namespaces
// are BLOCKED: Ubuntu 24.04's AppArmor `userns` restriction, RHEL/hardened
// `kernel.unprivileged_userns_clone=0` or `user.max_user_namespaces=0`, and
// many container/CI hosts. There every REAL command then died with
//     bwrap: setting up uid map: Permission denied
// while agentty happily reported "sandbox: active (bwrap)" — GitHub issue #21.
//
// So we run the SAME namespace unshares a real command uses against a trivial
// /bin/true and require it to actually start AND exit 0. This makes the probe
// a faithful predictor of run_wrapped(): if it can't build the namespace here,
// it can't build it for the shell either, and we report no backend so Auto
// degrades to unsandboxed and On surfaces a clear, actionable error instead of
// letting every command fail.

[[nodiscard]] Backend probe() {
    // One Linux engine. available() is a faithful predictor of spawn() --
    // probe_host() forks and attempts the uid_map write rather than reading
    // sysctls -- so "usable" means it will work, not that the kernel
    // advertises the feature.
    //
    // Its floor is landlock OR seccomp, neither of which needs a user
    // namespace. That is what makes one engine enough: the Ubuntu 24.04 case
    // that motivated a second backend (an AppArmor profile denying the
    // uid_map write, which killed bwrap outright) still gets the filesystem
    // boundary and the syscall filter here. The wall report says per
    // capability what a given host did not reach, so a reduced posture is
    // visible rather than silent.
#if MAYA_OS_LINUX
    if (claybin_backend::available()) return Backend::Claybin;
#endif
    return Backend::None;
}

// Build the bwrap argv prefix. Workspace gets read-write bound to
// itself; system dirs are bound read-only so the shell can find
// /bin/sh, libc, /etc/resolv.conf, etc.; /tmp is a fresh tmpfs (no
// leakage into / from the host /tmp); /proc and /dev are minimal;
// network namespace is shared (so `git push` / `npm install` / `curl`
// keep working — sandboxing those would break legitimate agent flows
// users expect to work).
//
// The intent is "bash can do anything WITHIN your workspace, plus
// network, plus read system libs — but it cannot mutate /etc,
// /home/<other-projects>, ~/.ssh, /opt, etc." That's the threat model
// we're actually defending against: a compromised or sloppy model
// running `rm -rf ~` or `cat ~/.ssh/id_rsa` after one user approval.
//
// `--die-with-parent` ensures the sandbox dies if agentty dies — no
// detached zombies. `--unshare-pid` gives the child a clean PID
// namespace so kills work cleanly. `--new-session` so the child can't
// steal the controlling tty.
// ---------------------------------------------------------------------------
// THE READ SET.
//
// Named lists rather than an inline wall of --ro-bind-try, because this is the
// security boundary and it should be readable as one. Each entry is here for a
// stated reason; anything not listed is NOT readable from inside the sandbox.
//
// Kept as data rather than open-coded because a second backend once restated
// this set by hand and got it wrong -- granting read on "/", which handed
// ~/.ssh, ~/.aws and ~/.config to any approved `bash` call that also had the
// network. Read + exfiltrate, exactly what the sandbox exists to close, and
// invisible because it still reported "sandbox: active". One list, so a future
// backend extends it instead of paraphrasing it.
//
// Visible to BOTH platform arms, because the claybin backend consumes them on
// Linux and on macOS alike. That is the point of the list: one boundary
// definition, extended rather than paraphrased, however many backends read it.
#endif  // MAYA_OS_LINUX (reopened below)

#if MAYA_OS_LINUX || MAYA_OS_APPLE

// System roots: the toolchain and shared libraries. Read-only, and no /etc —
// see kEtcReadable.
constexpr const char* kSystemReadRoots[] = {
    "/usr", "/bin", "/lib", "/lib64", "/sbin", "/opt",
#if MAYA_OS_APPLE
    // darwin's toolchain is NOT under /usr: /usr/bin/git, cc, python3, make
    // are xcrun shims that dlopen libxcrun from the active developer dir
    // and re-exec the real tool there. Without these every one of them died
    // with "xcrun: error: unable to load libxcrun" -- under the default
    // posture, since the sandbox was first turned on for macOS. The
    // developer dir is resolved at posture time (xcode-select can point it
    // anywhere); these are the two fixed places it lives.
    "/System", "/Library/Developer/CommandLineTools",
    // the whole bundle, not just Contents/Developer: xcrun loads
    // SharedFrameworks and SystemFrameworks next to it.
    "/Applications/Xcode.app/Contents",
    "/private/var/select",
    // localtime(3): every `date`, git log and python datetime.
    "/private/var/db/timezone",
#endif
};

// /etc: the specific files a shell + toolchain + name resolution need, NOT the
// tree. Binding all of /etc exposed host secrets (/etc/shadow where readable,
// krb5 keytabs, corporate config) to a command that also has the network.
constexpr const char* kEtcReadable[] = {
    "/etc/resolv.conf",   // DNS
    "/etc/hosts",
    "/etc/nsswitch.conf", // NSS resolution order
    "/etc/host.conf",
    "/etc/passwd",        // uid->name (git, shells)
    "/etc/group",
    "/etc/localtime",     // timestamps
    "/etc/ssl",           // TLS trust store (curl/git over https)
    "/etc/pki",           // RHEL/Fedora trust store
    "/etc/ca-certificates",
    "/etc/ca-certificates.conf",
    "/etc/gitconfig",     // system git config
    "/etc/profile",
    "/etc/alternatives",  // Debian toolchain symlinks
};

// User-local toolchains (GitHub issue #21). Many toolchains install OUTSIDE
// /usr — webinstall.dev drops go/gofmt/node under ~/.local/opt and links them
// into ~/.local/bin; rustup/cargo, go, nvm, pyenv, rbenv, asdf, bun, deno and
// sdkman all live under $HOME. Without these an "approved" bash call could not
// find the very tools the user asked the agent to run.
//
// Deliberately EXCLUDES broad dirs that mix in secrets/app-data: ~/.local/share
// (app data), ~/.npm (may cache an _auth token), ~/.config (creds),
// ~/.local/state (logs/history) — and of course ~/.ssh and ~/.aws.
constexpr const char* kHomeToolSubdirs[] = {
    "/.local/bin", "/.local/opt", "/.local/lib", // webinstall.dev etc.
    // webinstall puts some packages under ~/.local/xbin rather than bin --
    // same tool, different directory, and a tool the agent cannot find is
    // indistinguishable from a broken sandbox from the user's side.
    "/.local/xbin",
    "/.cargo/bin", "/.rustup",   // Rust (bin only from .cargo)
    "/go/bin", "/.go",           // Go (GOPATH bin + webinstall)
    "/.nvm",                     // Node version manager
    "/.pyenv", "/.rbenv", "/.asdf", // version managers
    "/.bun/bin", "/.deno/bin",   // Bun / Deno
    "/.dotnet", "/.sdkman/candidates", // .NET / JVM
    // git's user config: identity, aliases, default branch. git treats an
    // UNREADABLE ~/.gitconfig as fatal ("unable to access"), not as absent,
    // so without it every git command inside the sandbox died -- status and
    // log included. Read-only, and the credential store (.git-credentials,
    // gh's hosts.yml) stays masked: a helper NAMED here still has nothing to
    // read.
    "/.gitconfig", "/.config/git",
};
#endif  // MAYA_OS_LINUX || MAYA_OS_APPLE

// Windows' names for the same three lists, resolved at posture build time:
// the toolchain and shell live under the system root and Program Files, and
// the user profile is where the credential hygiene still bites (a Windows
// .ssh is at %USERPROFILE%\\.ssh, .aws at %USERPROFILE%\\.aws, just like
// posix). claybin's windows compiler takes NATIVE paths; these are native.
#if MAYA_OS_WINDOWS
// {env var, fallback}. Windows isn't always on C:, so read the real roots.
constexpr std::pair<const char*, const char*> kSystemReadRootsWin[] = {
    {"SystemRoot", "C:\\Windows"},                  // loader, DLLs, System32
    {"ProgramFiles", "C:\\Program Files"},           // installed tools
    {"ProgramFiles(x86)", "C:\\Program Files (x86)"},
};
// "readable machine config", the %ProgramData% half of what /etc is on posix.
constexpr std::pair<const char*, const char*> kEtcReadableWin[] = {
    {"ProgramData", "C:\\ProgramData"},  // package manager metadata, host ssh config
};
[[nodiscard]] std::string win_dir(const std::pair<const char*, const char*>& d) {
    const char* v = std::getenv(d.first);
    return v && *v ? std::string{v} : std::string{d.second};
}
constexpr const char* kHomeToolSubdirsWin[] = {
    "\\.cargo\\bin", "\\.rustup",   // Rust
    "\\go\\bin",                     // Go
    "\\.nvm",                        // Node version manager
    "\\.pyenv", "\\.rbenv",          // version managers
    "\\.bun\\bin", "\\.deno\\bin",   // Bun / Deno
    "\\.dotnet",                     // .NET
};
#endif  // MAYA_OS_WINDOWS

// Back into the per-platform arm the shared read set interrupted.
#if MAYA_OS_LINUX


// The read set, handed to claybin as DATA rather than restated in its backend.
//
// This function is what keeps the two backends honest. Both consume
// kSystemReadRoots / kEtcReadable / kHomeToolSubdirs, so a change to the
// boundary applies to both or neither. A previous backend restated the set by
// hand and ended up granting read on `/` -- handing ~/.ssh and ~/.aws to any
// approved bash call while still reporting "sandbox: active".
//
// POSIX-wide rather than Linux-only: the macOS claybin backend consumes the
// very same Posture. That is the payoff for describing the boundary as data
// instead of as argv -- the read set, the masks and the pane's extra grants
// cross the platform line unchanged, and only the mechanisms that enforce
// them differ.
//
// Hence the #endif immediately below, closing the Linux arm that opened far
// above: this one function has to be visible to BOTH the Linux and the macOS
// run_wrapped(), and the per-platform arm reopens right after it.
#endif  // MAYA_OS_LINUX (reopened below)

// Below Strict, a JIT command gets W^X lifted or it cannot start at all.
// Strict (Hardened, Airgapped) keeps the wall and the build fails instead.
void relax_wx_for_jit(claybin_backend::Posture& p, std::string_view cmd) {
    if (p.wx_protect && p.syscall_mode != 2 && command_needs_jit(cmd))
        p.wx_protect = false;
}
[[maybe_unused]] std::string join_argv(const std::vector<std::string>& argv) {
    std::string s;
    for (const auto& a : argv) { s += a; s += ' '; }
    return s;
}

// The host's global git identity. Empty when the host has none (identity set
// per repository, which the guest reads itself) or git is not installed.
//
// Cached against the size+mtime of the files git reads for --global, so the
// cost is one stat per file per spawn, a `git config --global user.name ...`
// on the host is picked up by the next command rather than the next restart,
// and a missing git is asked once per change rather than once per command.
// One git spawn reads both keys; the timeout is short because this runs on
// the command's own spawn path. An [include] in the global file is followed
// (--includes) but not stat-ed -- editing only an included file needs a
// touch of the top-level one, which is the same rule git's own fsmonitor
// users live with.
[[nodiscard]] std::uint64_t git_global_stamp() {
    std::vector<fs::path> files;
    if (const char* g = std::getenv("GIT_CONFIG_GLOBAL"); g && *g) {
        files.emplace_back(g);
    } else {
        const fs::path home = ::agentty::util::home_dir_or_empty();
        if (!home.empty()) files.push_back(home / ".gitconfig");
        if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x)
            files.push_back(fs::path{x} / "git" / "config");
        else if (!home.empty())
            files.push_back(home / ".config" / "git" / "config");
    }
    std::uint64_t h = 1469598103934665603ull;   // FNV-1a over (size, mtime)
    const auto mix = [&h](std::uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    for (const auto& f : files) {
        std::error_code ec;
        const auto sz = fs::file_size(f, ec);
        mix(ec ? ~0ull : static_cast<std::uint64_t>(sz));
        const auto mt = fs::last_write_time(f, ec);
        mix(ec ? 0 : static_cast<std::uint64_t>(mt.time_since_epoch().count()));
    }
    return h;
}

[[nodiscard]] std::string read_host_git_identity() {
    auto r = run_argv_s({"git", "config", "--global", "--includes", "--get-regexp",
                         "^user\\.(name|email)$"},
                        4096, std::chrono::seconds{2});
    // 1 = no match (no global identity): an answer, not an error.
    if (!r.started || r.timed_out || r.exit_code != 0) return {};
    std::string name, email;
    std::string_view rest = r.output;
    while (!rest.empty()) {
        const auto nl = rest.find('\n');
        std::string_view line = rest.substr(0, nl);
        rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
        while (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const auto sp = line.find(' ');
        if (sp == std::string_view::npos) continue;
        const auto key = line.substr(0, sp);
        const auto val = line.substr(sp + 1);
        // Last one wins, as for git itself. A value that would need escaping
        // in the file we write is dropped rather than half-quoted.
        if (val.empty() || val.find_first_of("\"\\") != std::string_view::npos) continue;
        if (key == "user.name") name = val;
        else if (key == "user.email") email = val;
    }
    std::string out;
    if (!name.empty())  out += "\tname = \"" + name + "\"\n";
    if (!email.empty()) out += "\temail = \"" + email + "\"\n";
    return out.empty() ? out : "[user]\n" + out;
}

[[nodiscard]] std::string host_git_identity() {
    struct Cache {
        bool          valid = false;
        std::uint64_t stamp = 0;
        std::string   identity;
    };
    // Read-mostly and shared by every spawning worker: a snapshot, as for the
    // mask sweep. Two concurrent misses both ask git; the last store wins and
    // both answers are right for their stamp.
    static maya::snapshot<Cache> cache;
    const std::uint64_t stamp = git_global_stamp();
    if (auto cur = cache.load(); cur->valid && cur->stamp == stamp) return cur->identity;
    Cache next{.valid = true, .stamp = stamp, .identity = read_host_git_identity()};
    std::string id = next.identity;
    cache.store(std::move(next));
    return id;
}

// `<workspace>/.agentty/cache/git-identity`, holding only [user] name and
// email, rewritten when it differs. Nullopt when the host has no global
// identity or the file cannot be written.
[[nodiscard]] std::optional<std::string> global_git_identity_file(const std::string& workspace) {
    const std::string body = host_git_identity();
    if (body.empty() || workspace.empty()) return std::nullopt;
    const fs::path file = fs::path{workspace} / ".agentty" / "cache" / "git-identity";
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (ec) return std::nullopt;
    std::string have;
    if (std::ifstream in{file, std::ios::binary}; in)
        have.assign(std::istreambuf_iterator<char>{in}, {});
    if (have != body
        && !persistence::write_atomic(::agentty::IoAccess::grant(), file, body,
                                      {.durability = persistence::Durability::Atomic}))
        return std::nullopt;
    return file.string();
}

#if MAYA_OS_LINUX || MAYA_OS_APPLE || MAYA_OS_WINDOWS
// The directory a trusted file under one of these is protected through, or
// nullopt. These hold many files (and running binaries), so one read-only
// bind of the directory replaces a bind per file.
[[nodiscard]] std::optional<std::string> trusted_dir_of(const std::string& path,
                                                        const std::string& workspace) {
    static constexpr std::string_view kDirs[] = {
        ".local/bin", ".config/autostart", ".config/systemd",
        ".config/fish", ".config/environment.d", ".config/hypr",
        ".config/sway", ".config/i3", ".config/plasma-workspace",
    };
    for (auto rel : kDirs) {
        std::string dir = workspace;
        if (!dir.empty() && dir.back() != '/') dir += '/';
        dir += rel;
        if (path.size() > dir.size() && path.starts_with(dir) && path[dir.size()] == '/')
            return dir;
    }
    return std::nullopt;
}

[[nodiscard]] claybin_backend::Posture build_claybin_posture(
        const handoff::TrustedSnapshot* trusted = nullptr) {
    claybin_backend::Posture p;

    // The user's saved policy, or the shipped default when they have never
    // opened the pane. `configured` is what keeps an upgrade from changing
    // anyone's boundary.
    //
    // ONE snapshot, held for the whole function. This runs on a tool worker
    // thread while the reducer thread may be publishing a new policy, so
    // re-reading the global per field could mix two policies into one
    // sandbox -- read paths from the old one, syscall profile from the new.
    // Holding the snapshot also means a save cannot change the boundary of a
    // command that is already being built.
    const auto snap = config_snapshot();
    const auto& cfg = *snap;

    // ── the read set, by scope ────────────────────────────────────────
    //
    // Minimal deliberately omits the home toolchain dirs: for a
    // self-contained repo, anything under the home directory is suspicious.
    // HostReadable is the convenient answer other agents take, named
    // honestly -- it is still confined by the appcontainer token's DACLs and
    // by the mask list, but your other projects are readable by an approved
    // command. On Windows the lists are native paths (kSystemReadRootsWin etc.);
    // the home env var is USERPROFILE there and HOME nowhere else.
#if MAYA_OS_WINDOWS
    for (const auto& r : kSystemReadRootsWin) p.system_read_roots.push_back(win_dir(r));
    for (const auto& f : kEtcReadableWin) p.etc_readable.push_back(win_dir(f));
    if (cfg.fs_scope == sandbox_cfg::FsScope::Toolchain) {
        if (const char* home = std::getenv("USERPROFILE"); home && *home) {
            for (const char* sub : kHomeToolSubdirsWin)
                p.home_tool_dirs.emplace_back(std::string{home} + sub);
        }
    } else if (cfg.fs_scope == sandbox_cfg::FsScope::HostReadable) {
        if (const char* drive = std::getenv("SystemDrive"); drive && *drive)
            p.system_read_roots.emplace_back(std::string{drive} + "\\");
        else
            p.system_read_roots.emplace_back("C:\\");
    }
#else
    for (const char* r : kSystemReadRoots) p.system_read_roots.emplace_back(r);
#if MAYA_OS_LINUX
    // /usr/lib32 (and Debian's /usr/lib/i386-linux-gnu) are already under
    // the read-only /usr bind. Separate top-level multilib roots are optional.
    for (const char* r : {"/lib32", "/libx32"}) {
        std::error_code ec;
        if (fs::is_directory(r, ec)) p.system_read_roots.emplace_back(r);
    }
    // A merged-/usr root (/lib -> usr/lib) is recreated as the same link, not
    // bound: see Posture::system_links. Only a link whose target resolves
    // inside /usr qualifies, so a link to anywhere else is still a plain bind.
    std::erase_if(p.system_read_roots, [&](const std::string& root) {
        if (root == "/usr") return false;
        std::error_code ec;
        if (!fs::is_symlink(fs::symlink_status(root, ec))) return false;
        const auto target = fs::read_symlink(root, ec);
        if (ec) return false;
        const auto resolved = fs::canonical(root, ec);
        if (ec || !resolved.string().starts_with("/usr/")) return false;
        p.system_links.emplace_back(root, target.string());
        return true;
    });
#endif
    for (const char* f : kEtcReadable) p.etc_readable.emplace_back(f);
    if (cfg.fs_scope == sandbox_cfg::FsScope::Toolchain) {
        if (const char* home = std::getenv("HOME"); home && *home) {
            for (const char* sub : kHomeToolSubdirs)
                p.home_tool_dirs.emplace_back(std::string{home} + sub);
        }
    } else if (cfg.fs_scope == sandbox_cfg::FsScope::HostReadable) {
        // `/` alone: binding /usr etc. under a read-only `/` fails creating
        // their mount points, which killed every spawn.
        p.system_read_roots = {"/"};
        p.system_links.clear();
        p.etc_readable.clear();
    }
#endif
    // Whatever the user added on top, made absolute (claybin ignores `~` and
    // relative paths).
    p.workspace = workspace_root().string();
    {
#if MAYA_OS_WINDOWS
        const char* h = std::getenv("USERPROFILE");
#else
        const char* h = std::getenv("HOME");
#endif
        const std::string_view home = h ? h : "";
        for (const auto& r : cfg.read_paths)
            if (auto a = sandbox_cfg::resolve_user_path(r, p.workspace, home); !a.empty())
                p.read_paths.push_back(std::move(a));
        for (const auto& w : cfg.write_paths)
            if (auto a = sandbox_cfg::resolve_user_path(w, p.workspace, home); !a.empty())
                p.write_paths.push_back(std::move(a));
    }

    // Minimal scope hides ~/.gitconfig, so a user whose identity is only
    // global could not commit. Hand over the two identity values and nothing
    // else of that file: GIT_CONFIG_GLOBAL is the global scope, so a repo's
    // own user.name still wins, as on the host.
    if (cfg.fs_scope == sandbox_cfg::FsScope::Minimal)
        if (auto f = global_git_identity_file(p.workspace)) {
            p.extra_env.emplace_back("GIT_CONFIG_GLOBAL", *f);
            p.handoff_ro.push_back(*f);
        }

#if MAYA_OS_APPLE
    // darwin's per-user temp and cache dirs (/var/folders/xx/yyy/{T,C}).
    // This is where $TMPDIR points, NOT /tmp: mktemp, python's tempfile,
    // xcrun's tool cache, clang's module cache, swift and every Cocoa tool
    // write there, and without it /usr/bin/git and cc failed with "couldn't
    // create cache file". The tmp-equivalent of the Linux tmpfs: private to
    // this uid, and holding nothing a command could not have written itself.
    for (int which : {_CS_DARWIN_USER_TEMP_DIR, _CS_DARWIN_USER_CACHE_DIR}) {
        char buf[1024];
        const std::size_t n = ::confstr(which, buf, sizeof buf);
        if (n > 1 && n <= sizeof buf) {
            std::string d{buf};
            while (d.size() > 1 && d.back() == '/') d.pop_back();
            p.write_paths.push_back(std::move(d));
        }
    }
#endif

    p.workspace = workspace_root().string();
    p.cwd = p.workspace;

    // ── secrets, always, plus the user's own masks ───────────────
    //
    // kAlwaysMasked is not configurable, and that is the point: a control the
    // user can switch off to make their build work is a control that is off.
    // The pane can ADD masks, never remove these.
    //
    // The LIST is shared with the bwrap path (sandbox_cfg::mask_paths), so
    // the backends cannot disagree about what is covered. On Windows the home
    // prefix differs (%USERPROFILE% instead of $HOME); the list is POSIX
    // vocabulary, so each platform prefixes its own home onto the same names.
    {
#if MAYA_OS_WINDOWS
        const char* home = std::getenv("USERPROFILE");
#else
        const char* home = std::getenv("HOME");
#endif
        auto masks = sandbox_cfg::mask_paths(cfg, p.workspace,
                                            home ? home : "");
        p.masked.insert(p.masked.end(),
                        std::make_move_iterator(masks.begin()),
                        std::make_move_iterator(masks.end()));
    }

#if !MAYA_OS_WINDOWS
    // ── /dev/tty: the controlling terminal is agentty's own screen ────
    //
    // Not a credential, so it is not in kAlwaysMasked (that list is $HOME
    // secrets) -- but it is masked for the same reason, on every backend that
    // has a device tree. Windows has no /dev/tty node: the console is a
    // handle the child either inherits (it does not here) or opens through
    // CONIN$/CONOUT$, both blocked by the appcontainer's default-deny on
    // the console object. So there is nothing to mask and nothing omitted.
    //
    // claybin's devtmpfs binds the HOST /dev/tty into the sandbox, following
    // bubblewrap's node list exactly. That node resolves to the calling
    // process's controlling terminal, so a child that opens it writes
    // straight to the screen -- bypassing the pipe whose bytes clean_capture()
    // scrubs. `printf '\033[2J\033[3J' > /dev/tty` wipes the transcript.
    //
    // Masking is the right mechanism rather than dropping the node: an
    // absent /dev/tty changes behaviour for programs that probe for a
    // terminal, while a masked one still opens and still accepts writes --
    // they just go nowhere. Captured stdout/stderr are untouched.
    p.masked.emplace_back("/dev/tty");
#endif

    // ── trust-handoff prevention: bind host-trusted paths read-only ─────
    //
    // The PREVENTION half of the gate. Detection (handoff::review_trusted)
    // reports a write after the bytes land and works on every host; this stops
    // the write, and needs a mount namespace.
    //
    // Reuses handoff::snapshot_trusted so the two halves cannot disagree about
    // WHICH paths are host-trusted -- the same reasoning the mask list above
    // records for bwrap vs claybin, where two copies drifted and one of them
    // did not exist. One table, two mechanisms.
    //
    // Only EXISTING paths: a bind of a path that is not there fails, and
    // bind_try tolerating that is not the same as wanting it. A trusted path
    // CREATED during the session is what detection is for.
    //
    // Skipped when the policy is Allow -- a user who turned the gate off has
    // said these files are theirs to edit, and silently making them read-only
    // would be the config lying to them.
    if (cfg.handoff != sandbox_cfg::HandoffPolicy::Allow && !p.workspace.empty()) {
        // The dispatcher already walked the workspace for this call (to watch
        // it); reuse that instead of walking again, when it walked this root.
        if (trusted && fs::path{trusted->root}.lexically_normal() != fs::path{p.workspace}.lexically_normal()
            && fs::path{trusted->root + "/"}.lexically_normal() != fs::path{p.workspace + "/"}.lexically_normal())
            trusted = nullptr;
        handoff::TrustedSnapshot walked;
        if (!trusted) walked = handoff::snapshot_trusted(p.workspace);
        const auto& snap = trusted ? *trusted : walked;
        p.handoff_ro.reserve(snap.entries.size());
        // Files under a .git/hooks dir get one ro bind of that dir instead of
        // one each. A checkout of many repos has thousands.
        std::set<std::string> dirs;
        // A masked path must stay masked. The handoff walk matches .agentty
        // as a hook-config dir, so with the workspace at $HOME it found
        // ~/.agentty/memory.jsonl and bound the real file read-only on top
        // of its mask. Skip anything that is masked or sits under a mask.
        auto under_mask = [&](const std::string& path) {
            for (const auto& m : p.masked) {
                if (path == m) return true;
                if (path.size() > m.size() && path.starts_with(m)
                    && path[m.size()] == '/')
                    return true;
            }
            return false;
        };
        // A core.hooksPath dir (.husky, .githooks) is bound whole, so a NEW
        // hook cannot appear in it either; its files are skipped below.
        std::error_code plec;
        for (const auto& d : snap.pinned_dirs) {
            if (!under_mask(d) && !fs::is_symlink(fs::symlink_status(d, plec))
                && dirs.insert(d).second)
                p.handoff_ro.push_back(d);
        }
        // Anchor each git dir in place (all postures), and each nested repo's
        // worktree under the strict ones, where `mv repo` is rarer than the
        // escape it closes. The workspace root is a mount already.
        const bool strict = cfg.syscall_mode == sandbox_cfg::SyscallMode::Strict;
        for (const auto& g : snap.git_dirs) {
            if (under_mask(g) || fs::is_symlink(fs::symlink_status(g, plec))) continue;
            p.handoff_anchor.push_back(g);
            const std::string wt = fs::path{g}.parent_path().string();
            if (strict && wt != p.workspace && !under_mask(wt)
                && !fs::is_symlink(fs::symlink_status(wt, plec)))
                p.handoff_anchor.push_back(wt);
        }
        // Outermost first, so an inner anchor is not covered by an outer one.
        std::ranges::sort(p.handoff_anchor);
        for (const auto& e : snap.entries) {
            if (!e.existed) continue;
            if (under_mask(e.path)) continue;
            // A bind follows the link, so it can't protect it, and a link that
            // points out of the tree makes claybin's confined open fail (EXDEV).
            std::error_code lec;
            if (fs::is_symlink(fs::symlink_status(e.path, lec))) continue;
            const fs::path parent = fs::path{e.path}.parent_path();
            if (dirs.contains(parent.string())) continue;
            const auto pname = parent.filename();
            // Any git dir's hooks/: the main .git, a submodule's
            // .git/modules/<name> (one checkout with 7 submodules was paying
            // 98 per-file binds per command), or a worktree's commondir.
            const fs::path gd = parent.parent_path();
            const bool git_hooks = pname == "hooks"
                && (gd.filename() == ".git"
                    || gd.parent_path().filename() == "modules"
                    || fs::exists(gd / "HEAD", lec));
            if (git_hooks && !fs::is_symlink(fs::symlink_status(parent, lec))) {
                if (dirs.insert(parent.string()).second && !under_mask(parent.string()))
                    p.handoff_ro.push_back(parent.string());
                continue;
            }
            // Same for the login and PATH dirs: one ro bind of the dir, not
            // one per file. Binding a file needs claybin to open it for
            // writing, which fails with ETXTBSY on a binary in ~/.local/bin
            // that is running right now, and that failed every spawn.
            if (const auto d = trusted_dir_of(e.path, p.workspace)) {
                if (dirs.insert(*d).second && !under_mask(*d)
                    && !fs::is_symlink(fs::symlink_status(*d, lec)))
                    p.handoff_ro.push_back(*d);
                continue;
            }
            p.handoff_ro.push_back(e.path);
        }
    }

    // ── agentty's own config: never writable from inside ────────────
    //
    // Not subject to the handoff policy, and not only when the workspace is
    // $HOME: a write path the user added can reach it too, and so can an
    // agent's own home. build_policy keeps only the roots some writable bind
    // reaches. Resolved paths as well as spelled ones, because the bind acts
    // on whatever the path resolves to.
    for (const auto& r : ::agentty::util::trust_roots()) {
        std::error_code ec;
        if (!fs::exists(r, ec)) continue;
        p.own_config_ro.push_back(r.string());
        if (const auto c = fs::canonical(r, ec); !ec && c != r)
            p.own_config_ro.push_back(c.string());
    }

    // ── network ──────────────────────────────────────────────────────────
    p.net_mode = static_cast<int>(cfg.net_mode);
    p.allow_ports = cfg.allow_ports;
    p.allow_hosts = cfg.allow_hosts;

    // ── syscalls and caps ────────────────────────────────────────────────
    p.syscall_mode = static_cast<int>(cfg.syscall_mode);
    p.wx_protect = cfg.wx_protect;
    p.scope_ipc = cfg.scope_ipc;
    p.close_inherited_fds = cfg.close_inherited_fds;
    p.tmp_bytes = cfg.tmp_mb * 1024ull * 1024;
    p.memory_bytes = cfg.memory_mb * 1024ull * 1024;
    p.max_processes = cfg.max_procs;
    p.cpu_percent = cfg.cpu_percent;
    p.max_open_files = cfg.max_open_files;
    p.wall_clock_secs = cfg.wall_clock_secs;
    p.cpu_secs = cfg.cpu_secs;
    p.fake_hostname = cfg.fake_hostname;
    p.env_scope = static_cast<int>(cfg.env_scope);
    p.env_keep = cfg.env_keep;
    // Brokering is on whenever the filter is, because the runner can always
    // poll the listener -- it already owns a poll loop for the output pipe.
    // Gated on the filter rather than being a separate row: brokering IS the
    // syscall filter deciding at runtime instead of up front, so a user who
    // turned the filter off has already said they do not want either.
    p.broker = cfg.syscall_mode != sandbox_cfg::SyscallMode::Off;
    return p;
}

// Role::Command's posture for ONE command: the sealed config, W^X lifted when
// `command_line` starts a JIT, then this call's cwd and extra roots (an editor
// session's other folders, writable like the workspace). Every command spawn
// path builds through here, and posture_for() starts from it, so the shell
// tool, hooks, ACP terminals and long-lived children cannot drift apart again.
//
// `command_line` is the line the shell will see: the string itself for a
// shell command, join_argv() for an argv. The JIT check reads command
// position from it, and a joined argv loses the quoting a shell string keeps.
[[nodiscard]] claybin_backend::Posture command_posture(
        std::string_view command_line, std::string_view cwd = {},
        const std::vector<std::string>& extra_roots = {},
        const handoff::TrustedSnapshot* trusted = nullptr) {
    auto p = build_claybin_posture(trusted);
    relax_wx_for_jit(p, command_line);
    if (!cwd.empty()) p.cwd = std::string{cwd};
    for (const auto& r : extra_roots)
        if (!r.empty()) p.write_paths.push_back(r);
    return p;
}

// claybin's spawn result, as the runner adopts it. ALL of it, every time: the
// copies of this used to differ per platform, and Windows' shell arm dropped
// the network proxy that its argv arm kept alive.
//   pidfd          Linux: jaal reaps by it, no pid-reuse race. Windows: the
//                  process HANDLE, the waitable exit.
//   supervisor_fd  the syscall broker, polled beside the output pipe for the
//                  child's whole life (a brokered call blocks until answered)
//   keepalive      the filtered network proxy; it must outlive the child
//   kill_tree      cgroup.kill / the job object, when the posture got one;
//                  empty means the runner falls back to the process group
[[maybe_unused]] [[nodiscard]] SubprocessOptions::SpawnedChild adopt_spawn(
        claybin_backend::SpawnResult r) {
    SubprocessOptions::SpawnedChild out;
    if (!r.started) { out.error = std::move(r.start_error); return out; }
    out.pid           = r.pid;
    out.pidfd         = r.pidfd;
    out.supervisor_fd = r.supervisor_fd;
    out.service       = std::move(r.service_broker);
    out.keepalive     = std::move(r.net_proxy);
    out.kill_tree     = std::move(r.kill_tree);
    return out;
}
#endif  // MAYA_OS_LINUX || MAYA_OS_APPLE || MAYA_OS_WINDOWS

// Back into the per-platform arm the posture builder interrupted.
//
// Linux takes the first branch (its run_wrapped), macOS the second, Windows
// the third. The guard must stay linux-ONLY: a `|| _WIN32` here would take
// the linux branch on Windows and the probe() definition below would never
// compile -- the one thing init() needs.
#if MAYA_OS_LINUX

[[nodiscard]] SubprocessResult run_wrapped(std::string_view cmd,
                                           std::size_t max_bytes,
                                           std::chrono::seconds timeout,
                                           std::string stdin_data = {},
                                           bool split_stderr = false) {
    SubprocessOptions opts;
    opts.max_bytes = max_bytes;
    opts.timeout = timeout;
    opts.stdin_data = std::move(stdin_data);
    opts.split_stderr = split_stderr;

#if MAYA_OS_LINUX || MAYA_OS_APPLE
    if (detected_backend() == Backend::Claybin) {
        // claybin is a library, so there is no argv prefix to build. It forks,
        // applies the plan, and execs itself -- but only the SPAWN; the
        // supervise loop (poll, progress, idle deadline, SIGTERM/SIGKILL, reap)
        // stays in Subprocess::run, shared with every other path.
        //
        // Identical on both platforms, which is the whole return on keeping
        // the supervise loop out of the backend: Linux hands back a pidfd and
        // a seccomp-notify fd, macOS hands back neither, and the runner needs
        // no branch to tell them apart -- the sentinel values already say so.
        //
        // `command` still carries the shell string so the runner's logging and
        // error messages read the same for both backends.
        opts.command = SubprocessOptions::Shell{std::string{cmd}};
        std::string shell_cmd{cmd};
        opts.spawner = [shell_cmd](const SubprocessOptions&, int out_fd, int err_fd,
                                   int in_fd) -> SubprocessOptions::SpawnedChild {
            return adopt_spawn(claybin_backend::spawn_shell(
                command_posture(shell_cmd), shell_cmd, out_fd, err_fd, in_fd));
        };
        return Subprocess::run(std::move(opts));
    }
#endif

    // No backend: run_wrapped is only reached when the sandbox is active,
    // and on Linux "active" means claybin. Reaching here would mean the
    // probe said yes and the spawn path disagreed, so refuse rather than
    // run the command unconfined -- a command that was approved on the
    // understanding it would be boxed must not quietly escape the box.
    SubprocessResult unconfined;
    unconfined.started     = false;
    unconfined.start_error = "sandbox is active but no backend could wrap "
                             "this command; refusing to run it unconfined";
    return unconfined;
}

// argv-form: same wrap as the shell form, with the user's argv reaching the
// child exactly as given -- no `sh -c` indirection to re-parse quotes or
// `$vars`.
[[nodiscard]] SubprocessResult run_wrapped_argv(const std::vector<std::string>& user_argv,
                                                std::size_t max_bytes,
                                                std::chrono::seconds timeout) {
    if (user_argv.empty()) {
        SubprocessResult r;
        r.started = false; r.start_error = "empty argv";
        return r;
    }

    SubprocessOptions opts;
    opts.max_bytes   = max_bytes;
    opts.timeout     = timeout;

#if MAYA_OS_LINUX || MAYA_OS_WINDOWS
    if (detected_backend() == Backend::Claybin) {
        opts.command = SubprocessOptions::Argv{user_argv};
        std::vector<std::string> argv = user_argv;
        opts.spawner = [argv](const SubprocessOptions&, int out_fd, int err_fd,
                              int in_fd) -> SubprocessOptions::SpawnedChild {
            return adopt_spawn(claybin_backend::spawn_argv(
                command_posture(join_argv(argv)), argv, out_fd, err_fd, {}, in_fd));
        };
        return Subprocess::run(std::move(opts));
    }
#endif

    SubprocessResult unconfined;
    unconfined.started     = false;
    unconfined.start_error = "sandbox is active but no backend could wrap "
                             "this command; refusing to run it unconfined";
    return unconfined;
}

#elif MAYA_OS_APPLE

[[nodiscard]] Backend probe() {
    // claybin first, when asked for AND usable.
    //
    // Both engines here are seatbelt -- claybin calls sandbox_init() directly
    // from a compiled policy, sandbox-exec is Apple's CLI wrapper around the
    // same kernel mechanism -- so this is not a choice about strength. It is a
    // choice about what survives the trip.
    //
    // build_profile() below concatenates a fixed profile string, so it can
    // express exactly what it was written to express: the workspace, /tmp, the
    // caches. The claybin path compiles the same Posture the Linux backend
    // consumes, so the pane's read_paths, write_paths, masks and resource caps
    // mean something on a mac instead of being Linux-only settings that
    // silently do nothing here. It also produces the guarantee report, which
    // is the part worth having: "filesystem.read strong via seatbelt" is
    // checkable in a way that "sandbox: active" is not.
    //
    // The preference is shared with Linux rather than given a second knob. A
    // user's answer to "which engine applies my policy" does not change when
    // they move between a mac and a Linux box.
    //
    // claybin first where it can build, sandbox-exec otherwise. Both are
    // real boundaries here; the order just prefers the one whose policy
    // vocabulary the pane actually speaks.
    if (claybin_backend::available()) return Backend::Claybin;
    if (can_invoke("sandbox-exec")) return Backend::SandboxExec;
    // And the other direction, for symmetry with the Ubuntu 24.04 case: if
    // sandbox-exec is somehow missing (SIP damage, a stripped image), claybin
    // still talks to the kernel directly and does not need the binary.
    if (claybin_backend::available()) return Backend::Claybin;
    return Backend::None;
}

// Generate a minimal sandbox-exec profile. Allows reads broadly,
// limits writes to workspace + tmp + system caches, allows network
// (same rationale as bwrap: agent-typical commands need it).
//
// Apple deprecated `sandbox-exec` in public docs but the binary keeps
// working. The profile language is Scheme-ish; we keep it small so a
// future Apple removal is easy to spot.
[[nodiscard]] std::string build_profile(std::string_view workspace) {
    std::string p;
    p += "(version 1)\n";
    p += "(deny default)\n";
    // Process / signals
    p += "(allow process-exec)\n";
    p += "(allow process-fork)\n";
    p += "(allow signal (target same-sandbox))\n";
    // Reads: broad. Credential paths are denied at the end of the profile.
    p += "(allow file-read*)\n";
    // Writes: workspace + tmp/cache regions only. The workspace path is
    // SBPL-escaped (see sbpl_escape). If it can't be represented safely (a
    // control char), the workspace write clause is OMITTED entirely rather
    // than risk a broken/injected profile — sandboxed bash then gets no
    // workspace write access, which is safe (fail closed), not a bypass.
    if (std::string ws = sbpl_escape(workspace); !ws.empty())
        p += "(allow file-write* (subpath \"" + ws + "\"))\n";
    p += "(allow file-write* (subpath \"/tmp\"))\n";
    p += "(allow file-write* (subpath \"/private/tmp\"))\n";
    p += "(allow file-write* (subpath \"/private/var/folders\"))\n";   // user caches
    p += "(allow file-write* (subpath \"/dev/null\"))\n";
    // /dev/tty is deliberately NOT allowed — see the long note on the bwrap
    // backend's --dev-bind /dev/null /dev/tty. It is the controlling
    // terminal, i.e. agentty's own screen, and it bypasses the pipe that
    // clean_capture() scrubs. A child writing \033[2J\033[3J there wipes the
    // transcript. Captured stdout/stderr are unaffected; this only removes a
    // child's ability to paint the UI directly.
    // Network: open. Restricting would break git push / curl / npm.
    p += "(allow network*)\n";
    p += "(allow system-socket)\n";
    // Mach services BY NAME. A bare (allow mach-lookup) lets a guest read the
    // pasteboard and enumerate keychains through launchd daemons that run
    // outside the sandbox. Same allowlist claybin's macOS backend emits:
    // directory lookups, logging, prefs, plus DNS/TLS since network is open.
    p += "(allow mach-lookup\n"
         "  (global-name \"com.apple.system.opendirectoryd.libinfo\")\n"
         "  (global-name \"com.apple.system.logger\")\n"
         "  (global-name \"com.apple.logd\")\n"
         "  (global-name \"com.apple.diagnosticd\")\n"
         "  (global-name \"com.apple.PowerManagement.control\")\n"
         "  (global-name \"com.apple.cfprefsd.daemon\")\n"
         "  (global-name \"com.apple.cfprefsd.agent\")\n"
         "  (global-name \"com.apple.dnssd.service\")\n"
         "  (global-name \"com.apple.mDNSResponder\")\n"
         "  (global-name \"com.apple.SystemConfiguration.DNSConfiguration\")\n"
         "  (global-name \"com.apple.SystemConfiguration.configd\")\n"
         "  (global-name \"com.apple.networkd\")\n"
         "  (global-name \"com.apple.trustd\")\n"
         "  (global-name \"com.apple.trustd.agent\")\n"
         "  (global-name \"com.apple.ocspd\"))\n";
    p += "(allow pseudo-tty)\n";
    p += "(allow file-read* file-write* file-ioctl (literal \"/dev/ptmx\"))\n";
    p += "(allow ipc-posix-sem)\n";
    p += "(allow iokit-open)\n";
    p += "(allow sysctl-read)\n";
    // Credential paths, the same list claybin masks. Last, because a later
    // rule wins in SBPL and the workspace write allow above would reopen them.
    {
        const char* home = std::getenv("HOME");
        const auto snap = config_snapshot();
        for (const auto& m : sandbox_cfg::mask_paths(*snap, workspace, home ? home : ""))
            if (std::string esc = sbpl_escape(m); !esc.empty())
                p += "(deny file-read* file-write* (subpath \"" + esc + "\"))\n";
    }
    return p;
}

[[nodiscard]] SubprocessResult run_wrapped(std::string_view cmd,
                                           std::size_t max_bytes,
                                           std::chrono::seconds timeout,
                                           std::string stdin_data = {},
                                           bool split_stderr = false) {
    SubprocessOptions opts;
    opts.max_bytes = max_bytes;
    opts.timeout = timeout;
    opts.stdin_data = std::move(stdin_data);
    opts.split_stderr = split_stderr;

    if (detected_backend() == Backend::Claybin) {
        // NOT claybin's spawn + adopt, as on Linux: darwin has no pidfd, so
        // jaal's posix_process::adopt refuses every foreign child ("the exit
        // is not watchable"). That refusal came AFTER the spawn, so the guest
        // ran unsupervised while the caller got exit 128 and no output --
        // every sandboxed hook and ACP terminal command silently failed.
        // Instead put claybin's compiled profile into the argv via
        // sandbox-exec, exactly as the bash tool does (JaalExec::confine):
        // same posture, and the child is our own fork, so it is watchable.
        auto wrapped = wrap_argv_for_exec({"/bin/sh", "-c", std::string{cmd}}, {}, {});
        if (!wrapped) {
            SubprocessResult r;
            r.started = false;
            r.start_error = "sandbox: " + wrapped.error();
            return r;
        }
        opts.command = SubprocessOptions::Argv{std::move(*wrapped)};
        return Subprocess::run(std::move(opts));
    }

    auto profile = build_profile(workspace_root().string());
    opts.command = SubprocessOptions::Argv{{
        "sandbox-exec", "-p", std::move(profile),
        "/bin/sh", "-c", std::string{cmd}
    }};
    return Subprocess::run(std::move(opts));
}

[[nodiscard]] SubprocessResult run_wrapped_argv(const std::vector<std::string>& user_argv,
                                                std::size_t max_bytes,
                                                std::chrono::seconds timeout) {
    if (user_argv.empty()) {
        SubprocessResult r;
        r.started = false; r.start_error = "empty argv";
        return r;
    }
    SubprocessOptions opts;
    auto profile = build_profile(workspace_root().string());
    std::vector<std::string> argv{"sandbox-exec", "-p", std::move(profile)};
    for (const auto& a : user_argv) argv.push_back(a);
    opts.command = SubprocessOptions::Argv{std::move(argv)};
    opts.max_bytes = max_bytes;
    opts.timeout = timeout;
    return Subprocess::run(std::move(opts));
}

#elif MAYA_OS_WINDOWS

// A REAL capability probe, not a table lookup: claybin's probe_host() creates
// and deletes an appcontainer profile, proves a restricted token by RUNNING a
// child under it, and initializes the mitigation attribute list. The floor is
// one working wall -- appcontainer or job objects -- so Auto degrades only
// when there is genuinely nothing to build a boundary from.
[[nodiscard]] Backend probe() {
    if (claybin_backend::available()) return Backend::Claybin;
    return Backend::None;
}

[[nodiscard]] SubprocessResult run_wrapped(std::string_view cmd,
                                           std::size_t max_bytes,
                                           std::chrono::seconds timeout,
                                           std::string stdin_data = {},
                                           bool split_stderr = false) {
    SubprocessOptions opts;
    opts.command = SubprocessOptions::Shell{std::string{cmd}};
    opts.max_bytes = max_bytes;
    opts.timeout   = timeout;
    opts.stdin_data = std::move(stdin_data);
    opts.split_stderr = split_stderr;

    if (detected_backend() == Backend::Claybin) {
        opts.spawner = [shell_cmd = std::string{cmd}](
                           const SubprocessOptions&, int out_fd, int err_fd,
                           int in_fd) -> SubprocessOptions::SpawnedChild {
            // The window HANDLEs arrive as ints (the runner's pipes were
            // HANDLEs); the adapter maps them onto the child's std streams.
            return adopt_spawn(claybin_backend::spawn_shell(
                command_posture(shell_cmd), shell_cmd, out_fd, err_fd, in_fd));
        };
    }
    return Subprocess::run(std::move(opts));
}

// argv-form: the user's argv reaches claybin's spawner exactly as given -- no
// cmd.exe /S /C re-parsing of quotes or %vars% on the way in. Same contract as
// the linux and macOS arms.
[[nodiscard]] SubprocessResult run_wrapped_argv(const std::vector<std::string>& user_argv,
                                                std::size_t max_bytes,
                                                std::chrono::seconds timeout) {
    if (user_argv.empty()) {
        SubprocessResult r;
        r.started = false; r.start_error = "empty argv";
        return r;
    }
    SubprocessOptions opts;
    opts.command = SubprocessOptions::Argv{user_argv};
    opts.max_bytes = max_bytes;
    opts.timeout   = timeout;

    if (detected_backend() == Backend::Claybin) {
        opts.spawner = [argv = user_argv](
                           const SubprocessOptions&, int out_fd, int err_fd,
                           int in_fd) -> SubprocessOptions::SpawnedChild {
            return adopt_spawn(claybin_backend::spawn_argv(
                command_posture(join_argv(argv)), argv, out_fd, err_fd, {}, in_fd));
        };
    }
    return Subprocess::run(std::move(opts));
}

#else // Windows / unsupported

[[nodiscard]] Backend probe() { return Backend::None; }

[[nodiscard]] SubprocessResult run_wrapped(std::string_view cmd,
                                           std::size_t max_bytes,
                                           std::chrono::seconds timeout,
                                           std::string stdin_data = {},
                                           bool split_stderr = false) {
    // Should never be called — is_active() returns false on this
    // platform — but defensively fall through to the unsandboxed path.
    SubprocessOptions opts;
    opts.command = SubprocessOptions::Shell{std::string{cmd}};
    opts.max_bytes = max_bytes;
    opts.timeout = timeout;
    opts.stdin_data = std::move(stdin_data);
    opts.split_stderr = split_stderr;
    return Subprocess::run(std::move(opts));
}

[[nodiscard]] SubprocessResult run_wrapped_argv(const std::vector<std::string>& user_argv,
                                                std::size_t max_bytes,
                                                std::chrono::seconds timeout) {
    return run_argv_s(user_argv, max_bytes, timeout);
}

#endif

} // namespace

bool init(Mode requested) {
    g_mode.store(requested, std::memory_order_release);
    auto found = (requested == Mode::Off) ? Backend::None : probe();
#if MAYA_OS_LINUX || MAYA_OS_APPLE || MAYA_OS_WINDOWS
    // "A backend exists" is not "the posture we ship compiles here". A host
    // that can build SOME wall but refuses THIS policy (a mask that cannot be
    // expressed, a network promise that cannot be kept) used to report
    // `sandbox: active` and then fail every command. Compile the real
    // posture once, now: a refusal is a startup fact, stated in the banner,
    // and under On it stops startup with the reason.
    if (found == Backend::Claybin) {
        const auto rep = claybin_backend::describe(build_claybin_posture());
        g_posture_error.publish(std::make_shared<const std::string>(rep.ok ? "" : rep.detail));
        std::vector<DegradedWall> degraded;
        if (rep.ok)
            for (const auto& w : rep.walls)
                if ((w.name == "filesystem.read" || w.name == "filesystem.write"
                     || w.name == "network.isolation" || w.name == "process.isolation")
                    && w.strength != "strong")
                    degraded.push_back({w.name, w.strength, w.mechanism});
        // One cause covers nearly every degraded host: no user namespaces
        // (Ubuntu 24.04's AppArmor default, most containers). Name it once.
        std::string reason;
#if MAYA_OS_LINUX
        if (!degraded.empty()) {
            const auto& h = claybin_backend::host();
            if (!h.user_namespaces || !h.mount_namespaces) reason = "no user namespaces";
        }
#endif
        g_degraded_reason.publish(std::make_shared<const std::string>(std::move(reason)));
        g_posture_degraded.publish(
            std::make_shared<const std::vector<DegradedWall>>(std::move(degraded)));
        if (!rep.ok) found = Backend::None;
    }
#endif
    g_backend.store(found, std::memory_order_release);
    if (requested == Mode::On && found == Backend::None) {
        // Strict mode + no backend = init failure. Caller decides
        // whether to abort startup or downgrade silently.
        return false;
    }
    return true;
}

Mode    requested_mode()   noexcept { return g_mode.load(std::memory_order_acquire); }
Backend detected_backend() noexcept { return g_backend.load(std::memory_order_acquire); }


void set_config(const sandbox_cfg::Config& cfg) {
    // SEALED AFTER THE FIRST CALL. A later call is ignored, and says so.
    //
    // This is the whole security model of the pane, so it is worth stating
    // plainly. Every other setting in agentty is live: change the theme and
    // the next frame is painted with it. A sandbox cannot work that way,
    // because the thing it configures is a BOUNDARY, and a boundary that can
    // move while the process runs has three problems no amount of care fixes:
    //
    //   1. It only ever gets WEAKER usefully. Tightening mid-session is
    //      fine but pointless (whatever already ran, ran); loosening is what
    //      an attacker wants, and a prompt-injected agent that can reach the
    //      settings reducer is exactly who would want it.
    //
    //   2. The weakening is INVISIBLE and IRREVERSIBLE. Switching claybin ->
    //      bwrap silently drops the seccomp filter, the cgroup caps AND the
    //      non-configurable secret masks -- measured: the same workspace
    //      .env reads back as zero bytes under claybin and `SECRET=leaked`
    //      under bwrap. Re-tightening afterwards does not un-read a key that
    //      already left.
    //
    //   3. Two commands in one session get two different boundaries, with
    //      nothing in the transcript saying which got which. That makes the
    //      sandbox unauditable, which is worse than a weaker sandbox that is
    //      at least one known thing.
    //
    // So the policy is fixed at startup, from the saved settings plus the
    // CLI, and the pane WRITES TO DISK for the next launch rather than
    // mutating the live one. Restart is the apply step. That is a real cost
    // -- you cannot try a tighter profile without relaunching -- and it buys
    // the only property that matters here: whatever confined the first
    // command in this session confines the last one too.
    //
    // Enforced in code, not by comment. It was a comment before ("call
    // before init()") and the pane broke it within a day of existing.
    if (g_cfg_sealed.exchange(true, std::memory_order_acq_rel)) {
        AGT_LOG(Tool, Warn, "sandbox.seal",
                "refused a live sandbox policy change: the boundary is sealed "
                "at startup and only a restart applies a new one");
        return;
    }

    // First call: publish the one snapshot this process will ever use.
    //
    // Still an immutable published snapshot rather than a plain global. The seal means it is written once, but it is READ from
    // tool worker threads while the UI thread reads it too, and
    // shared_ptr-to-const is how a reader gets a coherent view without a
    // lock. (It also kept a real use-after-free out of the tree back when
    // this was republishable: Config holds three vectors, and assigning one
    // under a concurrent reader is a freed-buffer walk.)
    auto next = std::make_shared<const sandbox_cfg::Config>(cfg);
    g_cfg.publish(std::move(next));

    // The policy carries the ENGINE, so sealing it seals the backend too.
    //
    // Only when the user has actually chosen: an unconfigured config holds
}

bool config_sealed() noexcept {
    return g_cfg_sealed.load(std::memory_order_acquire);
}

void reset_config_for_test() noexcept {
    // Order matters even here: drop the config first, then the seal, so a
    // concurrent reader never sees "unsealed but still holding the old
    // policy" -- which is the one state that would let a second set_config
    // land while a reader is mid-snapshot.
    (void)g_cfg.take();
    g_cfg_sealed.store(false, std::memory_order_release);
}

std::shared_ptr<const sandbox_cfg::Config> config_snapshot() noexcept {
    auto snap = g_cfg.current();
    // Never hand back null, so callers can dereference without checking. A
    // default config is the correct answer before anyone has set one: it is
    // exactly the posture the bwrap argv has always built.
    if (!snap) {
        static const auto kDefault =
            std::make_shared<const sandbox_cfg::Config>();
        return kDefault;
    }
    return snap;
}

sandbox_cfg::Config config() {
    // BY VALUE, deliberately, and this is the one place the cost is worth
    // arguing about.
    //
    // It used to return `const Config&` into a plain global. That reference
    // cannot be made safe now that the value is republished under readers:
    // handing out a reference into a snapshot means the snapshot's last
    // owner can drop while the caller still holds the reference, and the
    // caller has no way to know. A reference into shared, swappable state is
    // a dangling pointer with a delay on it.
    //
    // So the UI-facing accessor copies. Callers are form builders and status
    // lines -- a handful per keystroke, three small vectors each. Readers on
    // the hot path (the spawn path, once per command) use config_snapshot()
    // and pay nothing but a refcount.
    return *config_snapshot();
}

bool config_enforceable() noexcept {
    const auto cfg = config_snapshot();
    // Nothing configured ⇒ nothing to ignore. A default config is exactly the
    // posture the bwrap argv already builds, so both backends "enforce" it.
    if (!cfg->configured) return true;
#if MAYA_OS_LINUX || MAYA_OS_APPLE || MAYA_OS_WINDOWS
    // Off is not a failure to enforce, it is a choice not to sandbox, which
    // the banner already reports on its own.
    if (requested_mode() == Mode::Off) return true;
    return detected_backend() == Backend::Claybin;
#else
    return false;
#endif
}

bool is_active() noexcept {
    return requested_mode() != Mode::Off
        && detected_backend() != Backend::None;
}

bool workspace_is_home() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) return false;
    std::error_code ec;
    return std::filesystem::equivalent(workspace_root(), home, ec) && !ec;
}

std::vector<DegradedWall> degraded_walls() {
    if (!is_active()) return {};
    const auto d = g_posture_degraded.current();
    return d ? *d : std::vector<DegradedWall>{};
}

std::string degraded_reason() {
    const auto r = g_degraded_reason.current();
    return r ? *r : std::string{};
}

// One vocabulary for both audiences. The banner gets the short phrase, the
// model the consequence: "filesystem.write partial" is claybin's name for a
// fact the user and the model each need in their own words.
std::string degraded_phrase(const DegradedWall& w) {
    const bool none = w.strength == "none";
    if (w.wall == "network.isolation")
        return none ? "network: not isolated" : "network: partly isolated";
    if (w.wall == "process.isolation")
        return "other processes: visible";
    if (w.wall == "filesystem.write")
        return none ? "writes: not confined" : "protected files: tool gate only";
    if (w.wall == "filesystem.read")
        return none ? "reads: not confined" : "reads: partly confined";
    return w.wall + ": " + w.strength;
}

std::string degraded_meaning(const DegradedWall& w) {
    const bool none = w.strength == "none";
    if (w.wall == "network.isolation")
        return none ? "network isolation is unavailable on this host; commands share "
                      "your network, so do not probe local or private services"
                    : "network isolation is partial on this host (" + w.mechanism +
                          "); treat the network rules above as the policy even "
                          "where a connection gets through";
    if (w.wall == "process.isolation")
        return "process isolation is unavailable on this host; commands can see "
               "your other processes (ps, /proc), so do not inspect or signal them";
    if (w.wall == "filesystem.write")
        return none ? "write confinement is unavailable on this host; write only "
                      "inside the workspace"
                    : "files the host trusts inside the workspace (.git/hooks, "
                      ".git/config, .vscode/tasks.json and the like) are not "
                      "write-protected for commands on this host; do not change "
                      "them from the shell, and every change there is reported "
                      "to the user";
    if (w.wall == "filesystem.read")
        return none ? "read confinement is unavailable on this host; do not read "
                      "outside the workspace and your toolchain"
                    : "read confinement is partial on this host; treat paths "
                      "outside the workspace as off-limits even where a read "
                      "succeeds";
    return w.wall + " is " + w.strength + " on this host";
}

std::string describe_state() {
    auto m = requested_mode();
    auto b = detected_backend();
    if (m == Mode::Off) return "sandbox: off";
    const char* tag = nullptr;
    switch (b) {
        case Backend::Claybin:     tag = "claybin";      break;
        case Backend::SandboxExec: tag = "sandbox-exec"; break;
        case Backend::None:        tag = nullptr;        break;
    }
    if (tag) {
        // --workspace / rw-binds the whole filesystem: still wrapped,
        // but no filesystem containment. Be honest about it.
        std::error_code wec;
        auto ws = fs::weakly_canonical(workspace_root(), wec);
        if (wec) ws = workspace_root();
        if (ws == ws.root_path())
            return std::string{"sandbox: degraded ("} + tag
                 + ", --workspace / gives no filesystem containment)";
        // The walls this host could not build for the shipped posture, each
        // in words -- "active" alone was true on a host enforcing far less,
        // and "filesystem.write partial" told nobody what they had lost.
        std::string s = std::string{"sandbox: active ("} + tag + ")";
        const auto deg = degraded_walls();
        for (const auto& w : deg) s += " \xc2\xb7 " + degraded_phrase(w);
        if (const auto why = degraded_reason(); !deg.empty() && !why.empty())
            s += " (" + why + ")";
        return s;
    }
    // The engine exists but refused the posture we ship. Name the reason:
    // "no backend" sent people to install things that were already there.
    if (const auto err = g_posture_error.current(); err && !err->empty())
        return std::string{m == Mode::On ? "sandbox: requested but the posture cannot be built here ("
                                         : "sandbox: unavailable, running unsandboxed ("}
             + *err + ")";
    if (m == Mode::On)
        return "sandbox: requested but no backend "
#if MAYA_OS_LINUX
               "(the kernel offers neither landlock nor seccomp \xe2\x80\x94 "
               "nothing left to build a boundary from; run with --sandbox off)";
#elif MAYA_OS_APPLE
               "(sandbox-exec missing \xe2\x80\x94 system integrity issue)";
#elif MAYA_OS_WINDOWS
               "(no sandbox wall could be built on this host \xe2\x80\x94 "
               "appcontainer profiles unavailable and no job objects; "
               "run with --sandbox off)";
#else
               "(unsupported on this platform)";
#endif
    // Mode::Auto + no backend → falling through unsandboxed
    return "sandbox: unavailable, running unsandboxed "
#if MAYA_OS_LINUX
           "(the kernel offers neither landlock nor seccomp)";
#elif MAYA_OS_APPLE
           "(sandbox-exec missing)";
#elif MAYA_OS_WINDOWS
           "(no sandbox wall available: appcontainer unavailable)";
#else
           "(no backend on this platform)";
#endif
}

// Does this output look like the SANDBOX refused something?
//
// ── Why annotate at all ───────────────────────────────────────────
//
// A bare "Permission denied" is indistinguishable from a real bug, and a model
// reading it concludes the wrong thing with total confidence: it blames a
// corporate proxy, tells the user to check CrowdStrike, or retries the same
// denied call. Every one of those wastes a turn and none can work, because the
// boundary is a kernel decision.
//
// The system prompt says a sandbox exists (see provider/prompt.cpp), and that
// helps -- but it is thousands of tokens away from the error by the time the
// error arrives. Attaching the explanation to the OUTPUT puts it where the
// model is actually looking, at the moment it has to decide what to do next.
// That is the pattern mature sandbox runtimes converge on, and it is the half
// that stops retry loops.
//
// ── Why pattern-matching is sound here ──────────────────────────────
//
// It cannot be exact: the kernel denies, libc stringifies, and the program
// prints whatever it likes. A false POSITIVE costs one extra sentence on an
// error the model was already handling; a false NEGATIVE just leaves today's
// behaviour. Neither can corrupt anything, so matching broadly is the right
// trade -- which is also why this never claims the sandbox DID deny, only that
// it is the likely cause.
[[nodiscard]] bool looks_like_denial(std::string_view out) {
    static constexpr std::string_view kSigns[] = {
        "Permission denied",
        "Operation not permitted",
        "Read-only file system",
        "Network is unreachable",
        "Could not resolve host",
        "Temporary failure in name resolution",
        "Connection refused",
        "Bad system call",            // a seccomp kill, as the shell reports it
        // The sandbox proxy refusing a host or address. Every client says it
        // differently, and none of them says "sandbox": curl, git and pip
        // report a failed CONNECT tunnel, python a tunnel error, and both
        // show the 403 the proxy sent.
        "CONNECT tunnel failed",
        "Tunnel connection failed",
        "refused by the sandbox proxy",
        "Received HTTP code 403 from proxy",
        // macOS: opening apps or URLs needs Launch Services, which the
        // sandbox blocks, so `open .` reports a missing app instead.
        "No application knows how to open",
        "Unable to find application named",
        "LSOpenURLsWithRole",
    };
    for (auto s : kSigns)
        if (out.find(s) != std::string_view::npos) return true;
    return false;
}

std::vector<std::string> denied_paths(std::string_view out, std::string_view cwd) {
    static constexpr std::string_view kErrs[] = {
        "Permission denied", "Read-only file system", "Operation not permitted",
    };
    std::vector<std::string> paths;
    auto add = [&](std::string_view raw) {
        while (!raw.empty() && (raw.front() == ' ' || raw.front() == '"')) raw.remove_prefix(1);
        while (!raw.empty() && (raw.back() == ' ' || raw.back() == '"')) raw.remove_suffix(1);
        if (raw.empty() || raw.size() > 4096 || raw.find('\n') != std::string_view::npos) return;
        fs::path p{std::string{raw}};
        if (raw.front() == '~') {
            const char* h = std::getenv("HOME");
            if (!h || !*h || (raw.size() > 1 && raw[1] != '/')) return;
            p = fs::path{h} / std::string{raw.size() > 2 ? raw.substr(2) : std::string_view{}};
        } else if (p.is_relative()) {
            // A shell prefix ("bash", "line 3") is not a path; a relative
            // name counts if it exists where the command ran, or has a '/'
            // and its directory does (a file the command failed to create).
            if (cwd.empty()) return;
            p = fs::path{std::string{cwd}} / p;
            std::error_code ec;
            const bool there = fs::exists(fs::symlink_status(p, ec));
            const bool dir_there = raw.find('/') != std::string_view::npos
                && fs::is_directory(p.parent_path(), ec);
            if (!there && !dir_there) return;
        }
        std::string s = p.lexically_normal().string();
        if (std::ranges::find(paths, s) == paths.end()) paths.push_back(std::move(s));
    };
    std::size_t start = 0;
    while (start < out.size() && paths.size() < 8) {
        std::size_t end = out.find('\n', start);
        if (end == std::string_view::npos) end = out.size();
        const std::string_view line = out.substr(start, end - start);
        start = end + 1;
        for (auto err : kErrs) {
            const auto at = line.find(err);
            if (at == std::string_view::npos) continue;
            const auto before = line.substr(0, at);
            const auto after = line.substr(at + err.size());
            // python: "PermissionError: [Errno 13] Permission denied: '/x'"
            if (after.starts_with(": '")) {
                const auto q = after.find('\'', 3);
                if (q != std::string_view::npos) { add(after.substr(3, q - 3)); break; }
            }
            // coreutils: "touch: cannot touch '/x': Permission denied", or
            // ‘/x’ under a UTF-8 locale.
            if (before.ends_with("': ")) {
                const auto open = before.rfind('\'', before.size() - 4);
                if (open != std::string_view::npos) {
                    add(before.substr(open + 1, before.size() - 4 - open));
                    break;
                }
            }
            if (before.ends_with("\xe2\x80\x99: ")) {
                const auto open = before.rfind("\xe2\x80\x98");
                if (open != std::string_view::npos) {
                    add(before.substr(open + 3, before.size() - 5 - (open + 3)));
                    break;
                }
            }
            // shells and cat: "bash: line 1: /x: Permission denied" -- the
            // field between the last two ": ".
            if (before.ends_with(": ")) {
                const auto body = before.substr(0, before.size() - 2);
                const auto colon = body.rfind(": ");
                add(colon == std::string_view::npos ? body : body.substr(colon + 2));
            }
            break;
        }
    }
    return paths;
}

void record_denials(std::string_view out, std::string_view cwd) {
#if !MAYA_OS_WINDOWS
    for (const auto& p : denied_paths(out, cwd)) {
        // Could the host user have done it? If the file allows read+write,
        // or does not exist and its directory allows creating it, the
        // refusal was the box. Anything else may be the file's own mode, and
        // the feed records only what the sandbox stopped.
        bool host_could = false;
        std::error_code ec;
        if (fs::exists(fs::symlink_status(p, ec)))
            host_could = ::access(p.c_str(), R_OK | W_OK) == 0;
        else if (const auto dir = fs::path{p}.parent_path(); !dir.empty())
            host_could = ::access(dir.c_str(), W_OK | X_OK) == 0;
        if (!host_could) continue;
        broker::Event ev;
        ev.syscall = "file";
        ev.detail = p;
        ev.allowed = false;
        broker::record(ev);
        AGT_LOG(Tool, Info, "sandbox.denied", "file {}", logx::body(p));
    }
#else
    (void)out; (void)cwd;
#endif
}

// The note appended to a denied command's output.
//
// Phrased for the MODEL and kept short: it names the cause, forbids the two
// wrong moves (retry, blame the network), and says what to do instead. A note
// that only explained would still leave the model free to retry.
[[nodiscard]] std::string denial_note() {
    const auto cfg = config_snapshot();
    std::string n =
        "\n\n[sandbox] This command ran inside an OS-level sandbox, so a "
        "permission or network error above is most likely the sandbox "
        "refusing it \xe2\x80\x94 not a broken tool or the user's firewall. A "
        "proxy/tunnel/403 error is the sandbox's own network proxy.";
    if (cfg->net_mode == sandbox_cfg::NetMode::None)
        n += " Network access is BLOCKED in this session.";
    else if (cfg->net_mode == sandbox_cfg::NetMode::Ports)
        n += " Only specific ports are reachable in this session.";
    else if (!cfg->allow_hosts.empty()) {
        // Name the list: "a host was refused" sends the model hunting for a
        // mirror; the list tells it which hosts it CAN use, or what to ask for.
        n += " Only these hosts are reachable: ";
        for (std::size_t i = 0; i < cfg->allow_hosts.size() && i < 12; ++i)
            n += (i ? ", " : "") + cfg->allow_hosts[i];
        if (cfg->allow_hosts.size() > 12) n += ", \xe2\x80\xa6";
        n += ". Any other host, and every IP address, is refused.";
    } else
        n += " Private, loopback and cloud-metadata addresses are refused.";
    n += " The workspace is writable; most paths outside it are not.";
#if MAYA_OS_APPLE
    n += " Opening apps, Finder or URLs (open, osascript) needs macOS "
         "services the sandbox blocks.";
#endif
    n += " Approving a command, or the Write profile running it unasked, "
         "doesn't widen the sandbox. Do not retry the same command unchanged. Say "
         "the sandbox is the likely cause (not certain), then offer next "
         "steps: the user runs it in their own terminal, adds the folder in "
         "Settings > Sandbox (applies after a restart), or restarts agentty "
         "with --sandbox off for that run.";
    return n;
}

SubprocessResult run_shell_command(std::string_view cmd,
                                   std::size_t max_bytes,
                                   std::chrono::seconds timeout) {
    if (!is_active())
        return run_command_s(std::string{cmd}, max_bytes, timeout);
    auto r = run_wrapped(cmd, max_bytes, timeout);
    // Only on FAILURE, and only when the output looks like a wall. A note on
    // every command would be noise the model learns to skip, which is how an
    // explanation stops working.
    if (r.exit_code != 0 && looks_like_denial(r.output)) {
        record_denials(r.output, workspace_root().string());
        r.output += denial_note();
    }
    return r;
}

SubprocessResult run_shell_command_io(std::string_view cmd, std::string stdin_data,
                                      std::size_t max_bytes,
                                      std::chrono::seconds timeout) {
    if (!is_active()) {
        SubprocessOptions opts;
        opts.command      = SubprocessOptions::Shell{std::string{cmd}};
        opts.max_bytes    = max_bytes;
        opts.timeout      = timeout;
        opts.stdin_data   = std::move(stdin_data);
        opts.split_stderr = true;
        return Subprocess::run(std::move(opts));
    }
    return run_wrapped(cmd, max_bytes, timeout, std::move(stdin_data), true);
}

SubprocessResult run_argv(const std::vector<std::string>& argv,
                          std::size_t max_bytes,
                          std::chrono::seconds timeout) {
    if (!is_active())
        return run_argv_s(argv, max_bytes, timeout);
    auto r = run_wrapped_argv(argv, max_bytes, timeout);
    // Same annotation as the shell path: this is what hooks and ACP terminals
    // go through, and a hook that dies on a wall is just as easy to
    // misdiagnose as a bash call.
    if (r.exit_code != 0 && looks_like_denial(r.output)) {
        record_denials(r.output, workspace_root().string());
        r.output += denial_note();
    }
    return r;
}

claybin_backend::SpawnResult spawn_in_sandbox(
        const std::vector<std::string>& argv, std::string_view cwd, int out_fd,
        const std::vector<std::string>& extra_roots,
        const handoff::TrustedSnapshot* trusted,
        const std::vector<std::pair<std::string, std::string>>& env,
        int err_fd, int in_fd) {
#if MAYA_OS_LINUX || MAYA_OS_WINDOWS
    if (detected_backend() == Backend::Claybin) {
        return claybin_backend::spawn_argv(
            command_posture(join_argv(argv), cwd, extra_roots, trusted), argv, out_fd,
            err_fd >= 0 ? err_fd : out_fd, env, in_fd);
    }
#else
    (void)argv; (void)cwd; (void)out_fd; (void)extra_roots; (void)trusted; (void)env;
    (void)err_fd; (void)in_fd;
#endif
    claybin_backend::SpawnResult r;
    r.started = false;
    r.start_error = "no claybin backend active";
    return r;
}

#if MAYA_OS_APPLE
std::expected<std::vector<std::string>, std::string>
wrap_argv_for_exec(const std::vector<std::string>& argv, std::string_view cwd,
                   const std::vector<std::string>& extra_roots) {
    if (argv.empty()) return std::unexpected(std::string{"empty argv"});
    std::string profile;
    std::vector<std::string> proxy_env;
    if (detected_backend() == Backend::Claybin) {
        const auto posture = command_posture(join_argv(argv), cwd, extra_roots);
        std::string err;
        profile = claybin_backend::compile_profile(posture, err);
        if (profile.empty()) return std::unexpected(std::move(err));
        proxy_env = claybin_backend::proxy_env(posture);
    } else {
        profile = build_profile(workspace_root().string());
        // The fixed profile has no notion of extra roots; append them the
        // same way it writes the workspace (escaped, omitted if unsafe).
        for (const auto& r : extra_roots)
            if (std::string e = sbpl_escape(r); !e.empty())
                profile += "(allow file-write* (subpath \"" + e + "\"))\n";
    }
    std::vector<std::string> out{"/usr/bin/sandbox-exec", "-p", std::move(profile)};
    // The guest's only route out is the proxy port, so it has to be TOLD the
    // address: env(1) inside the sandbox, so it applies to exactly this
    // command and every tool it runs, and nothing in agentty's own env.
    if (!proxy_env.empty()) {
        out.emplace_back("/usr/bin/env");
        for (auto& kv : proxy_env) out.push_back(std::move(kv));
    }
    out.insert(out.end(), argv.begin(), argv.end());
    return out;
}
#endif

claybin_backend::Posture claybin_posture_for_test() {
#if MAYA_OS_LINUX || MAYA_OS_APPLE || MAYA_OS_WINDOWS
    return build_claybin_posture();
#else
    return {};
#endif
}

namespace {

// An ACP agent's own state under $HOME: where it keeps its sign-in, settings
// and history. The base posture MASKS several of these (they are credentials
// to every OTHER process), so the agent role has to give back exactly its
// own -- and nothing else's. Matched by the program's basename; an agent not
// listed gets no home state, which shows up as "please sign in", not a leak.
struct AgentHome { std::string_view bin; std::initializer_list<std::string_view> paths; };
constexpr AgentHome kAgentHomes[] = {
    {"claude",       {"/.claude", "/.claude.json", "/.config/claude"}},
    {"claude-code-acp", {"/.claude", "/.claude.json", "/.config/claude"}},
    {"codex",        {"/.codex"}},
    {"codex-acp",    {"/.codex"}},
    {"gemini",       {"/.gemini", "/.config/gcloud"}},
    {"devin",        {"/.devin", "/.config/devin", "/.local/share/devin"}},
    {"opencode",     {"/.opencode", "/.config/opencode", "/.local/share/opencode",
                      "/.local/state/opencode"}},
    {"goose",        {"/.config/goose", "/.local/share/goose"}},
};

[[nodiscard]] std::string_view base_name(std::string_view p) {
    if (auto s = p.find_last_of("/\\"); s != std::string_view::npos) p.remove_prefix(s + 1);
    return p;
}

// npx / uvx / pipx / bunx run their package from a cache they fill on first
// use. A stdio MCP server is very often `npx -y some-server`; without its
// cache writable it cannot start. These hold downloaded packages, not
// credentials (~/.npmrc, which can, stays masked).
constexpr std::string_view kRunnerCaches[] = {
    "/.npm/_npx", "/.npm/_cacache", "/.cache/uv", "/.cache/pip",
    "/.local/share/uv", "/.local/pipx", "/.bun/install/cache", "/.cache/node",
};

}  // namespace

claybin_backend::Posture posture_for(Role role, const std::vector<std::string>& argv) {
#if MAYA_OS_LINUX || MAYA_OS_APPLE || MAYA_OS_WINDOWS
    // Role::Command's posture, then what the role changes.
    auto p = command_posture(argv.empty() ? std::string{} : join_argv(argv));
#if MAYA_OS_WINDOWS
    const char* home_env = std::getenv("USERPROFILE");
#else
    const char* home_env = std::getenv("HOME");
#endif
    const std::string home = home_env ? home_env : "";
    auto give_back = [&](const std::string& path) {
        // un-mask a path this role owns, and let it write there.
        std::erase_if(p.masked, [&](const std::string& m) {
            return m == path || m.starts_with(path + "/");
        });
        std::error_code ec;
        if (fs::exists(fs::symlink_status(path, ec))) p.write_paths.push_back(path);
    };

    switch (role) {
        case Role::Command:
            break;
        case Role::McpServer:
        case Role::Agent:
        case Role::RagPeer: {
            // The program a server IS must be readable, wherever it lives: a
            // configured `python3 /opt/tools/my_server.py` or
            // `node ~/src/mcp/index.js` sits outside the workspace and the
            // system read set. Read + execute its directory (for sibling
            // imports), for argv[0] and for every argument that names an
            // existing file. Never writable, and never a masked path: a
            // config cannot hand a server ~/.ssh by naming it as an argument.
            auto masked_under = [&](const std::string& d) {
                for (const auto& m : p.masked)
                    if (d == m || d.starts_with(m + "/") || m.starts_with(d + "/")) return true;
                return false;
            };
            for (const auto& a : argv) {
                std::error_code ec;
                fs::path f{a};
                if (!f.is_absolute() || !fs::is_regular_file(f, ec)) continue;
                const fs::path dir = fs::canonical(f, ec).parent_path();
                if (ec || dir == dir.root_path()) continue;
                const std::string d = dir.string();
                if (!home.empty() && d == home) continue;   // never all of $HOME
                if (!masked_under(d)) p.read_paths.push_back(d);
            }
            break;
        }
    }
    switch (role) {
        case Role::Command:
            break;
        case Role::McpServer:
            // its package runner's cache, so `npx -y server` can install
            if (!home.empty())
                for (auto c : kRunnerCaches) {
                    const std::string path = home + std::string{c};
                    std::error_code ec;
                    fs::create_directories(path, ec);   // so the bind exists
                    if (!ec) p.write_paths.push_back(path);
                }
            break;
        case Role::Agent: {
            // its own home state, and the network: an agent is a client of
            // its vendor's API. Through the proxy, so the LAN and cloud
            // metadata stay refused even for an agent.
            if (p.net_mode != 1 /* None: the user's explicit choice wins */)
                p.net_mode = 0;
            if (!home.empty() && !argv.empty())
                for (const auto& a : kAgentHomes)
                    if (base_name(argv.front()) == a.bin)
                        for (auto rel : a.paths) give_back(home + std::string{rel});
            break;
        }
        case Role::RagPeer:
            p.workspace_read_only = true;
            p.write_paths.clear();
            p.net_mode = 1;   // None
            break;
    }
    // A server runs for the session; a command's wall-clock cap would kill it
    // mid-conversation. CPU-time stays: a spinning server is still a fault.
    if (role != Role::Command) p.wall_clock_secs = 0;
    // No broker: nothing polls a long-lived child's listener, and a brokered
    // ptrace/kill would block the server forever. The filter's flat deny
    // stands in (a server has no business debugging or signalling anyone).
    if (role != Role::Command) p.broker = false;
    return p;
#else
    (void)role;
    (void)argv;
    return {};
#endif
}

std::expected<std::optional<LongLived>, std::string>
spawn_long_lived(Role role, const std::vector<std::string>& argv,
                 const std::vector<std::pair<std::string, std::string>>& env,
                 const std::string& cwd, bool quiet_stderr) {
    if (argv.empty()) return std::unexpected(std::string{"empty argv"});
    // Off is the user's choice: spawn plainly, as before.
    if (requested_mode() == Mode::Off) return std::optional<LongLived>{};
#if MAYA_OS_LINUX
    if (!is_active() || detected_backend() != Backend::Claybin) {
        // Auto on a host with no usable sandbox: the banner already says
        // commands run unsandboxed, and the same goes for servers. On never
        // reaches here (init refused to start).
        return std::optional<LongLived>{};
    }
    auto posture = posture_for(role, argv);
    if (!cwd.empty()) posture.cwd = cwd;

    int in[2] = {-1, -1}, out[2] = {-1, -1};
    if (::pipe2(in, O_CLOEXEC) != 0 || ::pipe2(out, O_CLOEXEC) != 0) {
        for (int f : {in[0], in[1], out[0], out[1]}) if (f >= 0) ::close(f);
        return std::unexpected(std::string{"pipe: "} + std::strerror(errno));
    }
    // stderr: a server's logs are its own business. Quiet sends them to
    // /dev/null; otherwise they go where ours go (fd 2).
    int err_fd = 2;
    if (quiet_stderr) err_fd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    auto r = claybin_backend::spawn_argv(posture, argv, out[1], err_fd, env, in[0]);
    if (quiet_stderr && err_fd >= 0) ::close(err_fd);
    ::close(in[0]);
    ::close(out[1]);
    if (!r.started) {
        ::close(in[1]);
        ::close(out[0]);
        // FAIL CLOSED: the caller must not retry this child unsandboxed.
        return std::unexpected("sandbox (" + std::string{to_string(role)} + "): "
                               + r.start_error);
    }
    AGT_LOG(Tool, Info, "sandbox.long_lived", "role={} pid={} argv0={}",
            to_string(role), r.pid, argv.front());
    LongLived ll;
    ll.pid = r.pid;
    ll.pidfd = r.pidfd;
    ll.stdin_fd = in[1];
    ll.stdout_fd = out[0];
    ll.keepalive = r.net_proxy;
    ll.kill_tree = std::move(r.kill_tree);
    if (r.supervisor_fd >= 0) ::close(r.supervisor_fd);   // posture_for: no broker
    return std::optional<LongLived>{std::move(ll)};
#else
    // macOS / Windows: long-lived children are not sandboxed yet. Said in
    // SANDBOX.md and the Plugins pane rather than silently pretended.
    (void)role; (void)env; (void)cwd; (void)quiet_stderr;
    return std::optional<LongLived>{};
#endif
}

std::function<std::optional<::agentty::util::ChildProcess::Spawn::Adopt>(
    const ::agentty::util::ChildProcess::Spawn&)>
child_spawner(Role role) {
    using Spawn = ::agentty::util::ChildProcess::Spawn;
    return [role](const Spawn& s) -> std::optional<Spawn::Adopt> {
        std::vector<std::string> argv;
        argv.reserve(s.args.size() + 1);
        argv.push_back(s.command);
        argv.insert(argv.end(), s.args.begin(), s.args.end());
        std::vector<std::pair<std::string, std::string>> env;
        for (const auto& kv : s.env_kv)
            if (const auto eq = kv.find('='); eq != std::string::npos)
                env.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
        auto r = spawn_long_lived(role, argv, env, s.cwd, s.quiet_stderr);
        if (!r) throw std::runtime_error(r.error());   // fail closed
        if (!*r) return std::nullopt;                  // sandbox off: plain spawn
        Spawn::Adopt a;
        a.pid       = (*r)->pid;
        a.pidfd     = (*r)->pidfd;
        a.stdin_fd  = (*r)->stdin_fd;
        a.stdout_fd = (*r)->stdout_fd;
        a.keepalive = std::move((*r)->keepalive);
        return a;
    };
}

bool command_needs_jit(std::string_view cmd) noexcept {
    static constexpr std::string_view kJit[] = {
        "node", "nodejs", "npm", "npx", "pnpm", "yarn", "corepack", "deno",
        "bun", "bunx", "tsx", "ts-node", "vite", "next", "jest", "vitest",
        "eslint", "tsc", "prettier", "electron", "java", "javac", "gradle",
        "gradlew", "mvn", "mvnw", "kotlin", "kotlinc", "sbt", "scala",
        "clojure", "lein", "dotnet", "luajit", "pypy", "pypy3", "julia",
    };
    // run wrappers: the command word comes after them and their options.
    static constexpr std::string_view kWrap[] = {
        "env", "exec", "command", "builtin", "time", "nice", "nohup", "timeout",
        "stdbuf", "ionice", "setsid", "taskset", "xargs", "then", "do", "else",
        "!", "{",
    };
    auto base = [](std::string_view w) {
        if (auto s = w.find_last_of("/\\"); s != std::string_view::npos) w = w.substr(s + 1);
        for (std::string_view ext : {".exe", ".cmd", ".bat"})
            if (w.size() > ext.size() && w.ends_with(ext)) w.remove_suffix(ext.size());
        return w;
    };
    auto is_jit = [&](std::string_view w) {
        w = base(w);
        // versioned names: node22, java17, pypy3.10
        while (!w.empty() && ((w.back() >= '0' && w.back() <= '9') || w.back() == '.' || w.back() == '-'))
            w.remove_suffix(1);
        for (auto k : kJit)
            if (w == k) return true;
        return false;
    };

    // Split into words (quotes removed) and operators. Quoted text is data,
    // so `echo "node"` never reaches command position.
    struct Tok { std::string text; bool op; };
    std::vector<Tok> toks;
    std::string cur;
    bool have = false;
    auto flush = [&] { if (have) toks.push_back({cur, false}); cur.clear(); have = false; };
    // sh escapes with backslash; cmd.exe (Windows) treats it as a path char.
#if MAYA_OS_WINDOWS
    constexpr bool kBackslashEscapes = false;
#else
    constexpr bool kBackslashEscapes = true;
#endif
    for (std::size_t i = 0; i < cmd.size(); ++i) {
        const char c = cmd[i];
        if (kBackslashEscapes && c == '\\' && i + 1 < cmd.size()) { cur += cmd[++i]; have = true; }
        else if (c == '\'') {
            have = true;
            while (++i < cmd.size() && cmd[i] != '\'') cur += cmd[i];
        } else if (c == '"') {
            have = true;
            while (++i < cmd.size() && cmd[i] != '"') {
                if (cmd[i] == '\\' && i + 1 < cmd.size()) ++i;
                cur += cmd[i];
            }
        } else if (c == ' ' || c == '\t') flush();
        else if (c == ';' || c == '&' || c == '|' || c == '(' || c == ')' || c == '`'
                 || c == '\n') {
            flush();
            toks.push_back({std::string(1, c), true});
        } else if (c == '$' && i + 1 < cmd.size() && cmd[i + 1] == '(') {
            flush();
            toks.push_back({"$(", true});
            ++i;
        } else { cur += c; have = true; }
    }
    flush();

    bool at_cmd = true, after_wrap = false;
    for (std::size_t k = 0; k < toks.size(); ++k) {
        const auto& t = toks[k];
        if (t.op) { at_cmd = true; after_wrap = false; continue; }
        if (!at_cmd) continue;
        const std::string_view w = t.text;
        // FOO=1 cmd
        if (auto eq = w.find('='); eq != std::string_view::npos && eq > 0
                && w.substr(0, eq).find('/') == std::string_view::npos)
            continue;
        // wrapper options and arguments: -n 10, 30s
        if (after_wrap && !w.empty() && (w[0] == '-' || (w[0] >= '0' && w[0] <= '9')))
            continue;
        const auto b = base(w);
        if (std::find(std::begin(kWrap), std::end(kWrap), b) != std::end(kWrap)) {
            after_wrap = true;
            continue;
        }
        // sh -c '...': the script is its own command line
        if (b == "sh" || b == "bash" || b == "dash" || b == "zsh" || b == "ash") {
            for (std::size_t m = k + 1; m < toks.size() && !toks[m].op; ++m)
                if (toks[m].text == "-c" || (toks[m].text.size() > 1 && toks[m].text[0] == '-'
                                            && toks[m].text.back() == 'c')) {
                    if (m + 1 < toks.size() && !toks[m + 1].op
                        && command_needs_jit(toks[m + 1].text))
                        return true;
                    break;
                }
        }
        if (is_jit(w)) return true;
        at_cmd = false;
        after_wrap = false;
    }
    return false;
}

} // namespace agentty::tools::util::sandbox
