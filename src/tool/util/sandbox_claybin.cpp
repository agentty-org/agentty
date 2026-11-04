// agentty::tools::util::sandbox — the claybin backend.
//
// Same boundary as the bwrap backend, built through claybin's library API
// instead of by exec'ing a binary. Kept in its own translation unit so the
// bwrap path is untouched: bwrap has a decade of hardening and demonstrably
// works on users' machines, claybin is days old, and the choice between them
// belongs to the user at runtime rather than to whoever edits this file.
//
// WHY BOTH, concretely:
//   - no `bwrap` on PATH requirement, so a host without bubblewrap installed
//     still gets a sandbox.
//   - three walls bwrap gives us nothing for today, because we pass it no
//     --seccomp fd and no cgroup: a syscall filter, landlock as a second
//     filesystem wall, and real cgroup2 memory/pids caps.
//   - a guarantee report, so `describe_state()` can say WHICH walls are up
//     rather than the word "active".
//
// WHAT MUST NOT DIVERGE: the read set. kSystemReadRoots, kEtcReadable and
// kHomeToolSubdirs live in sandbox.cpp and are consumed here by reference --
// NOT restated. A previous backend restated them by hand and ended up granting
// read on `/`, which handed ~/.ssh and ~/.aws to any approved bash call while
// still reporting "sandbox: active". One list, two backends.
#include "agentty/tool/util/sandbox_claybin.hpp"
#include "agentty/tool/util/sandbox_broker.hpp"
#include "agentty/util/logx.hpp"   // AGT_LOG — the broker audit trail

// POSIX, not Linux-only. claybin's plan compiler is portable, and as of the
// macOS and Windows backends there are now three platforms that can APPLY a
// policy:
//
//   Linux  — namespaces, landlock, seccomp, cgroup2  (claybin/plan/spawn.hpp)
//   macOS  — seatbelt + rlimits                      (claybin/macos/backend.hpp)
//   Windows— appcontainer, restricted token, job,   (claybin/windows/spawn.cpp)
//            mitigation policies
//
// `build_policy()` below is shared between them, which is the entire point of
// claybin's design: one description of the boundary, compiled per platform,
// with a guarantee report saying what each host could actually enforce. The
// three spawn paths differ because the mechanisms differ; the POLICY does not.
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <map>
// Darwin's <unistd.h> doesn't declare environ (glibc does under _GNU_SOURCE).
extern "C" char** environ;
#endif

#include "claybin/plan/compile.hpp"
#include "claybin/policy/policy.hpp"
#include "claybin/policy/profiles.hpp"

#if defined(__linux__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include "claybin/broker/notify.hpp"
#include "claybin/plan/spawn.hpp"
#elif defined(__APPLE__)
#include "claybin/macos/backend.hpp"
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "claybin/windows/backend.hpp"
#endif

namespace agentty::tools::util::sandbox::claybin_backend {

namespace {

using namespace ::clay;
using namespace ::clay::literals;

// Where the guest's proxy listens, inside its OWN network namespace.
//
// A FIXED port, chosen by us rather than the kernel: the guest's environment
// (http_proxy and friends) has to name the address before exec, and the
// namespace does not exist until setup runs. Nothing can collide with it --
// a fresh namespace has no other sockets at all.
constexpr std::uint16_t kProxyPort = 3128;   // the squid port, for familiarity

// The memory THROTTLE to apply when the user configured no hard cap.
//
// WHY A DEFAULT IS POSSIBLE HERE AND NOT FOR memory.max. A hard cap has to be
// right: too low and a legitimate `cargo build` is OOM-killed, which looks
// like agentty breaking the build, and there is no number that fits both an
// 8 GB laptop and a 128 GB workstation. memory.high is different in kind --
// crossing it puts the guest under reclaim pressure, so the pathological case
// crawls while a normal build that briefly peaks is merely slowed. Nothing
// dies, so a wrong guess costs throughput rather than correctness.
//
// 75% of total RAM: high enough that no real build notices (a link step
// peaking at 12 GB on a 32 GB box is under it), low enough that the host
// keeps a quarter of its memory for the editor, the browser and agentty
// itself. Derived from the machine rather than hardcoded, because that is the
// part the original "no number works for someone else's laptop" objection
// got right.
//
// 0 = could not read the machine's memory, so apply nothing.
[[nodiscard]] std::uint64_t default_memory_high_bytes() noexcept {
#if defined(__linux__)
    const long pages = ::sysconf(_SC_PHYS_PAGES);
    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (pages <= 0 || page_size <= 0) return 0;
    const auto total = static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page_size);
    // Below ~2 GB the arithmetic stops being a kindness: leave it alone and
    // let the OOM killer's score (oom_score_adj=500, set below) do the work.
    if (total < (2ull << 30)) return 0;
    return total / 4 * 3;
#else
    return 0;
#endif
}

}  // namespace

// What the guest inherits from agentty's own environment. A denylist, not an
// allowlist: builds read arbitrary vars (CC, GOPATH, JAVA_HOME, npm_config_*)
// and an allowlist breaks the next toolchain nobody listed. What goes is the
// stuff that is either a door out of the box or a credential agentty itself
// holds:
//   - session and desktop sockets: dbus (systemd-run, secret service),
//     wayland/X11 (keystrokes, screenshots), the ssh/gpg agents, tmux, kitty
//     and hyprland control sockets. most also point at a path the box can't
//     see, but the address alone leaks host layout, and an abstract socket
//     needs no path at all.
//   - provider keys agentty uses to talk to the model. a command the agent
//     runs has no business with the key that pays for the agent.
bool box_env_keeps(std::string_view key) noexcept {
    static constexpr std::string_view kDrop[] = {
        "DBUS_SESSION_BUS_ADDRESS", "DBUS_SYSTEM_BUS_ADDRESS",
        "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "DISPLAY", "XAUTHORITY",
        "SSH_AUTH_SOCK", "SSH_AGENT_PID", "GPG_AGENT_INFO",
        "TMUX", "TMUX_PANE", "KITTY_LISTEN_ON", "KITTY_PUBLIC_KEY", "KITTY_WINDOW_ID",
        "HYPRLAND_INSTANCE_SIGNATURE", "SWAYSOCK", "I3SOCK", "NIRI_SOCKET",
        "XDG_SESSION_COOKIE", "GNOME_KEYRING_CONTROL", "KDE_SESSION_UID",
        "AT_SPI_BUS_ADDRESS", "PULSE_SERVER", "PIPEWIRE_REMOTE",
        "ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN", "CLAUDE_CODE_OAUTH_TOKEN",
        "OPENAI_API_KEY", "AGENTTY_API_KEY",
    };
    for (auto d : kDrop)
        if (key == d) return false;
    // Every *_API_KEY agentty's provider registry reads (GROQ_API_KEY,
    // GEMINI_API_KEY, ...). Matched by suffix so a new provider is covered.
    if (key.size() > 8 && key.ends_with("_API_KEY")) return false;
    return true;
}

#if defined(__APPLE__)

// The darwin proxy. ONE per agentty process, not one per command as on Linux:
// macOS guests run through sandbox-exec in the argv (no adopt, see
// run_shell_command), so there is no per-spawn lifetime to hang a proxy on.
// It listens on a KERNEL-chosen loopback port, unlike Linux's fixed 3128:
// here the guest shares the host's loopback, where a fixed port could already
// belong to someone else -- and whoever owned it would be the guest's
// gateway. Leaked on purpose: tearing down a WorkerGroup during static
// destruction would race the logger it reports to.
//
// KEYED BY ALLOWLIST: the list is fixed per proxy (read lock-free by its
// worker), and the user can edit it in settings mid-session. Each distinct
// list gets its own proxy, so a command always runs against the list in
// force when it STARTED -- in practice one or two lists per process.
//
// The registry is a maya::snapshot, not a mutex (the concurrency banlist
// keeps raw locks out of agentty): read lock-free, published with update().
// Two commands racing on a NEW list may each start a proxy; update() keeps
// the first one published, and the loser's is an idle listener for the
// rest of the process -- one socket, once, in a race that needs the list to
// change between two concurrent spawns. Cheaper than a lock on every spawn.
using ProxyPorts = std::map<std::vector<std::string>, std::uint16_t>;
maya::snapshot<ProxyPorts>& proxy_ports() {
    static maya::snapshot<ProxyPorts> s;
    return s;
}

std::uint16_t macos_proxy_port(const std::vector<std::string>& allow_hosts) {
    {
        const auto cur = proxy_ports().load();
        if (auto it = cur->find(allow_hosts); it != cur->end()) return it->second;
    }
    const std::uint16_t port = [&] () -> std::uint16_t {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return 0;
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        socklen_t len = sizeof a;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 ||
            ::listen(fd, 128) != 0 ||
            ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) != 0) {
            ::close(fd);
            return 0;
        }
        const std::uint16_t p = ntohs(a.sin_port);
        auto* proxy = new netproxy::Proxy{};   // leaked, see above
        if (!proxy->start(fd, allow_hosts)) {
            AGT_LOG(Tool, Warn, "sandbox.proxy.start_failed", "darwin port={}", p);
            return 0;
        }
        AGT_LOG(Tool, Debug, "sandbox.proxy.up", "darwin port={} hosts={}", p,
                allow_hosts.size());
        return p;
    }();
    // a failure is NOT cached: the next command retries rather than every
    // command for the rest of the session running with no network.
    if (port == 0) return 0;
    const auto published = proxy_ports().update(
        [](const ProxyPorts& old, const std::vector<std::string>& key, std::uint16_t p) {
            ProxyPorts next = old;
            next.emplace(key, p);   // emplace keeps an existing entry: first publish wins
            return next;
        },
        allow_hosts, port);
    const auto it = published->find(allow_hosts);
    return it != published->end() ? it->second : port;
}

std::vector<std::string> proxy_env(const Posture& p) {
    std::vector<std::string> out;
    if (p.net_mode != 0 /* Full */) return out;
    const auto port = macos_proxy_port(p.allow_hosts);
    if (port == 0) return out;
    const std::string addr = "http://127.0.0.1:" + std::to_string(port);
    for (const char* k : {"http_proxy", "https_proxy", "HTTP_PROXY",
                          "HTTPS_PROXY", "ALL_PROXY", "all_proxy"})
        out.push_back(std::string{k} + "=" + addr);
    // node's built-in fetch/undici ignores *_proxy unless told (node >= 24);
    // without this every `fetch` in a script dies with ENOTFOUND, since the
    // guest has no DNS. npm itself reads the vars and needs nothing.
    out.emplace_back("NODE_USE_ENV_PROXY=1");
    // an inherited no_proxy=* or =github.com would send those hosts DIRECT,
    // which the seatbelt rule refuses: a confusing failure, never a bypass.
    out.emplace_back("no_proxy=");
    out.emplace_back("NO_PROXY=");
    return out;
}

#endif

namespace {

// Build the sealed policy that mirrors build_bwrap_argv()'s posture.
//
// Order matters exactly as it does for bwrap: the workspace bind goes LAST so
// that a workspace living under /tmp is not shadowed by the /tmp tmpfs mounted
// for it. claybin's mount plan is applied in declaration order for the same
// reason bubblewrap's is.
[[nodiscard]] Policy<Sealed> build_policy(const Posture& p) {
    auto d = Policy<Draft>{};

    // ── the read set, from the SHARED lists ───────────────────────────────
    for (const auto& root : p.system_read_roots) {
        // /usr and /bin must exist for a shell to run at all; the rest are
        // best-effort, matching --ro-bind-try.
        if (root == "/usr" || root == "/bin")
            d = std::move(d).ro_bind(root, root);
        else
            d = std::move(d).bind_try(root, root);
    }
    for (const auto& f : p.etc_readable) d = std::move(d).bind_try(f, f);
    for (const auto& h : p.home_tool_dirs) d = std::move(d).bind_try(h, h);
    // Extra grants from the pane. read_paths are read-only; write_paths are
    // separate because granting write is a different decision from granting
    // read, and one combined list would make the dangerous one the easy one.
    for (const auto& r : p.read_paths) d = std::move(d).bind_try(r, r);
    // write_paths are bound after the /tmp tmpfs below, like the workspace:
    // one under /tmp would otherwise be covered by it.

    // ── secrets, masked with MOUNTS ──────────────────────────────────────
    //
    // A mount, not a landlock deny, and that distinction cost me an
    // afternoon. landlock has no negative rule: add_rule() with zero rights
    // is ENOMSG (measured on abi 10), and omitting the rule means the path
    // INHERITS its ancestor's grant. So "bind $HOME, deny $HOME/.aws" is not
    // expressible in landlock at all -- I wrote it, watched the rule reach
    // the plan, and read the credentials anyway.
    //
    // What DOES work is making the path not be the file: bind something empty
    // over it. tmpfs for a directory, an empty file for a file. The guest
    // sees an empty ~/.aws rather than a denied one, which is also the better
    // failure mode -- a tool that reads it gets no credentials instead of an
    // EACCES it might report as a bug.
    //
    // bind_try semantics: a secret the user does not have is not an error,
    // and mask() is optional by default for exactly that reason.
    for (const auto& m : p.masked) d = std::move(d).mask(m);

    // ── pseudo-filesystems ───────────────────────────────────────────
    // A fresh procfs shows only our own pid namespace. dev_fs binds the handful
    // of device nodes a program needs rather than mounting devtmpfs, so
    // /dev/mem and friends are ABSENT rather than present-and-denied.
    d = std::move(d).proc_fs("/proc");
    d = std::move(d).dev_fs("/dev");

    // ── /tmp, then the workspace ───────────────────────────────────────
    // A tmpfs so nothing leaks into the host /tmp, and SIZED: claybin enforces
    // it as a mount option, so a runaway build hits ENOSPC inside the sandbox
    // instead of filling the host's memory. bwrap cannot express this.
    d = std::move(d).tmpfs("/tmp", Bytes{p.tmp_bytes});
    for (const auto& w : p.write_paths) {
        d = std::move(d).bind_try(w, w, false);
        // Same ordering rule as the workspace below: masks under a fresh
        // bind have to be applied again or the bind uncovers them.
        for (const auto& m : p.masked)
            if (m.starts_with(w)) d = std::move(d).mask(m);
    }
    if (!p.workspace.empty()) {
        d = std::move(d).bind(p.workspace, p.workspace);
        d = std::move(d).workdir(p.cwd.empty() ? p.workspace : p.cwd);

        // Re-apply any mask that lives INSIDE the workspace, because the bind
        // above just remounted over it.
        //
        // This is an ordering bug I shipped and then measured: masks are
        // applied at line ~94, the workspace bind lands here, and a bind of
        // the workspace root covers every mount underneath it. So
        // `<workspace>/.env` was masked and then un-masked one line later,
        // and a planted .env came back readable IN FULL under both backends.
        // Every table-level check passed the whole time -- the mask really
        // was in the policy, it just lost a race with a later mount.
        //
        // Order is the only fix: a mask must come after every bind that could
        // cover it. The $HOME masks are unaffected (nothing rebinds $HOME),
        // which is why ~/.ssh was safe and .env was not.
        for (const auto& m : p.masked) {
            if (m.starts_with(p.workspace)) d = std::move(d).mask(m);
        }

        // Then the handoff binds, for exactly the same ordering reason: a
        // read-only bind of <workspace>/.vscode/tasks.json is covered by the
        // workspace bind above, so it has to come after. Same bug shape as
        // the masks, same fix.
        //
        // bind_try with ro=true: TRY because a path the repo does not have is
        // not an error (most repos have no .vscode), and RO because the guest
        // must still READ these -- git reads its own config on every
        // invocation. A write gets EROFS, which is a truthful error rather
        // than the confusing EACCES a permission denial would give.
        //
        // This is prevention; detection (handoff::snapshot_trusted) runs
        // regardless, because this wall needs a mount namespace and is simply
        // absent on a host that denies unprivileged userns.
        for (const auto& ro : p.handoff_ro) {
            if (ro.starts_with(p.workspace))
                d = std::move(d).bind_try(ro, ro, /*ro=*/true);
        }

        // Masks go last, after every bind. build_claybin_posture already
        // keeps masked paths out of handoff_ro; this makes the order itself
        // safe too, so no later bind can uncover a mask again.
        if (!p.handoff_ro.empty())
            for (const auto& m : p.masked)
                if (m.starts_with(p.workspace)) d = std::move(d).mask(m);
    }

    // ── network ─────────────────────────────────────────────────────────
    //
    // Three modes:
    //
    //   Full  the guest may reach the internet, but through its OWN network
    //         namespace and a loopback proxy this process serves. It used to
    //         mean "share agentty's namespace", which is not a boundary at
    //         all: the sandbox audit's one High finding was that a command
    //         the agent ran could reach the host's own loopback services and
    //         the whole LAN. Now the kernel gives the box no route off its
    //         own loopback, and sandbox_netproxy decides what each connection
    //         may dial (no host loopback, no RFC1918, no link-local metadata,
    //         no CGNAT/tailnet). `strong`.
    //   None  no grant at all, so claybin leaves the netns empty. `strong`.
    //   Ports one grant per port, enforced by landlock (abi 4+). `strong`,
    //         and the thing no proxy-based allowlist can claim: an agent
    //         cannot be talked into bypassing a kernel rule. Shares the host
    //         namespace, because a port rule is about reaching real hosts.
    switch (p.net_mode) {
        case 1: break;  // None
        case 2:
            for (auto port : p.allow_ports) d = std::move(d).connect("", port);
            break;
        default:
#if defined(__APPLE__)
            // No private netns on darwin: the guest is pinned by seatbelt to
            // ONE loopback port, which agentty's own proxy serves. If that
            // proxy could not start, fail CLOSED (no network) -- compiling
            // the bare connect("", 0) would be (allow network*), i.e. the
            // exact hole the proxy exists to close.
            if (const auto port = macos_proxy_port(p.allow_hosts); port != 0)
                d = std::move(d).connect("", 0).proxied_netns(port);
#else
            d = std::move(d).connect("", 0).proxied_netns(kProxyPort);
#endif
            break;  // Full
    }

    // ── process hardening ──────────────────────────────────────────────
    // new_session: the child cannot steal the controlling tty (bwrap's
    //   --new-session, and why TIOCSTI injection is unreachable).
    // die_with_parent: no detached zombies if agentty dies.
    d = std::move(d).new_session();
    d = std::move(d).die_with_parent();

    // ── the walls bwrap is given none of ────────────────────────────
    //
    // The syscall filter, plus any brokered calls.
    //
    // Built as one SyscallPolicy and applied ONCE, because a draft does not
    // expose its syscall policy for amendment -- `syscall_profile()` replaces
    // wholesale. So the notify rules have to be layered on here, before the
    // policy is handed over, rather than added afterwards.
    SyscallPolicy sp = [&] {
        switch (p.syscall_mode) {
            // Off is everything(), NOT "no profile": a default-constructed
            // SyscallPolicy is kill-by-default with no rules, which compiles to
            // a sandbox that kills the guest at execve. claybin refuses it
            // outright, and the settings pane's live preview caught that the
            // first time it ran.
            case 0: return SyscallPolicy::everything();
            // Strict is compiler() when there is no network, so a shell can
            // still exec. with_filesystem() has no execve and killed every
            // command. With network on it needs the socket calls.
            case 2: return p.net_mode == 1 ? profiles::compiler()
                                           : profiles::compiler_with_network();
            default: return profiles::compiler_with_network();
        }
    }();

    // The profiles always deny RWX mappings. With the filter off, add that
    // one rule when W^X is on.
    //
    // And on STRICT only, make it real: PR_SET_MDWE denies the
    // mmap(W)->mprotect(X) transition, which the argument rule cannot see
    // (measured: that sequence succeeded with the arg rule alone). It is
    // gated on Strict because it breaks every JIT -- node/V8 will not start
    // under it -- and Balanced has to run a normal toolchain. Hardened and
    // Airgapped already accept a build failing over an unusual syscall, so
    // this is the same trade they were chosen for.
    if (p.wx_protect && p.syscall_mode == 2 /* Strict */)
        d = std::move(d).deny_write_execute();
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#if defined(__x86_64__)
    constexpr clay::SysNr kMmap = 9, kMprotect = 10;
#else
    constexpr clay::SysNr kMmap = 222, kMprotect = 226;
#endif
    constexpr std::uint64_t kProtWriteExec = 0x2 | 0x4;
    if (p.syscall_mode == 0 && p.wx_protect) {
        sp.deny_arg_all(kMmap, 2, kProtWriteExec, clay::SysAction::errno_, 1);
        sp.deny_arg_all(kMprotect, 2, kProtWriteExec, clay::SysAction::errno_, 1);
    }
    // W^X off: lift the profile's rule too, or node/V8 can't start.
    if (p.syscall_mode != 0 && !p.wx_protect) {
        sp.drop_arg_rules(kMmap, 2);
        sp.drop_arg_rules(kMprotect, 2);
    }
#endif

    // Brokered syscalls: decided at runtime by a supervisor rather than by the
    // filter. Layered on AFTER the profile, because the profile sets a flat
    // action for these and brokering has to win.
    //
    // Only when the caller actually wired a supervisor. A policy that brokers
    // with nobody listening HANGS the guest -- the kernel blocks the thread
    // until someone answers -- so this is gated on `p.broker` rather than being
    // unconditional. Getting that backwards turns a hardening feature into a
    // deadlock on every ptrace.
    //
    // Also skipped under syscall_mode Off: everything() is "no filter", and
    // adding notify rules to it would mean brokering on a sandbox the user
    // asked not to filter at all.
    //
    // Linux-only: brokering is built on seccomp user-notify, which macOS has
    // no equivalent of. Compiled out rather than no-op'd at runtime, so a
    // future reader does not have to work out whether `p.broker` means
    // something different on darwin -- there, it means nothing at all, and the
    // guarantee report says `syscall.filter: none` to match.
#if defined(__linux__)
    if (p.broker && p.syscall_mode != 0) {
        for (auto nr : broker::brokered_syscalls()) sp.notify(nr);
    }
#endif

    // socket(): only the address families real tools use. AF_NETLINK reads
    // the host's routes, links and addresses (the host netns is shared under
    // Full), and AF_VSOCK reaches a hypervisor -- neither is something a
    // build or a test needs. The family is arg 0, a 32-bit int.
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
    if (p.syscall_mode != 0) {
        static constexpr std::uint32_t kFamilies[] = {
            1,   // AF_UNIX
            2,   // AF_INET
            10,  // AF_INET6
        };
#if defined(__x86_64__)
        constexpr clay::SysNr kSocket = 41;
#else
        constexpr clay::SysNr kSocket = 198;
#endif
        sp.allow_arg32_only(kSocket, 0 /* family */,
                            std::span<const std::uint32_t>{kFamilies},
                            clay::SysAction::errno_, 97 /* EAFNOSUPPORT */);
    }
#endif
    d = std::move(d).syscall_profile(std::move(sp));

    // Resource caps. cgroup2 when the host delegates, rlimit as a backstop;
    // the report distinguishes the two rather than claiming both.
    if (p.memory_bytes) d = std::move(d).memory(Bytes{p.memory_bytes});
    else if (const std::uint64_t high = default_memory_high_bytes(); high)
        // No hard cap configured. A throttle still is safe to default,
        // because it cannot kill a build -- see default_memory_high_bytes.
        d = std::move(d).memory_high(Bytes{high});
    if (p.max_processes) d = std::move(d).processes(p.max_processes);
    if (p.cpu_percent) d = std::move(d).cpu_percent(p.cpu_percent);
    else
        // No quota configured. A WEIGHT is still safe to default: it changes
        // nothing on an idle machine (the guest gets every core), and under
        // contention the guest loses to anything at the default 100 -- your
        // editor, your browser, agentty itself. So a 12-way build can no
        // longer make the desktop unusable, without ever slowing a build that
        // is the only thing running. A quota could not make that trade.
        d = std::move(d).cpu_weight(25);
    // Descriptors: a separate lever from processes, because a runaway that
    // leaks fds exhausts the host's file table without ever forking.
    if (p.max_open_files) d = std::move(d).open_files(p.max_open_files);
    // Two different ceilings, deliberately both available. cpu_time bounds
    // total compute, so a process that sleeps forever is untouched; wall_clock
    // bounds elapsed time, so one that blocks forever is not. Neither
    // subsumes the other, and the tool layer's own timeout is a third thing
    // (it can be ignored by a child that traps SIGTERM -- these cannot).
    if (p.cpu_secs)
        d = std::move(d).cpu_time(Nanos{std::uint64_t{p.cpu_secs} * 1'000'000'000ull});
    if (p.wall_clock_secs)
        d = std::move(d).wall_clock(Nanos{std::uint64_t{p.wall_clock_secs} * 1'000'000'000ull});

    // Hostname. Not containment -- the guest cannot escalate either way -- but
    // the real host name leaks into build output, test snapshots and anything
    // that shells out to `hostname`, which makes those non-reproducible and
    // mildly fingerprintable. claybin already defaults its PolicyData to
    // "sandbox"; this is the row that lets a user keep the real one when a
    // build genuinely depends on it. claybin defaults to "sandbox", so off
    // has to clear it; an empty name skips sethostname and the new UTS
    // namespace keeps the host's own name.
    d = std::move(d).hostname(p.fake_hostname ? "sandbox" : "");

    // When the OOM killer has to pick, it should pick the sandboxed build,
    // not the user's editor or agentty itself. Balanced has no memory cap, so
    // this is the one thing standing between a runaway build and the desktop.
    d = std::move(d).oom_score_adj(500);

    // claybin closes inherited fds unless told not to.
    if (!p.close_inherited_fds) d = std::move(d).unsafe_inherit_fds();

    return std::move(d).seal();
}

#if defined(__linux__)
// execvp's lookup, done before the spawn: claybin's guest calls execve. A
// name with a slash is a path already; anything not found stays as given,
// so the guest's execve reports the real error. Linux-only: the macOS
// backend spawns through posix_spawnp, which searches PATH itself.
std::string resolve_on_path(const std::string& name) {
    if (name.empty() || name.find('/') != std::string::npos) return name;
    const char* path = std::getenv("PATH");
    std::string_view rest = (path && *path) ? path : "/usr/local/bin:/usr/bin:/bin";
    while (true) {
        const auto colon = rest.find(':');
        std::string dir{rest.substr(0, colon)};
        if (dir.empty()) dir = ".";
        std::string full = dir + "/" + name;
        if (::access(full.c_str(), X_OK) == 0) return full;
        if (colon == std::string_view::npos) break;
        rest.remove_prefix(colon + 1);
    }
    return name;
}
#endif

}  // namespace

// The host's sandbox capabilities. Probed once (it forks), then reused: they
// don't change while agentty runs, and the Sandbox pane asks on every key.
#if defined(__APPLE__)
const ::clay::macos::HostCapabilities& host() {
    static const ::clay::macos::HostCapabilities h = ::clay::macos::probe_host();
    return h;
}
std::uint32_t landlock_abi() { return 0; }
#else
const ::clay::HostCapabilities& host() {
    static const ::clay::HostCapabilities h = ::clay::probe_host();
    return h;
}
std::uint32_t landlock_abi() { return host().landlock_abi; }
#endif

#if defined(__APPLE__)

// ─────────────────────────── macOS ────────────────────────────────────
//
// Shorter than the Linux path because seatbelt is one mechanism rather than
// four, and because there is no broker: macOS has no seccomp user-notify, so
// there is nothing to supervise and `SpawnResult::service_broker` stays empty.

bool available() {
    // probe_host() forks and actually calls sandbox_init() on a throwaway
    // profile rather than testing for the symbol. Same discipline as the Linux
    // probe and for the same reason: a capability that reads as present but
    // fails on use is worse than absence, because Auto then degrades silently
    // instead of loudly.
    //
    // In-process probing is not an option here. A seatbelt profile is
    // IRREVERSIBLE -- once entered, a process cannot leave it -- so a probe
    // that applied one would confine agentty itself.
    return host().seatbelt;
}

Report describe(const Posture& p) {
    Report r;
    auto sealed = build_policy(p);
    auto compiled = ::clay::macos::compile(sealed, host());
    if (!compiled) {
        r.ok = false;
        r.detail = std::string{compiled.error().mechanism};
        return r;
    }
    r.ok = true;
    for (int i = 0; i < static_cast<int>(CapId::count_); ++i) {
        auto id = static_cast<CapId>(i);
        auto s = compiled->guarantees.strength(id);
        Wall w;
        w.name = ::clay::cap_name(id);
        w.strength = s == Enforcement::strong    ? "strong"
                     : s == Enforcement::partial ? "partial"
                     : s == Enforcement::advisory ? "advisory"
                                                  : "none";
        // The receipt, same as Linux: "filesystem.read strong via seatbelt" is
        // checkable in a way that "sandbox: active" is not. The mechanism
        // strings differ per platform precisely because they are the truth.
        w.mechanism = compiled->guarantees.mechanism(id);
        r.walls.push_back(std::move(w));
    }
    return r;
}

SpawnResult spawn_shell(const Posture& p, const std::string& shell_cmd, int stdout_fd,
                        int stderr_fd) {
    SpawnResult out;

    auto sealed = build_policy(p);
    ::clay::macos::Options opts;
    opts.profile_name = "agentty";
    // Live probe at spawn: what this command gets must match the kernel now.
    auto compiled = ::clay::macos::compile(sealed, ::clay::macos::probe_host(), opts);
    if (!compiled) {
        out.start_error =
            "claybin: compile failed: " + std::string{compiled.error().mechanism};
        return out;
    }

    // /bin/sh -c, matching the bwrap and Linux-claybin paths: pipes, redirects
    // and globs are part of what an approved bash call means.
    const char* argv[] = {"/bin/sh", "-c", shell_cmd.c_str(), nullptr};
    // Same filtered environment as linux and windows (box_env_keeps).
    std::vector<std::string> env_store;
    for (char** e = ::environ; e && *e; ++e) {
        const std::string_view kv{*e};
        if (box_env_keeps(kv.substr(0, kv.find('=')))) env_store.emplace_back(kv);
    }
    std::vector<const char*> envp;
    envp.reserve(env_store.size() + 1);
    for (const auto& s : env_store) envp.push_back(s.c_str());
    envp.push_back(nullptr);
    ::clay::macos::SpawnRequest req;
    req.program = "/bin/sh";
    req.argv = argv;
    req.envp = envp.data();
    req.stdout_fd = stdout_fd;
    req.stderr_fd = stderr_fd;
    if (!p.cwd.empty())
        req.workdir = p.cwd;
    else if (!p.workspace.empty())
        req.workdir = p.workspace;

    auto spawned = ::clay::macos::spawn(*compiled, req);
    if (!spawned) {
        out.start_error = "claybin: spawn failed: " +
                          std::string{spawned.error().mechanism} + " (errno " +
                          std::to_string(spawned.error().sys_errno) + ")";
        return out;
    }

    out.started = true;
    out.pid = spawned->pid;
    // No pidfd on darwin and no broker to service: macOS has no seccomp
    // user-notify, so there is no supervisor fd. Left at their defaults rather
    // than faked, so the runner's `supervisor_fd >= 0` test does the right
    // thing without needing to know which platform it is on.
    return out;
}

std::string compile_profile(const Posture& p, std::string& error) {
    auto sealed = build_policy(p);
    ::clay::macos::Options opts;
    opts.profile_name = "agentty";
    auto compiled = ::clay::macos::compile(sealed, host(), opts);
    if (!compiled) {
        error = "claybin: compile failed: " + std::string{compiled.error().mechanism};
        return {};
    }
    if (!compiled->use_seatbelt || compiled->profile.empty()) {
        error = "claybin: compiled policy has no seatbelt profile";
        return {};
    }
    return std::move(compiled->profile);
}

#elif defined(_WIN32)

// ─────────────────────────── Windows ──────────────────────────────
//
// appcontainer + restricted token + job object + mitigation policies, through
// claybin's windows backend. The compiler is the same build_policy() the
// other two platforms run; only the apply step differs, and the guarantee
// report says which walls each host actually erected.
//
// What the caller gets back is HANDLES, not fds: process + job, both already
// open, both owned by the caller. jaal's windows_process::adopt consumes them;
// the job is the kill switch (KILL_ON_JOB_CLOSE), the process handle the
// waitable exit.

bool available() {
    // The probe is real, like every other backend's: appcontainer profiles are
    // created and deleted, restricted tokens are PROVEN by running a child
    // under one, mitigation attributes by initializing the list. A capability
    // that reads as present but fails on use is worse than absence, because
    // Auto then degrades silently instead of loudly.
    const auto host = ::clay::windows::probe_host();
    // The floor is one real wall: the appcontainer token or a job object.
    return host.app_container || host.job_objects;
}

Report describe(const Posture& p) {
    Report r;
    auto sealed = build_policy(p);
    auto compiled = ::clay::windows::compile(sealed, ::clay::windows::probe_host());
    if (!compiled) {
        r.ok = false;
        r.detail = std::string{compiled.error().mechanism};
        return r;
    }
    r.ok = true;
    for (int i = 0; i < static_cast<int>(CapId::count_); ++i) {
        auto id = static_cast<CapId>(i);
        auto s = compiled->guarantees.strength(id);
        Wall w;
        w.name = ::clay::cap_name(id);
        w.strength = s == Enforcement::strong    ? "strong"
                     : s == Enforcement::partial ? "partial"
                                                 : "none";
        // The receipt, per platform truth: "filesystem.read partial via
        // appcontainer+DACL" is checkable; "sandbox active" is not.
        w.mechanism = compiled->guarantees.mechanism(id);
        r.walls.push_back(std::move(w));
    }
    return r;
}

SpawnResult spawn_argv(const Posture& p, const std::vector<std::string>& argv_in,
                       int stdout_fd, int stderr_fd,
                       const std::vector<std::pair<std::string, std::string>>& env) {
    SpawnResult out;

    if (argv_in.empty()) {
        out.start_error = "claybin: empty argv";
        return out;
    }

    auto sealed = build_policy(p);
    ::clay::windows::Options opts;
    opts.profile_name = "agentty";
    // ACL stamping is a host-side mutation, so it stays opt-in at the API
    // level -- and agentty opts in HERE, once, deliberately: the alternative
    // is an appcontainer that can reach nothing the user asked the command to
    // reach, which makes the sandbox useless rather than safe. The compile
    // report still grades the walls honestly (partial, per-object semantics).
    opts.allow_host_acl_mutation = true;

    auto compiled = ::clay::windows::compile(sealed, ::clay::windows::probe_host(), opts);
    if (!compiled) {
        out.start_error = "claybin: compile failed: " +
                          std::string{compiled.error().mechanism};
        return out;
    }

    // The caller's argv reaches the child exactly as given -- argv[0] is the
    // program NAME, the posix convention all three backends share, and
    // claybin's windows spawner rebuilds the command line from it. No shell
    // re-parsing of user data on the way in.
    std::vector<const char*> argv;
    argv.reserve(argv_in.size() + 1);
    for (const auto& a : argv_in) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    // PATH resolution: CreateProcessW searches the loader path like execvp,
    // but only when no slash is present; keep the caller's program as given.
    const std::string program = argv_in.front();

    // The request's env, layered on a FILTERED copy of ours (box_env_keeps).
    // The windows loader wants a sorted double-NUL block; claybin builds that
    // (case-insensitive sort, "NAME=VALUE\0" pairs) from the char* list below.
    std::vector<std::string> env_store;
    std::vector<const char*> envp;
    {
        // ours first, so the caller's overrides land after and win the sort
        if (wchar_t* wbase = ::GetEnvironmentStringsW()) {
            for (const wchar_t* e = wbase; *e;) {
                const std::wstring wide{e};
                e += wide.size() + 1;
                // only "NAME=VALUE" shapes, skip the odd special entries
                const auto eq = wide.find(L'=');
                if (eq == std::wstring::npos || eq == 0) continue;
                const std::string name(wide.begin(), wide.begin() + eq);
                {
                    std::string upper = name;  // windows names are case-blind
                    for (char& c : upper)
                        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
                    if (!box_env_keeps(upper)) continue;
                }
                // overridden names are supplied by the caller below; keep the
                // name side ASCII-faithful (env var names are ASCII in
                // practice) and convert the value losslessly via UTF-8
                bool overridden = false;
                for (const auto& [k, v] : env)
                    if (_stricmp(k.c_str(), name.c_str()) == 0) { overridden = true; break; }
                if (overridden) continue;
                const int n = ::WideCharToMultiByte(
                    CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
                if (n > 0) {
                    std::string line(n - 1, '\0');
                    ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1,
                                          line.data(), n, nullptr, nullptr);
                    env_store.push_back(std::move(line));
                }
            }
            ::FreeEnvironmentStringsW(wbase);
        }
        for (const auto& [k, v] : env) env_store.push_back(k + "=" + v);
        for (const auto& line : env_store) envp.push_back(line.c_str());
        envp.push_back(nullptr);
    }

    ::clay::windows::SpawnRequest req;
    req.program = program.c_str();
    req.argv = argv.data();
    req.envp = envp.data();
    // The caller's pipe handles arrive through the FdMap (they are HANDLES on
    // windows, and the exec path maps them explicitly). The sentinel fields
    // below cover the spawn_shell path where no map was provided: inherit
    // stdout/stderr, NUL on stdin.
    req.stdin_fd = ::clay::windows::SpawnRequest::kDevNull;
    if (stdout_fd >= 0 || stderr_fd >= 0) {
        int n = 0;
        if (stdout_fd >= 0)
            req.fds[n++] = {reinterpret_cast<void*>(static_cast<intptr_t>(stdout_fd)), 1};
        if (stderr_fd >= 0)
            req.fds[n++] = {reinterpret_cast<void*>(static_cast<intptr_t>(stderr_fd)), 2};
        req.fd_count = n;
        req.stdout_fd = -1;  // mapped explicitly above
        req.stderr_fd = -1;
    } else {
        req.stdout_fd = -1;
        req.stderr_fd = -1;
    }
    if (!p.cwd.empty())
        req.workdir = p.cwd;
    else if (!p.workspace.empty())
        req.workdir = p.workspace;

    auto spawned = ::clay::windows::spawn(*compiled, req);
    if (!spawned) {
        out.start_error = "claybin: spawn failed: " +
                          std::string{spawned.error().mechanism} + " (errno " +
                          std::to_string(spawned.error().sys_errno) + ")";
        return out;
    }

    out.started = true;
    out.pid = spawned->pid;
    // HANDLES, not descriptors. SpawnResult carries them as ints (its shape is
    // shared with the posix paths); jaal's adopt reinterprets them. The job is
    // the kill switch, the process handle the waitable exit -- both owned by
    // the caller from here.
    out.pidfd = static_cast<int>(reinterpret_cast<intptr_t>(spawned->process));
    // The closure owns the job handle. Dropping it closes the job, and
    // KILL_ON_JOB_CLOSE takes down anything the command left behind.
    std::shared_ptr<void> job{spawned->job, [](void* h) {
        if (h) ::CloseHandle(static_cast<HANDLE>(h));
    }};
    out.kill_tree = [job = std::move(job)]() -> bool {
        if (!job) return false;
        return ::TerminateJobObject(static_cast<HANDLE>(job.get()), 1) != FALSE;
    };
    // No broker on windows: seccomp user-notify has no equivalent, so
    // supervisor_fd and service_broker keep their sentinel defaults.
    return out;
}

SpawnResult spawn_shell(const Posture& p, const std::string& shell_cmd, int stdout_fd,
                        int stderr_fd) {
    return spawn_argv(p, {"cmd.exe", "/d", "/s", "/c", shell_cmd}, stdout_fd, stderr_fd);
}

#else  // __linux__

bool available() {
    // The same question the bwrap probe asks, and for the same reason: a
    // capability that reads as present but fails on use is worse than absence,
    // because Auto then degrades silently instead of loudly.
    //
    // claybin's probe_host() forks and attempts the uid_map write rather than
    // reading sysctls -- so this is a faithful predictor of spawn(), not a
    // guess about the kernel's feature list.
    const auto& host = claybin_backend::host();

    // NOT gated on user namespaces, and that is the whole reason claybin is
    // worth having as a second backend.
    //
    // This used to require `user_namespaces && mount_namespaces`, which made
    // claybin useless on exactly the host that needs it most. Ubuntu 24.04
    // ships an AppArmor profile that denies the uid_map write to unconfined
    // binaries, so bwrap fails with "setting up uid map: permission denied"
    // and agentty reported no backend at all (issue from KhazAkar). Copying
    // bwrap's precondition meant copying bwrap's failure.
    //
    // But landlock and seccomp are UNPRIVILEGED mechanisms. Neither needs a
    // namespace, and AppArmor's userns restriction does not touch them. So on
    // that host claybin still delivers, measured through compile():
    //
    //   filesystem.read/write/exec  strong   landlock
    //   syscall.filter              strong   seccomp-bpf
    //   resource.memory             strong   cgroup2 memory.max
    //   privilege.drop              strong   no_new_privs + empty bounding set
    //   network.isolation           none     (needs a netns)
    //   process.isolation           none     (needs userns + pidns)
    //
    // That is a real sandbox where bwrap has none: the filesystem boundary --
    // the one that keeps an approved command out of ~/.ssh -- survives intact.
    // Losing network and pid isolation is a genuine downgrade and the pane's
    // wall report says so per capability, which is the honest way to ship a
    // partial boundary.
    //
    // The floor is landlock OR seccomp. With neither there is nothing left to
    // enforce and claiming a sandbox would be the lie this whole subsystem is
    // built to avoid, so we decline and let the caller fall back to bwrap (or
    // to a loud failure under --sandbox on).
    return host.landlock_abi > 0 || host.seccomp;
}

Report describe(const Posture& p) {
    Report r;
    auto sealed = build_policy(p);
    auto compiled = compile(sealed, host());
    if (!compiled) {
        r.ok = false;
        r.detail = std::string{compiled.error().mechanism};
        return r;
    }
    r.ok = true;
    for (int i = 0; i < static_cast<int>(CapId::count_); ++i) {
        auto id = static_cast<CapId>(i);
        auto s = compiled->guarantees.strength(id);
        Wall w;
        w.name = ::clay::cap_name(id);
        w.strength = s == Enforcement::strong    ? "strong"
                     : s == Enforcement::partial ? "partial"
                                                 : "none";
        // The receipt. claybin already knows this; not copying it out was the
        // difference between "the sandbox is active" and "filesystem.read is
        // strong via landlock abi 10", and only the second is checkable.
        w.mechanism = compiled->guarantees.mechanism(id);
        r.walls.push_back(std::move(w));
    }
    return r;
}

SpawnResult spawn_argv(const Posture& p, const std::vector<std::string>& argv_in,
                       int stdout_fd, int stderr_fd,
                       const std::vector<std::pair<std::string, std::string>>& env) {
    SpawnResult out;

    if (argv_in.empty()) {
        out.start_error = "claybin: empty argv";
        return out;
    }

    auto sealed = build_policy(p);
    // Live probe at spawn: what this command gets must match the kernel now.
    auto compiled = compile(sealed, probe_host());
    if (!compiled) {
        out.start_error = "claybin: compile failed: " + std::string{compiled.error().mechanism};
        return out;
    }

    // "No network" is a promise the user chose (the pane's None, the locked
    // posture, `run --no-network`). claybin keeps it with an empty netns, and
    // a host that denies unprivileged user namespaces gets none: compile()
    // still succeeds, it just DEGRADES network.isolation, and the guest would
    // run with the host's network. Refuse instead -- fail closed, and say why.
    if (p.net_mode == 1 /* None */
        && compiled->guarantees.strength(CapId::net_isolation) != Enforcement::strong) {
        out.start_error =
            "sandbox: network is set to None but this host can't isolate it "
            "(no network namespace -- unprivileged user namespaces are likely "
            "disabled); refusing to run with the network open";
        return out;
    }

    // The caller's argv reaches the child exactly as given. No `sh -c`
    // here: a commit message with quotes or a `$` in it must not be
    // re-parsed on the way in, which is the whole reason the argv form
    // exists beside the shell one.
    std::vector<const char*> argv;
    argv.reserve(argv_in.size() + 1);
    for (const auto& a : argv_in) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    // claybin calls execve, which does no PATH lookup, so a bare "git" came
    // back as exit 127. Resolve it here, in the parent, the way execvp would.
    const std::string program = resolve_on_path(argv_in.front());
    // The request's env, layered on a FILTERED copy of ours, as a ready envp:
    // the child can't build one (it may not allocate). Always built here --
    // a null envp would hand the guest agentty's whole environ.
    std::vector<std::string> env_store;
    std::vector<const char*> envp;
    {
        for (char** e = ::environ; e && *e; ++e) {
            const std::string_view kv{*e};
            const auto key = kv.substr(0, kv.find('='));
            if (!box_env_keeps(key)) continue;
            bool overridden = false;
            for (const auto& [k, v] : env) if (k == key) { overridden = true; break; }
            if (!overridden) env_store.emplace_back(kv);
        }
        for (const auto& [k, v] : env) env_store.push_back(k + "=" + v);
        // The guest's route off its own loopback. Every spelling is set
        // because the toolchain is not consistent: curl reads
        // http_proxy/https_proxy and ALL_PROXY, git reads http_proxy,
        // npm/pip/cargo want the uppercase forms, go reads HTTP(S)_PROXY.
        // One address under all of them, so no tool bypasses the proxy just
        // by not being asked.
        //
        // No NO_PROXY for localhost: inside the box localhost IS the proxy,
        // and an exemption there would break the connection rather than open
        // a hole, because nothing else is listening in that namespace.
        if (p.net_mode != 1 /* not None */) {
            const std::string addr = "http://127.0.0.1:" + std::to_string(kProxyPort);
            for (const char* k : {"http_proxy", "https_proxy", "HTTP_PROXY",
                                  "HTTPS_PROXY", "ALL_PROXY", "all_proxy"}) {
                bool have = false;
                for (const auto& [ek, ev] : env) if (ek == k) { have = true; break; }
                if (!have) env_store.push_back(std::string{k} + "=" + addr);
            }
        }
        envp.reserve(env_store.size() + 1);
        for (const auto& s : env_store) envp.push_back(s.c_str());
        envp.push_back(nullptr);
    }
    Command cmd{program.c_str(), argv.data(), envp.data()};
    // stdin from /dev/null explicitly rather than inherited -- a guest that can
    // read the user's terminal can prompt them, and nothing a tool runs should.
    cmd.stdin_fd = Command::kDevNull;
    cmd.stdout_fd = stdout_fd;
    cmd.stderr_fd = stderr_fd;

    // Hold the compiled posture for as long as the guest runs. It OWNS the
    // cgroup: letting it die closes the handle and rmdirs the group, which
    // would both drop the limits and lose the only reliable way to kill the
    // tree. shared_ptr because the kill hook below outlives this function.
    using Compiled = std::remove_reference_t<decltype(*compiled)>;
    auto held = std::make_shared<Compiled>(std::move(*compiled));

    // spawn_in, NOT spawn. spawn() forwards an EMPTY group, and spawn_in only
    // ever looks at the group it is handed -- never at the compiled posture --
    // so the plain call left the guest in no cgroup at all. describe() was
    // meanwhile reporting `resource.memory strong via cgroup2` off the same
    // compile, so the pane advertised a wall that was never erected. One call,
    // two things fixed: the limits now bind, and the group exists to kill.
    auto spawned = spawn_in(held->plan, cmd, held->cgroup);
    if (!spawned) {
        out.start_error = "claybin: spawn failed: " + std::string{spawned.error().mechanism} +
                          " (errno " + std::to_string(spawned.error().sys_errno) + ")";
        return out;
    }

    out.started = true;
    out.pid = spawned->pid;
    out.pidfd = spawned->pidfd;

    // The guest's outbound network, filtered.
    //
    // claybin put the box in its own network namespace and opened one
    // listener on its loopback; from here we accept on that descriptor and
    // decide what each connection may dial. Without this the box has no
    // route anywhere, so a failure to start is fail-CLOSED (the guest's
    // connections are refused) rather than fail-open.
    //
    // Lifetime rides in the closure the same way the broker's listener does:
    // SpawnResult is copyable and a Proxy is not, so the shared_ptr is what
    // makes "the proxy dies when the command's result does" true without the
    // runner having to know it exists.
    if (spawned->proxy_listen_fd >= 0) {
        auto proxy = std::make_shared<netproxy::Proxy>();
        if (proxy->start(spawned->proxy_listen_fd, p.allow_hosts)) {
            AGT_LOG(Tool, Debug, "sandbox.proxy.up", "port={} pid={} netns={}",
                    spawned->proxy_port, spawned->pid, spawned->proxy_netns_ino);
            out.net_proxy = std::move(proxy);
        } else {
            AGT_LOG(Tool, Warn, "sandbox.proxy.start_failed", "pid={}", spawned->pid);
        }
    }

    // Killing the tree, when the kernel gives us the means (cgroup.kill,
    // 5.14+). Left empty otherwise so the runner keeps its process-group
    // fallback rather than believing a kill that did nothing.
    if (held->cgroup.valid() && held->cgroup.kill_supported()) {
        out.kill_tree = [held]() -> bool { return held->cgroup.kill().has_value(); };
    }

    // The seccomp listener, when the policy brokers syscalls.
    //
    // claybin hands the listener to the CALLER, and the caller must answer
    // every notification: the kernel blocks the guest thread until someone
    // responds, so an unpolled listener is a hang rather than a weaker wall.
    // The subprocess runner polls `supervisor_fd` alongside the output pipe and
    // calls `service_broker` when it is readable.
    if (spawned->notify_fd >= 0) {
        out.supervisor_fd = spawned->notify_fd;

        // The listener lives in the closure, by shared_ptr, because
        // SpawnResult is copyable and a Listener is not. It closes when the
        // last copy of the callback dies, which is the right lifetime: the
        // runner drops the callback when the command ends.
        auto listener = std::make_shared<::clay::broker::Listener>(
            ::clay::OwnedFd{spawned->notify_fd});
        // The guest's own pid doubles as its process group: claybin calls
        // setsid() (new_session), so the child leads its own group and every
        // descendant inherits it. That is what makes "inside my own group" a
        // decidable question from a scalar.
        const std::uint32_t guest_pid = static_cast<std::uint32_t>(spawned->pid);

        out.service_broker = [listener, guest_pid]() -> bool {
            auto req = listener->next();
            if (!req) return false;   // guest gone, or the fd closed

            broker::Event ev;
            const auto verdict =
                broker::decide(req->nr, req->args, req->pid, guest_pid, &ev);

            // THE CHECK EVERY IMPLEMENTATION GETS WRONG, done immediately
            // before responding.
            //
            // A notification id can be reused once its thread is gone. If the
            // guest thread died between `next()` and here, responding would
            // answer a DIFFERENT syscall than the one we judged -- the kernel
            // offers no other way to close that race, which is why
            // seccomp_unotify(2) documents the ID_VALID ioctl as mandatory
            // rather than advisory.
            //
            // Returning true (not false) on a dead notification: the listener
            // itself is still fine, only this request is stale. Tearing down
            // the supervisor here would strand every later brokered call.
            if (!listener->still_valid(*req)) {
                AGT_LOG(Tool, Debug, "sandbox.broker",
                        "notification went stale before the response; dropped");
                return true;
            }

            const auto decision = verdict == broker::Verdict::Allow
                                      ? ::clay::broker::Decision::allow_it()
                                      : ::clay::broker::Decision::deny_it(EPERM);

            // Logged at every outcome, because this is the audit trail that
            // makes the sandbox teachable. "your build failed" says nothing;
            // "cargo tried ptrace ATTACH pid=1 and was denied" says what the
            // toolchain does and what to allow if you disagree.
            AGT_LOG(Tool, Info, "sandbox.broker", "{} {} {}",
                    ev.syscall, ev.detail, ev.allowed ? "allowed" : "DENIED");

            // And into the feed the settings pane reads. record() ignores
            // allows, so this is the denial path only -- the log is the trace,
            // the feed is the security surface.
            //
            // Called from a tool WORKER thread; the feed takes a lock for
            // exactly this reason.
            broker::record(ev);

            if (!listener->respond(*req, decision)) {
                // Responding failed, which usually means the guest died while
                // we were deciding. Not an error worth tearing down for; the
                // next next() will report the listener finished.
                AGT_LOG(Tool, Debug, "sandbox.broker", "respond failed");
            }
            return true;
        };
    }
    return out;
}

// The shell form is the argv form with a shell in front of it. Pipes,
// redirects and globs are part of what a shell tool is for, so this is a
// genuine convenience rather than a second implementation -- there is one
// spawn path and it is above.
SpawnResult spawn_shell(const Posture& p, const std::string& shell_cmd, int stdout_fd,
                        int stderr_fd) {
    return spawn_argv(p, {"/bin/sh", "-c", shell_cmd}, stdout_fd, stderr_fd);
}

#endif  // __APPLE__ / __linux__

}  // namespace agentty::tools::util::sandbox::claybin_backend

#endif  // __linux__ || __APPLE__
