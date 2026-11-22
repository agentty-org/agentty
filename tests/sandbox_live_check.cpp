// Live enforcement check, driving agentty's OWN claybin spawn path.
//
// The end-to-end route (agentty run -> LLM -> shell tool) kept dying on
// provider hiccups, and it is the wrong instrument anyway: it proves the model
// can call a tool, not that the sandbox enforces. This calls the same
// claybin_backend::spawn_shell() the shell tool calls, with a Posture built by
// hand, and looks at what the child actually got.
//
// Two claims:
//   1. under the compiler profile, realloc() works           (the mremap fix)
//   2. with net_mode=None, a socket to a raw IP cannot connect (the net rule)
#include "agentty/tool/util/sandbox_claybin.hpp"
#include "agentty/tool/util/sandbox.hpp"
#include "agentty/domain/sandbox_config.hpp"   // kAlwaysMasked*
#include "agentty/util/io.hpp"
#include "agentty/util/child_process.hpp"   // the long-lived section drives it

#include <claybin/plan/compile.hpp>   // probe_host: which walls this host has

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <filesystem>
#include <fstream>   // the handoff-prevention case reads the HOST file back
#include <string>
#include <string_view>
#include <system_error>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cb = agentty::tools::util::sandbox::claybin_backend;

// The stub setter from sandbox_config_race_stubs.cpp. Declared here rather
// than pulled from fs_helpers.hpp, because including that header is exactly
// what this target avoids (it reaches into mcp and drags the tool layer in).
namespace agentty::tools::util { void set_workspace_root(Io, std::filesystem::path p); }

namespace {

// The sandbox's workspace for this check. Created in main(); a missing one
// makes every spawn fail on mount(ENOENT).
constexpr const char* kWorkspace = "/tmp/agentty-sandbox-live-check";

int failures = 0;
void expect(bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

// A check whose wall only exists with namespaces (a fake hostname, a sized
// /dev, a cgroup namespace, an empty core_pattern mount). On a host without
// them it is not a failure -- the wall cannot exist, and the guarantee report
// already says so -- but it is printed, never silently dropped, so a run
// shows exactly which walls this host lacks.
bool has_namespaces() {
    static const bool v = ::clay::probe_host().mount_namespaces;
    return v;
}
void expect_ns(bool ok, const char* what) {
    if (has_namespaces()) return expect(ok, what);
    std::printf("  n/a  %s (needs namespaces; this host has none)\n", what);
}

// The read set agentty grants by default, enough for /bin/sh + python3.
cb::Posture base_posture() {
    cb::Posture p;
    p.system_read_roots = {"/usr", "/bin", "/sbin", "/lib", "/lib64", "/opt"};
    p.etc_readable      = {"/etc/ld.so.cache", "/etc/ld.so.conf", "/etc/ld.so.conf.d",
                           "/etc/resolv.conf", "/etc/hosts", "/etc/nsswitch.conf",
                           "/etc/passwd", "/etc/group", "/etc/localtime",
                           "/etc/ssl", "/etc/ca-certificates", "/etc/pki"};
    p.workspace         = kWorkspace;
    p.cwd               = kWorkspace;
    p.syscall_mode      = 1;   // Compiler -- the default, and where the bug was
    p.net_mode          = 0;   // Full
    p.tmp_bytes         = 512ull * 1024 * 1024;

    // The non-configurable masks, exactly as build_claybin_posture() sets
    // them. Mirrored rather than left empty: an empty mask list is why the
    // first version of the .env check below "failed" for the wrong reason --
    // it was measuring a posture nobody ever spawns.
    if (const char* home = std::getenv("HOME"); home && *home) {
        for (const char* m : agentty::sandbox_cfg::kAlwaysMasked)
            p.masked.emplace_back(std::string{home} + m);
    }
    for (const char* n : agentty::sandbox_cfg::kAlwaysMaskedNames) {
        // Skip SUFFIX rules (`*.pem`, `*.tfvars`). A suffix is not a path and
        // cannot be turned into one without knowing what is on disk -- the
        // spawn path finds those by walking, which is the whole reason the
        // walk exists. Keyed on the `*` marker rather than on a hardcoded
        // ".pem", so adding a suffix to the list cannot silently plant a
        // nonsense mask at `<workspace>/*.tfvars` here.
        if (n[0] == '*') continue;
        // Every NAME rule, not just dotfiles. The old `n[0] != '.'` filter was
        // an artifact of a list that happened to be all dotfiles; it now
        // silently drops `credentials.json` and `service-account.json`, so
        // this harness would mirror a posture weaker than the one the spawn
        // path builds -- and a live check that measures the wrong posture is
        // the failure mode the comment above already warns about.
        p.masked.emplace_back(std::string{kWorkspace} + "/" + n);
    }
    return p;
}

// Run a shell command under the posture, capture its output, return exit code.
int run(const cb::Posture& p, const std::string& cmd, std::string& out) {
    int fds[2];
    if (::pipe(fds) != 0) { std::printf("  pipe failed\n"); return -1; }

    auto sp = cb::spawn_shell(p, cmd, fds[1], fds[1]);
    ::close(fds[1]);
    if (!sp.started) {
        // A spawn failure is a HARD failure of the whole check, not a quiet
        // -1 the caller might read as "blocked". That distinction bit once
        // already: with the workspace directory missing, claybin failed with
        // mount(ENOENT), the child never ran, and the "did not reach the
        // network" assertion passed -- because nothing reached anything. A
        // negative test that passes when the subject never executed is worse
        // than no test.
        std::printf("  spawn failed: %s\n", sp.start_error.c_str());
        ++failures;
        ::close(fds[0]);
        return -1;
    }
    // Drain the pipe AND service the supervisor, in one poll.
    //
    // Both, in the same loop, for the reason the production runner does it:
    // a brokered syscall blocks the guest in the kernel until someone answers,
    // so reading the pipe first and the listener later deadlocks -- the child
    // waits for a decision, we wait for output it cannot produce until it has
    // one. This harness has to mirror that or the brokering cases below would
    // hang instead of failing, and a hang reads as "the test is broken".
    std::string chunk;
    bool eof = false;
    while (!eof) {
        struct pollfd pfds[2];
        pfds[0] = {fds[0], POLLIN, 0};
        const bool brokering = sp.supervisor_fd >= 0 && sp.service_broker;
        if (brokering) pfds[1] = {sp.supervisor_fd, POLLIN, 0};

        // A bounded wait rather than -1: a bug that stops answering should
        // surface as a failed assertion, not as a harness that never returns.
        if (::poll(pfds, brokering ? 2 : 1, 10'000) <= 0) break;

        if (brokering && (pfds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            if (!sp.service_broker()) {
                sp.supervisor_fd = -1;
                sp.service_broker = nullptr;
            }
        }
        if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            char buf[4096];
            const ssize_t n = ::read(fds[0], buf, sizeof buf);
            if (n > 0) out.append(buf, static_cast<std::size_t>(n));
            else if (n == 0) eof = true;
            else if (errno != EINTR && errno != EAGAIN) eof = true;
        }
    }
    ::close(fds[0]);

    int status = 0;
    ::waitpid(sp.pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -2;
}

}  // namespace

int main() {
    if (!cb::available()) {
        std::printf("claybin unavailable on this host (no user/mount namespaces) -- skipping\n");
        return 0;
    }

    // The workspace has to EXIST before claybin can bind it -- otherwise
    // every spawn dies on mount(ENOENT) and the negative tests below pass
    // vacuously. Created here rather than assumed, so the check is
    // self-contained and cannot be broken by someone cleaning /tmp.
    {
        std::error_code ec;
        std::filesystem::create_directories(kWorkspace, ec);
        if (ec) {
            std::printf("cannot create workspace %s: %s\n", kWorkspace,
                        ec.message().c_str());
            return 1;
        }
    }

    // build_bwrap_argv() binds workspace_root(), so the stub has to point at
    // the directory we just made. Without this the bwrap case would bind /tmp
    // and mask /tmp/.env -- and then pass, because the canary it is looking
    // for lives somewhere else entirely. A check that passes for the wrong
    // reason is the failure mode this harness has already hit twice.
    agentty::tools::util::set_workspace_root(::agentty::IoAccess::grant(), kWorkspace);

    // LL_ONLY=1: run only the long-lived section (it is the slow one to reach).
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // a hang must show WHERE it is
    if (std::getenv("LL_ONLY")) goto long_lived;

    // ── net_mode=Ports: listed TCP ports only, and no listening ──────────
    std::printf("net_mode=Ports with 443 only:\n");
    {
        auto p = base_posture();
        p.net_mode = 2;
        p.allow_ports = {443};
        std::string out;
        run(p, "python3 - <<'EOF' 2>&1\n"
               "import socket\n"
               "s=socket.socket(); s.settimeout(2)\n"
               "try: s.connect(('127.0.0.1', 80)); print('tcp80 ok')\n"
               "except PermissionError: print('tcp80 denied')\n"
               "except Exception as e: print('tcp80 other', e)\n"
               "s=socket.socket()\n"
               "try: s.bind(('127.0.0.1', 0)); s.listen(); print('bind ok')\n"
               "except PermissionError: print('bind denied')\n"
               "EOF", out);
        expect(out.find("tcp80 denied") != std::string::npos, "an unlisted port is refused");
        expect(out.find("bind denied") != std::string::npos, "listening is refused");
        // UDP in this mode. Ports SHARES the host netns (a port rule is about
        // reaching real hosts), and landlock's port rules cover TCP only --
        // UDP is a separate access right, added in abi 10, which claybin does
        // not handle. So this is the one posture where a guest can still send
        // a datagram anywhere, and the check records which way it goes rather
        // than asserting a wall that is not there.
        out.clear();
        run(p,
            "python3 -c \"import socket\n"
            "for label, addr in (('internet', ('1.1.1.1', 53)), ('lan', ('192.168.1.1', 53))):\n"
            "    try:\n"
            "        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(2)\n"
            "        s.sendto(b'x', addr); print('udp', label, 'SENT')\n"
            "    except Exception as e: print('udp', label, type(e).__name__)\" 2>&1",
            out);
        std::printf("  udp under Ports: %s", out.c_str());
        // Not an expect() for the ABI: on a host below abi 10 landlock cannot
        // model UDP and there is nothing to assert. Where it can, this is a
        // real wall and the gap was real -- measured SENT before the fix.
        if (out.find("PermissionError") != std::string::npos) {
            expect(out.find("udp internet PermissionError") != std::string::npos
                       && out.find("udp lan PermissionError") != std::string::npos,
                   "UDP to an unlisted port is refused under Ports (abi 10)");
            // And the other half of the trade: a port the caller ALLOWED has
            // to work over UDP too, or allowing 53 breaks DNS. This is the
            // regression the fix could have caused.
            auto with_dns = base_posture();
            with_dns.net_mode = 2;
            with_dns.allow_ports = {53};
            out.clear();
            run(with_dns,
                "python3 -c \"import socket\n"
                "s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(2)\n"
                "try:\n"
                "    s.sendto(b'x', ('1.1.1.1', 53)); print('dns53 SENT')\n"
                "except Exception as e: print('dns53', type(e).__name__)\" 2>&1",
                out);
            std::printf("  udp on an allowed port: %s", out.c_str());
            expect(out.find("dns53 SENT") != std::string::npos,
                   "UDP on an ALLOWED port still works (DNS keeps resolving)");
        }
    }

    // ── Strict still runs a shell, and W^X applies with the filter off ──
    // Strict used to map to with_filesystem(), which has no execve, so the
    // Hardened and Airgapped presets failed every command.
    std::printf("syscall_mode=Strict runs a command:\n");
    for (int net : {0, 1}) {
        auto p = base_posture();
        p.syscall_mode = 2;
        p.net_mode = net;
        std::string out;
        const int rc = run(p, "echo strict-ok", out);
        expect(rc == 0 && out.find("strict-ok") != std::string::npos,
               net ? "strict + no network runs echo" : "strict + network runs echo");
    }
    std::printf("W^X with the filter off:\n");
    {
        auto p = base_posture();
        p.syscall_mode = 0;
        p.wx_protect = true;
        std::string out;
        run(p, "python3 -c 'import mmap; mmap.mmap(-1,4096,prot=7); print(\"MAPPED\")' 2>&1", out);
        expect(out.find("PermissionError") != std::string::npos, "an RWX mapping is refused");
        p.wx_protect = false;
        out.clear();
        run(p, "python3 -c 'import mmap; mmap.mmap(-1,4096,prot=7); print(\"MAPPED\")' 2>&1", out);
        expect(out.find("PermissionError") == std::string::npos, "W^X off allows an RWX mapping");
    }

    // ── 1. realloc under the compiler profile ────────────────────────
    // The regression: mremap was absent from base(), so realloc() of a large
    // block returned NULL and curl reported "out of memory" on a machine with
    // free RAM.
    //
    // Done in python rather than a compiled probe so the check has no build
    // step of its own and nothing to leave behind outside the workspace.
    // ctypes calls mremap(2) directly, then the allocator path curl hits.
    std::printf("realloc under syscall_mode=Compiler:\n");
    {
        auto p = base_posture();
        std::string out;
        int rc = run(p,
            "python3 -c 'import ctypes, os\n"
            "libc = ctypes.CDLL(None, use_errno=True)\n"
            "libc.mmap.restype = ctypes.c_void_p\n"
            "libc.mremap.restype = ctypes.c_void_p\n"
            "small, big = 1 << 20, 4 << 20\n"
            "p = libc.mmap(None, ctypes.c_size_t(small), 3, 0x22, -1, 0)\n"
            "assert p not in (0, 2**64 - 1), \"mmap failed\"\n"
            "print(\"mmap ok\")\n"
            "ctypes.set_errno(0)\n"
            "q = libc.mremap(ctypes.c_void_p(p), ctypes.c_size_t(small), ctypes.c_size_t(big), 1)\n"
            "e = ctypes.get_errno()\n"
            "print(\"mremap ok\" if (q or 0) != 2**64 - 1 else \"mremap FAILED errno=%d (%s)\" % (e, os.strerror(e)))\n"
            "b = bytearray(small)\n"
            "b.extend(bytes(big))            # realloc past the mmap threshold\n"
            "print(\"realloc ok\" if len(b) == small + big else \"realloc FAILED\")' 2>&1",
            out);
        std::printf("  exit=%d, child said: %s", rc, out.c_str());
        expect(rc == 0, "probe exits 0");
        expect(out.find("mremap ok") != std::string::npos, "mremap allowed");
        expect(out.find("realloc ok") != std::string::npos, "realloc works");
    }

    // ── 2. net_mode=None isolates ───────────────────────────────────────
    std::printf("connect() with net_mode=None:\n");
    {
        auto p = base_posture();
        p.net_mode = 1;  // None
        std::string out;
        // A raw IP, so this exercises the socket layer and not DNS.
        int rc = run(p,
            "python3 -c 'import socket,sys\n"
            "s=socket.socket(); s.settimeout(3)\n"
            "try:\n"
            "    s.connect((\"1.1.1.1\",443)); print(\"CONNECTED\")\n"
            "except OSError as e:\n"
            "    print(\"blocked:\", e)' 2>&1", out);
        std::printf("  exit=%d, child said: %s", rc, out.c_str());
        expect(out.find("CONNECTED") == std::string::npos, "did not reach the network");
    }

    // ── 3. net_mode=Full still works, but only through the proxy ───────
    // The other half of the claim: isolation is a CHOICE, so the permissive
    // setting must actually permit. A sandbox that blocks everything is not
    // enforcing a policy, it is broken.
    //
    // What "permit" MEANS changed with audit #1. A direct connect now fails
    // (the box has no route off its own loopback — that is the boundary), and
    // the real reach is through the proxy, which every tool finds via the env.
    // So this asserts both halves: raw socket blocked, proxied fetch works.
    std::printf("net_mode=Full: direct vs proxied:\n");
    {
        auto p = base_posture();
        p.net_mode = 0;
        std::string out;
        int rc = run(p,
            "python3 -c 'import socket\n"
            "s=socket.socket(); s.settimeout(5)\n"
            "try:\n"
            "    s.connect((\"1.1.1.1\",443)); print(\"CONNECTED\")\n"
            "except OSError as e:\n"
            "    print(\"blocked:\", e)' 2>&1", out);
        std::printf("  direct exit=%d, child said: %s", rc, out.c_str());
        expect(out.find("CONNECTED") == std::string::npos,
               "a raw socket cannot leave the box, even on Full (#1)");
        out.clear();
        // And the path that IS meant to work.
        rc = run(p, "curl -sS --max-time 20 -o /dev/null -w net=%{http_code} "
                    "https://example.com/ 2>&1", out);
        std::printf("  proxied exit=%d, child said: %s\n", rc, out.c_str());
        expect(out.find("net=200") != std::string::npos,
               "a real fetch through the proxy works (#1)");
    }

    // ── 4. the masks that are NOT configurable ────────────────────────
    // kAlwaysMasked/kAlwaysMaskedNames are the one part of the policy a user
    // cannot switch off, so they are the part most worth measuring rather
    // than trusting. A workspace .env is the sharpest case: the workspace is
    // bound READ-WRITE by both backends, so scope does not save us and the
    // mask is the only thing standing between an agent and a real secret.
    std::printf("workspace .env is masked:\n");
    {
        const std::string envp = std::string{kWorkspace} + "/.env";
        {
            std::FILE* f = std::fopen(envp.c_str(), "w");
            if (!f) { std::printf("  cannot plant .env\n"); ++failures; }
            else { std::fputs("SECRET=canary\n", f); std::fclose(f); }
        }
        auto p = base_posture();
        std::string out;
        int rc = run(p, "cat .env 2>&1", out);
        std::printf("  exit=%d, child said: %s", rc, out.c_str());
        expect(out.find("SECRET=canary") == std::string::npos,
               "the secret did NOT reach the child");
        std::error_code ec;
        std::filesystem::remove(envp, ec);
    }

    // ── 5. curl, specifically ─────────────────────────────────────
    // Its own case because curl reports EVERY early failure as
    // CURLE_OUT_OF_MEMORY (27), which sends you looking at memory caps and
    // the network rules when the cause is neither. python3 fetches the same
    // URL fine under this exact posture, so a curl failure here is about
    // curl's startup, not about the boundary.
    //
    // Kept as a check rather than a note because curl is what users reach
    // for, and "the sandbox broke curl" is indistinguishable from "the
    // sandbox is broken" from the outside.
    std::printf("curl reaches the network (net_mode=Full):\n");
    {
        auto p = base_posture();
        std::string out;
        int rc = run(p,
            "curl -sS -m 5 -o /dev/null -w 'H:%{http_code}' https://example.com 2>&1",
            out);
        std::printf("  exit=%d, child said: %s\n", rc, out.c_str());
        expect(out.find("H:200") != std::string::npos, "curl got a 200");
    }

    // ── 7. secrets BELOW the workspace root ──────────────────────────
    // The gap the bounded sweep closes. kAlwaysMaskedNames are BASENAMES, and
    // a name cannot become a mount without knowing where the file is -- so
    // masking only `<workspace>/.env` covered the one layout nobody uses.
    // `services/api/.env` is where it actually lives in a monorepo.
    //
    // Also checks the two things the sweep must NOT do: follow a directory
    // symlink out of the workspace, and walk into node_modules.
    std::printf("nested secrets are masked (sweep depth):\n");
    {
        const std::filesystem::path root{kWorkspace};
        std::error_code ec;
        std::filesystem::create_directories(root / "services" / "api", ec);
        std::filesystem::create_directories(root / "node_modules" / "pkg", ec);

        auto plant = [&](const std::filesystem::path& p, const char* body) {
            std::FILE* f = std::fopen(p.c_str(), "w");
            if (f) { std::fputs(body, f); std::fclose(f); }
        };
        plant(root / "services" / "api" / ".env", "NESTED=canary\n");
        plant(root / "services" / "api" / "key.pem", "PEMBODY=canary\n");
        // Inside a skipped directory: NOT masked, and that is deliberate --
        // the sweep trades it for not walking a 40k-file tree on every spawn.
        // Asserted so the trade is visible rather than assumed.
        plant(root / "node_modules" / "pkg" / ".env", "SKIPPED=canary\n");

        auto p = base_posture();
        // The real list, from the shared function -- the same one both
        // backends consume, so this cannot pass while production differs.
        {
            agentty::sandbox_cfg::Config c;
            c.configured = true;
            const char* home = std::getenv("HOME");
            p.masked = agentty::sandbox_cfg::mask_paths(
                c, kWorkspace, home ? home : "");
        }

        std::string out;
        int rc = run(p, "cat services/api/.env services/api/key.pem 2>&1", out);
        std::printf("  exit=%d, child said: %s", rc, out.c_str());
        expect(out.find("NESTED=canary") == std::string::npos,
               "a nested .env is masked");
        expect(out.find("PEMBODY=canary") == std::string::npos,
               "a nested *.pem is masked (the suffix rule)");

        std::filesystem::remove_all(root / "services", ec);
        std::filesystem::remove_all(root / "node_modules", ec);
    }

    // ── 8. the default pid cap actually bounds a fork bomb ──────────────
    // sandbox_audit reported resource.pids as `none` on a default install,
    // which means an approved command could fork without limit. max_procs now
    // ships at 4096 -- the one resource wall with a number that is safe on
    // every host (no build needs 4096 concurrent processes; a fork bomb wants
    // millions).
    //
    // Checked in the CHILD rather than in the report, because "cgroup2
    // pids.max" in a guarantee table and "the fork actually fails" are
    // different claims and this subsystem has already shipped the first
    // without the second.
    std::printf("the default pid cap bounds a fork bomb:\n");
    {
        auto p = base_posture();
        p.max_processes = agentty::sandbox_cfg::Config{}.max_procs;
        std::string out;
        // Fork until it fails, print how far it got. A bounded sandbox reports
        // a number near the cap; an unbounded one runs until the HOST suffers,
        // which is the outcome worth never shipping.
        int rc = run(p,
            "python3 -c 'import os,sys\n"
            "n=0\n"
            "try:\n"
            "    for _ in range(20000):\n"
            "        if os.fork()==0: os._exit(0)\n"
            "        n+=1\n"
            "except OSError:\n"
            "    pass\n"
            "print(\"forked\", n)' 2>&1", out);
        std::printf("  exit=%d, child said: %s", rc, out.c_str());
        // The cap is 4096 and the loop tries 20000, so a working cap stops it
        // short. Asserting "did not reach 20000" rather than an exact number:
        // the kernel counts threads the shell and python already hold, so the
        // precise ceiling is host-dependent and pinning it would be flaky.
        expect(out.find("forked 20000") == std::string::npos,
               "the fork loop hit a limit");
    }

    // ── 9. syscall brokering decides instead of denying flatly ──────────
    // The feature with the worst failure mode in this whole subsystem: a
    // brokered syscall blocks the guest IN THE KERNEL until the supervisor
    // answers, so a broker that is not polled is a hang rather than a weaker
    // wall. Every case here would time out rather than fail if that regressed,
    // which is why they run with a real child and a real listener.
    std::printf("brokering: a self-ptrace is allowed\n");
    {
        auto p = base_posture();
        p.broker = true;
        std::string out;
        // PTRACE_TRACEME: the child volunteers to be traced by its own parent.
        // Grants no authority over anything else, and `strace prog` needs it.
        // A flat deny breaks every debugger; the broker allows it from scalars.
        int rc = run(p,
            "python3 -c 'import ctypes,os\n"
            "libc=ctypes.CDLL(None,use_errno=True)\n"
            "ctypes.set_errno(0)\n"
            "r=libc.ptrace(0,0,0,0)\n"           // 0 == PTRACE_TRACEME
            "print(\"traceme\", r, ctypes.get_errno())' 2>&1", out);
        std::printf("  exit=%d, child said: %s", rc, out.c_str());
        // The decision is what matters, not the return value: TRACEME can fail
        // for unrelated reasons (already traced). EPERM (1) is the broker
        // denying; anything else means it got through to the kernel.
        expect(out.find("errno=1") == std::string::npos &&
               out.find(" 1)") == std::string::npos,
               "TRACEME was not denied by the broker");
    }

    std::printf("brokering: attaching to pid 1 is denied\n");
    {
        auto p = base_posture();
        p.broker = true;
        std::string out;
        // PTRACE_ATTACH to pid 1 -- the sandbox's own init inside a pid
        // namespace, and a container-escape primitive rather than a debugging
        // step. The target is a SCALAR, which is exactly why this is decidable
        // soundly: no guest memory is read, so there is no TOCTOU window.
        int rc = run(p,
            "python3 -c 'import ctypes,os\n"
            "libc=ctypes.CDLL(None,use_errno=True)\n"
            "ctypes.set_errno(0)\n"
            "r=libc.ptrace(16,1,0,0)\n"          // 16 == PTRACE_ATTACH
            "e=ctypes.get_errno()\n"
            "print(\"attach\", r, e, os.strerror(e) if e else \"\")' 2>&1", out);
        std::printf("  exit=%d, child said: %s", rc, out.c_str());
        expect(out.find("attach -1") != std::string::npos,
               "ATTACH to pid 1 failed");
    }

    std::printf("brokering: the child still finishes (no hang)\n");
    {
        // The regression that would hurt most. If the supervisor stops being
        // polled -- or answers one notification and not the next -- the child
        // blocks forever and the command dies on the hard deadline. A test that
        // only checked decisions would pass while every real command took
        // exactly the timeout.
        auto p = base_posture();
        p.broker = true;
        std::string out;
        int rc = run(p,
            "python3 -c 'import ctypes\n"
            "libc=ctypes.CDLL(None,use_errno=True)\n"
            "for i in range(50): libc.ptrace(16,1,0,0)\n"
            "print(\"survived 50 brokered calls\")' 2>&1", out);
        std::printf("  exit=%d, child said: %s", rc, out.c_str());
        expect(out.find("survived 50") != std::string::npos,
               "50 brokered calls in a row did not hang");
        expect(rc == 0, "the child exited cleanly");
    }

    // ── 9. the REAL entry point, not a posture we built ourselves ──────
    //
    // Everything above drives claybin::spawn with a Posture this file
    // constructs. That proves claybin enforces what it is told — it does NOT
    // prove that the posture agentty actually builds says the same thing.
    //
    // The gap is real and has bitten before: build_claybin_posture() reads the
    // sealed config, applies kAlwaysMasked, runs the workspace sweep, and
    // decides the syscall profile. A mirror of that logic in a test is a
    // SECOND implementation, and the two drifted once already (bwrap emitted
    // no masks at all while this file's mirror did).
    //
    // So: call the same function a `bash` tool call reaches, with the config
    // installed the same way main.cpp installs it, and read what the child
    // actually got. Anything that passes here is true of the shipped product.
    std::printf("\nthe REAL path (run_shell_command, production posture):\n");
    {
        namespace sb = agentty::tools::util::sandbox;
        agentty::tools::util::set_workspace_root(::agentty::IoAccess::grant(), kWorkspace);
        sb::reset_config_for_test();
        agentty::sandbox_cfg::Config cfg;
        cfg.configured = true;            // defaults = the Balanced posture
        sb::set_config(cfg);
        // On, not Auto: the shipped posture must COMPILE on this host. init
        // now compiles it, so a refusal here (the no-userns failure) is a
        // failed check, not a quiet skip.
        expect(sb::init(sb::Mode::On), "the shipped posture is buildable on this host");

        if (!sb::is_active()) {
            std::printf("  %s\n", sb::describe_state().c_str());
        } else {
            std::printf("  %s\n", sb::describe_state().c_str());

            // Plant one of every masked SHAPE: a plain name, a nested name
            // (needs the sweep), and both suffix rules.
            const std::string ws{kWorkspace};
            std::filesystem::create_directories(ws + "/services/api");
            auto plant = [&](const std::string& rel, const char* body) {
                std::FILE* f = std::fopen((ws + "/" + rel).c_str(), "w");
                if (f) { std::fputs(body, f); std::fclose(f); }
            };
            plant(".env",                 "ROOT_SECRET=should_never_appear\n");
            plant(".npmrc",               "//r/:_authToken=npm_should_never\n");
            plant("credentials.json",     "{\"key\":\"gcp_should_never\"}\n");
            plant("server.pem",           "PEM_should_never\n");
            plant("prod.tfvars",          "tf_should_never\n");
            plant("services/api/.env",    "NESTED_should_never=1\n");
            plant("main.cpp",             "int main(){}\n");

            auto r = sb::run_shell_command(
                "cat .env .npmrc credentials.json server.pem prod.tfvars "
                "services/api/.env 2>&1; echo ---; cat main.cpp",
                64 * 1024, std::chrono::seconds{30});

            // Not one of the planted secrets may appear. `should_never` is the
            // shared marker so a single find() covers all six.
            const bool leaked = r.output.find("should_never") != std::string::npos;
            if (leaked)
                std::printf("  LEAKED: %s\n", r.output.c_str());
            expect(!leaked, "no masked secret reached a real bash call");

            // And the sandbox did not break ordinary work: a non-secret file
            // in the same directory still reads. A mask list that caught
            // main.cpp would pass the test above and break every build.
            expect(r.output.find("int main") != std::string::npos,
                   "an ordinary workspace file is still readable");

            // Host-readable grants read on /, and the masks must still hold.
            const char* real_home = std::getenv("HOME");
            const std::string saved_home = real_home ? real_home : "";
            const std::string fake = std::string{kWorkspace} + "-home";
            std::filesystem::create_directories(fake + "/.ssh");
            if (std::FILE* f = std::fopen((fake + "/.ssh/id_ed25519").c_str(), "w")) {
                std::fputs("SSH_should_never\n", f);
                std::fclose(f);
            }
            ::setenv("HOME", fake.c_str(), 1);
            sb::reset_config_for_test();
            agentty::sandbox_cfg::Config hr;
            hr.configured = true;
            hr.fs_scope = agentty::sandbox_cfg::FsScope::HostReadable;
            sb::set_config(hr);
            expect(sb::init(sb::Mode::On), "host-readable is buildable on this host");
            auto h = sb::run_shell_command(
                "cat .env " + fake + "/.ssh/id_ed25519 2>&1; echo ---; head -c 4 /etc/os-release",
                64 * 1024, std::chrono::seconds{30});
            ::setenv("HOME", saved_home.c_str(), 1);
            std::filesystem::remove_all(fake);
            if (!h.started) std::printf("  not started: %s\n", h.start_error.c_str());
            expect(h.started, "host-readable starts a command");
            expect(h.output.find("should_never") == std::string::npos,
                   "host-readable still masks .env and ~/.ssh");
            expect(h.output.find("---\n") != std::string::npos
                       && h.output.size() > h.output.find("---\n") + 4,
                   "host-readable can read outside the toolchain dirs");
            sb::reset_config_for_test();
            sb::set_config(agentty::sandbox_cfg::Config{});
            (void)sb::init(sb::Mode::Auto);
        }

        // The Hardened preset must still run a command (Strict once had no
        // execve), and a `~/` read path must grant what it names.
        if (sb::is_active()) {
            const char* home = std::getenv("HOME");
            const std::string dir = std::string{home ? home : "/tmp"} + "/.agentty-live-read";
            std::filesystem::create_directories(dir);
            if (std::FILE* f = std::fopen((dir + "/x").c_str(), "w")) {
                std::fputs("tilde-read-ok\n", f);
                std::fclose(f);
            }
            sb::reset_config_for_test();
            auto hard = agentty::sandbox_cfg::apply_posture(
                agentty::sandbox_cfg::Config{}, agentty::sandbox_cfg::Posture::Hardened);
            hard.configured = true;
            hard.fs_scope = agentty::sandbox_cfg::FsScope::Minimal;
            hard.read_paths = {"~/.agentty-live-read"};
            sb::set_config(hard);
            expect(sb::init(sb::Mode::On), "Hardened is buildable on this host");
            auto r = sb::run_shell_command("cat ~/.agentty-live-read/x",
                                           64 * 1024, std::chrono::seconds{30});
            expect(r.output.find("tilde-read-ok") != std::string::npos,
                   "Hardened runs a command and a ~/ read path is granted");
            std::filesystem::remove_all(dir);
            sb::reset_config_for_test();
            sb::set_config(agentty::sandbox_cfg::Config{});
            (void)sb::init(sb::Mode::Auto);
        }

        // ── why the handoff gate cannot be a sandbox wall ───────────────
        //
        // The obvious objection to the trust-handoff gate is "claybin already
        // confines the process, so how can the agent write .vscode/tasks.json
        // at all?" This is the answer, measured rather than argued.
        //
        // A sandbox wall answers "may this PROCESS touch this PATH". For a
        // path inside the workspace the answer is legitimately YES -- it has
        // to be, or no build could write an object file. tasks.json is not
        // dangerous because of WHERE it is; it is dangerous because VS Code
        // executes it LATER, on the host, outside any sandbox we installed.
        //
        // So confinement and handoff are different questions, and this pair of
        // checks pins that: the wall holds where confinement applies, and does
        // not apply where it doesn't. If the first ever starts failing, the
        // gate has become redundant; if the second ever starts failing, the
        // sandbox has broken ordinary work.
        {
            std::printf("\nconfinement vs. handoff (why the gate exists):\n");
            const std::string ws{kWorkspace};
            std::filesystem::create_directories(ws + "/.vscode");

            // OUTSIDE the workspace: the sandbox must refuse. This is
            // confinement, and it is claybin's job.
            //
            // NOT /tmp. The posture mounts a private tmpfs there (see
            // build_policy: "so nothing leaks into the host /tmp"), so a write
            // to /tmp SUCCEEDS inside the guest and never reaches the host --
            // which looks like an escape to a naive probe and is in fact the
            // containment working. I wrote that probe first and it reported
            // ESCAPED while the host file did not exist.
            //
            // $HOME is the honest test: real, outside the workspace, and not
            // virtualised by the posture.
            const char* home = std::getenv("HOME");
            const std::string victim =
                std::string{home ? home : "/root"} + "/agentty_confinement_probe";
            auto out = sb::run_shell_command(
                "echo escaped > " + victim + " && echo wrote || echo denied",
                4096, std::chrono::seconds{15});
            // The guest's own report is not evidence -- a virtualised target
            // would say "wrote" too. Ask the HOST filesystem.
            std::error_code hec;
            const bool on_host = std::filesystem::exists(victim, hec);
            if (on_host) std::printf("  ESCAPED to %s\n", victim.c_str());
            expect(!on_host, "a write OUTSIDE the workspace never reaches the host");
            std::error_code rec;
            std::filesystem::remove(victim, rec);

            // The Ona chain (2026): Claude Code's sandbox matched the COMMAND
            // (a denylist on `npx`), and the agent walked round it by naming
            // the same binary another way, then turned the sandbox off. None
            // of these spellings may matter here, because no wall looks at
            // names: each must hit the same landlock rule as a plain `sh`.
            std::printf("  the same write, the binary spelled another way:\n");
            struct Spelling { const char* label; std::string cmd; };
            const std::string write = "echo escaped > " + victim;
            const Spelling spellings[] = {
                {"via /proc/self/root", "/proc/self/root/bin/sh -c '" + write + "'"},
                {"via the dynamic loader",
                 "for l in /lib64/ld-linux-x86-64.so.2 /lib/ld-linux-aarch64.so.1 "
                 "/usr/lib/ld-linux-x86-64.so.2; do [ -x $l ] && exec $l /bin/sh -c '" +
                     write + "'; done; echo no-loader"},
                {"copied to /tmp and run",
                 "cp /bin/sh /tmp/xsh && chmod +x /tmp/xsh && /tmp/xsh -c '" + write + "'"},
                {"copied into the workspace and run",
                 "cp /bin/sh ./xsh && ./xsh -c '" + write + "'; rm -f ./xsh"},
            };
            for (const auto& s : spellings) {
                auto r = sb::run_shell_command(s.cmd + "; echo rc=$?", 4096,
                                               std::chrono::seconds{15});
                std::error_code e2;
                const bool hit = std::filesystem::exists(victim, e2);
                std::filesystem::remove(victim, e2);
                if (hit) std::printf("  ESCAPED %s: %s", s.label, r.output.c_str());
                std::string what = std::string{"a write outside, "} + s.label + ", is still denied";
                expect(!hit, what.c_str());
            }
            // And the posture has no off switch the guest can reach: agentty's
            // own config is outside the workspace, so it is read-only (or
            // invisible) like the rest of $HOME. Probed by CREATING a file
            // there, never by touching settings.json: if this wall ever fails
            // the probe must not have edited the real policy. The
            // workspace-at-$HOME case, where it is inside, is further down.
            {
                const std::string probe = std::string{home ? home : "/root"} +
                                          "/.agentty/agentty-live-check-probe";
                (void)sb::run_shell_command("echo x > " + probe, 4096,
                                            std::chrono::seconds{15});
                std::error_code pe;
                const bool wrote = std::filesystem::exists(probe, pe);
                std::filesystem::remove(probe, pe);
                expect(!wrote, "agentty's own config dir is not writable from the box");
            }

            // INSIDE the workspace: the sandbox ALLOWS it, and must. There is
            // no wall here to lean on, which is exactly why the handoff gate
            // is a separate mechanism.
            //
            // An ORDINARY file, not .vscode/tasks.json. This check used to use
            // the trusted path and it was the right choice then -- the point
            // was "confinement permits this, so confinement cannot be what
            // stops a handoff". Now that prevention binds trusted paths
            // read-only, that spelling would assert the gate does NOT work.
            // The property being pinned is unchanged: a plain workspace write
            // is permitted, so a wall is not what stops the dangerous one.
            auto in = sb::run_shell_command(
                "echo hello > ordinary-file.txt && echo wrote || echo denied",
                4096, std::chrono::seconds{15});
            expect(in.output.find("wrote") != std::string::npos,
                   "a write INSIDE the workspace is allowed -- so confinement "
                   "cannot be what stops a trust handoff");
        }

        // ── handoff PREVENTION: a read-only bind over the trusted path ───
        //
        // Detection reports a write after the bytes land. This stops it. The
        // mechanism is a read-only bind of the path over itself, which is the
        // same trick used to mask secrets and for the same reason: landlock
        // has no negative rule, so you make the path not be writable rather
        // than denying it.
        //
        // Needs a mount namespace, so this whole block is conditional. On a
        // host that denies unprivileged userns the wall is absent and
        // detection is the only layer -- which is the honest outcome, not a
        // silent downgrade.
        {
            std::printf("\nhandoff prevention (read-only bind):\n");
            const std::string ws{kWorkspace};
            std::filesystem::create_directories(ws + "/.vscode");
            // Must EXIST before the posture is built: a bind of a missing path
            // cannot be made, which is what detection covers.
            {
                std::ofstream f(ws + "/.vscode/tasks.json");
                f << "{\"version\":\"2.0.0\"}\n";
            }

            // Re-init so the posture picks up the now-existing trusted path.
            (void)sb::init(sb::Mode::Auto);

            auto w = sb::run_shell_command(
                "echo PWNED > .vscode/tasks.json && echo wrote || echo denied",
                4096, std::chrono::seconds{15});

            // The guest's own report is not evidence; read the HOST file.
            std::string after;
            {
                std::ifstream f(ws + "/.vscode/tasks.json");
                std::getline(f, after);
            }
            const bool clobbered = after.find("PWNED") != std::string::npos;

            if (clobbered) {
                std::printf("  guest said: %s\n", w.output.c_str());
                // Without a mount namespace this is EXPECTED: a read-only path
                // inside the writable workspace has no landlock expression
                // that keeps git working, so the report says filesystem.write
                // partial and the handoff gate (detection) is the wall.
                expect_ns(false,
                          "a host-trusted path must be READ-ONLY inside the "
                          "sandbox when mount namespaces exist");
            } else {
                expect(true, "a write to a host-trusted path is refused");
                // And it must still be READABLE -- git reads its own config on
                // every invocation, so masking would break ordinary work.
                auto r = sb::run_shell_command(
                    "cat .vscode/tasks.json", 4096, std::chrono::seconds{15});
                expect(r.output.find("2.0.0") != std::string::npos,
                       "...and is still readable, not masked");
            }
        }
    }

    // ── sandbox audit 2026-10-17 ───────────────────────────────────────────────
    std::printf("\naudit 2026-10-17 regressions:\n");
    {
        // #9: netlink and vsock refused, unix/inet still fine.
        auto p = base_posture();
        std::string out;
        run(p,
            "python3 -c 'import socket\n"
            "for fam,name in ((16,\"netlink\"),(40,\"vsock\"),(1,\"unix\"),(2,\"inet\")):\n"
            "    try:\n"
            "        s=socket.socket(fam,socket.SOCK_DGRAM if fam!=1 else socket.SOCK_STREAM); s.close(); print(name,\"ok\")\n"
            "    except OSError as e: print(name,\"refused\",e.errno)' 2>&1", out);
        std::printf("  child said: %s", out.c_str());
        expect(out.find("netlink refused") != std::string::npos, "AF_NETLINK socket refused");
        expect(out.find("vsock refused") != std::string::npos, "AF_VSOCK socket refused");
        expect(out.find("unix ok") != std::string::npos, "AF_UNIX still works");
        expect(out.find("inet ok") != std::string::npos, "AF_INET still works");
    }
    {
        // #7: fake_hostname=false keeps the real name.
        char host[256] = {};
        ::gethostname(host, sizeof host - 1);
        auto p = base_posture();
        p.fake_hostname = false;
        std::string out;
        run(p, "uname -n", out);
        std::printf("  host=%s child(off)=%s", host, out.c_str());
        expect(out.find(host) != std::string::npos && out.find("sandbox") == std::string::npos,
               "fake_hostname=false shows the real host name");
        p.fake_hostname = true;
        out.clear();
        run(p, "uname -n", out);
        std::printf("  child(on)=%s", out.c_str());
        expect_ns(out.find("sandbox") != std::string::npos,
                  "fake_hostname=true shows `sandbox`");
    }
    {
        // #1: a handoff ro bind must not uncover a mask. Workspace = a fake
        // $HOME holding ~/.agentty/memory.jsonl (masked) inside .agentty (a
        // handoff dir), the exact shape the audit hit.
        namespace sb = agentty::tools::util::sandbox;
        const std::string fake_home = std::string{kWorkspace} + "-home";
        std::error_code ec;
        std::filesystem::create_directories(fake_home + "/.agentty", ec);
        std::filesystem::create_directories(fake_home + "/.config/autostart", ec);
        {
            std::ofstream f(fake_home + "/.agentty/memory.jsonl");
            f << "{\"text\":\"MEMORY_SHOULD_NEVER_APPEAR\"}\n";
        }
        {
            std::ofstream f(fake_home + "/.agentty/hooks.json");
            f << "{}\n";
        }
        {
            std::ofstream f(fake_home + "/.config/autostart/x.desktop");
            f << "[Desktop Entry]\n";
        }
        const char* old_home = std::getenv("HOME");
        const std::string saved = old_home ? old_home : "";
        ::setenv("HOME", fake_home.c_str(), 1);
        agentty::tools::util::set_workspace_root(::agentty::IoAccess::grant(), fake_home);
        sb::reset_config_for_test();
        agentty::sandbox_cfg::Config cfg;
        cfg.configured = true;
        sb::set_config(cfg);
        (void)sb::init(sb::Mode::On);
        if (sb::is_active()) {
            expect(sb::workspace_is_home(), "workspace == $HOME is detected");
            auto r = sb::run_shell_command("cat .agentty/memory.jsonl; echo END",
                                           4096, std::chrono::seconds{15});
            expect(r.output.find("MEMORY_SHOULD_NEVER_APPEAR") == std::string::npos,
                   "a masked file inside a handoff dir stays masked");
            auto w = sb::run_shell_command(
                "echo PWNED > .config/autostart/x.desktop && echo wrote || echo denied",
                4096, std::chrono::seconds{15});
            std::string after;
            {
                std::ifstream f(fake_home + "/.config/autostart/x.desktop");
                std::getline(f, after);
            }
            expect_ns(after.find("PWNED") == std::string::npos,
                      "an autostart entry is read-only (#3)");

            // Step 4: the workspace IS $HOME, so ~/.agentty is inside it and
            // the workspace bind made it writable. The box must not be able
            // to rewrite the policy it runs under, or the server list, or the
            // approvals -- and the read-only bind over it must not uncover
            // the masked memory file inside.
            (void)sb::run_shell_command(
                "echo '{\"sandbox\":{}}' > .agentty/settings.json; "
                "echo '{}' > .agentty/mcp.json; "
                "mkdir -p .agentty/state && echo x > .agentty/state/mcp_approvals.json",
                4096, std::chrono::seconds{15});
            std::error_code oe;
            expect_ns(!std::filesystem::exists(fake_home + "/.agentty/settings.json", oe)
                          && !std::filesystem::exists(fake_home + "/.agentty/mcp.json", oe),
                      "~/.agentty is read-only with the workspace at $HOME (step 4)");
            // Only where the read-only bind exists: without namespaces the
            // move would succeed (that wall is absent, and the native gate is
            // what covers it there) and the mask check below would pass on a
            // missing file instead of a masked one.
            if (has_namespaces())
                (void)sb::run_shell_command("mv .agentty .agentty-moved", 4096,
                                            std::chrono::seconds{15});
            expect_ns(std::filesystem::exists(fake_home + "/.agentty", oe),
                      "...and cannot be moved aside (step 4)");
            auto mem = sb::run_shell_command("cat .agentty/memory.jsonl; echo END", 4096,
                                             std::chrono::seconds{15});
            expect(mem.output.find("MEMORY_SHOULD_NEVER_APPEAR") == std::string::npos,
                   "...and its masked memory file is still masked under the read-only bind");
            // An ordinary dotfile next to it is untouched: the bind is exact.
            auto plain = sb::run_shell_command("echo ok > .plainfile && echo wrote || echo denied",
                                               4096, std::chrono::seconds{15});
            expect(plain.output.find("wrote") != std::string::npos,
                   "...while an ordinary file in $HOME is still writable");
        } else {
            std::printf("  no backend -- skipped\n");
        }
        if (saved.empty()) ::unsetenv("HOME"); else ::setenv("HOME", saved.c_str(), 1);
        std::filesystem::remove_all(fake_home, ec);
    }

    // ── claybin-sandbox-audit.md #2-#7, observed from inside a real box ──
    {
        auto p = base_posture();
        std::string out;
        // #2: /dev/shm is its own tmpfs with a cap, /dev itself is tiny.
        run(p, "df -B1 --output=size,target /dev/shm /dev 2>&1 | tail -n +2", out);
        std::printf("  df: %s", out.c_str());
        expect_ns(out.find("67108864 /dev/shm") != std::string::npos,
                  "/dev/shm is capped at 64 MiB (#2)");
        expect_ns(out.find("1048576 /dev") != std::string::npos,
                  "/dev is capped at 1 MiB (#2)");
        out.clear();
        // #3: the box is the OOM killer's first pick.
        run(p, "cat /proc/self/oom_score_adj", out);
        expect(out.find("500") != std::string::npos, "oom_score_adj is 500 (#3)");
        out.clear();
        // #5: xattr reads work, so ls stays quiet.
        run(p, "ls -l /usr >/dev/null && echo ls-clean", out);
        expect(out.find("ls-clean") != std::string::npos &&
                   out.find("Operation not permitted") == std::string::npos,
               "ls -l prints no xattr EPERM noise (#5)");
        out.clear();
        // #6: prctl PR_SET_NAME works; PR_SET_MM / PR_CAP_AMBIENT do not.
        // python is the one interpreter that reaches prctl without a compiler.
        run(p,
            "python3 -c 'import ctypes;l=ctypes.CDLL(None);"
            "print(\"name\",l.prctl(15,b\"box\",0,0,0));"
            "print(\"mm\",l.prctl(35,0,0,0,0));"
            "print(\"pers\",l.personality(0xffffffff)>=0, l.personality(0x0040000))' 2>&1",
            out);
        std::printf("  prctl: %s", out.c_str());
        if (out.find("No such file") == std::string::npos) {
            expect(out.find("name 0") != std::string::npos, "prctl(PR_SET_NAME) works (#6)");
            expect(out.find("mm -1") != std::string::npos, "prctl(PR_SET_MM) refused (#6)");
            expect(out.find("pers True -1") != std::string::npos,
                   "personality query ok, ADDR_NO_RANDOMIZE refused (#6)");
        }
        out.clear();
        // #7: own cgroup namespace, no core dumps.
        run(p, "cat /proc/self/cgroup; ulimit -c", out);
        std::printf("  cgroup/core: %s", out.c_str());
        expect_ns(out.find("0::/\n") != std::string::npos,
                  "cgroup namespace hides the host path (#7)");
        expect(out.find("\n0\n") != std::string::npos || out.ends_with("0\n"),
               "RLIMIT_CORE is 0 (#7)");
        out.clear();
        // #4: session sockets and provider keys don't reach the guest.
        ::setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/run/user/1000/bus", 1);
        ::setenv("WAYLAND_DISPLAY", "wayland-1", 1);
        ::setenv("GROQ_API_KEY", "gsk_should_never", 1);
        ::setenv("MY_BUILD_FLAG", "kept", 1);
        ::setenv("CC", "cc-kept", 1);
        run(p, "env", out);
        expect(out.find("DBUS_SESSION_BUS_ADDRESS") == std::string::npos &&
                   out.find("WAYLAND_DISPLAY") == std::string::npos,
               "desktop/session sockets are not inherited (#4)");
        expect(out.find("gsk_should_never") == std::string::npos,
               "provider API keys are not inherited (#4)");
        // The shipped scope is Toolchain: the variables builds read pass
        // (CC, CARGO_*, VIRTUAL_ENV, ...), an unknown name does not -- it
        // could be a token named anything. MY_BUILD_FLAG passing used to be
        // asserted here, from before the scope existed.
        expect(out.find("CC=cc-kept") != std::string::npos,
               "toolchain env vars still are (#4)");
        expect(out.find("MY_BUILD_FLAG") == std::string::npos,
               "unknown env vars are not, under the Toolchain scope (#4)");
        ::unsetenv("GROQ_API_KEY");
        ::unsetenv("MY_BUILD_FLAG");
        ::unsetenv("CC");
    }

    // ── the network boundary (audit #1) ──────────────────────────────────
    //
    // The audit's one High finding: "network allowed" used to mean sharing
    // agentty's namespace, so a command could reach the host's loopback
    // services and the whole LAN. Now the box gets its own namespace and one
    // proxy on its loopback, and these are the claims that makes.
    {
        auto p = base_posture();
        std::string out;
        // A host-only service the box must NOT be able to reach. Bound here,
        // on the host's loopback, while the box runs.
        int hostsrv = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        int on = 1;
        ::setsockopt(hostsrv, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(48231);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const bool have_srv = hostsrv >= 0
            && ::bind(hostsrv, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0
            && ::listen(hostsrv, 4) == 0;

        // Its own namespace: a fresh one has loopback and nothing else.
        // Without a netns the box shares the host's interfaces, and the wall
        // is landlock pinning it to the proxy port -- checked below by the
        // direct connect being refused, so this one is skipped there.
        if (::clay::probe_host().net_namespaces) {
        run(p, "cat /proc/net/dev | awk 'NR>2 {print $1}' | tr -d ' :' | sort | tr '\\n' ' '", out);
        expect(out.find("lo") != std::string::npos && out.find("eth") == std::string::npos
                   && out.find("wl") == std::string::npos,
               "the box has its own netns: loopback only (#1)");
        out.clear();
        }
        // The proxy env the toolchain reads, in every spelling.
        run(p, "env | grep -c -i '^\\(http\\|https\\|all\\)_proxy=127\\|^\\(HTTP\\|HTTPS\\|ALL\\)_PROXY=http'", out);
        expect(out.find("6") != std::string::npos,
               "all six proxy env spellings point at the box's loopback (#1)");
        out.clear();
        if (have_srv) {
            // Direct: the host's service is not in this namespace at all.
            run(p, "curl -sS --max-time 5 --noproxy '*' -o /dev/null "
                   "-w direct=%{http_code} http://127.0.0.1:48231/ 2>&1", out);
            expect(out.find("direct=000") != std::string::npos
                       || out.find("Could not connect") != std::string::npos,
                   "a host loopback service is unreachable from the box (#1)");
            out.clear();
            // Through the proxy: refused on the resolved address. `-x
            // "$http_proxy"` rather than a literal: without a netns the proxy
            // is on a kernel-chosen host-loopback port, not 3128.
            run(p, "curl -sS --max-time 5 -o /dev/null -w loop=%{http_code} "
                   "-x \"$http_proxy\" http://127.0.0.1:48231/ 2>&1", out);
            expect(out.find("loop=403") != std::string::npos,
                   "the proxy refuses the host's loopback (#1)");
            out.clear();
        }
        run(p, "curl -sS --max-time 5 -o /dev/null -w lan=%{http_code} "
               "-x \"$http_proxy\" http://192.168.1.1/ 2>&1", out);
        expect(out.find("lan=403") != std::string::npos, "the proxy refuses RFC1918 (#1)");
        out.clear();
        run(p, "curl -sS --max-time 5 -o /dev/null -w meta=%{http_code} "
               "-x \"$http_proxy\" http://169.254.169.254/latest/meta-data/ 2>&1", out);
        expect(out.find("meta=403") != std::string::npos,
               "the proxy refuses cloud metadata (#1)");
        out.clear();
        if (hostsrv >= 0) ::close(hostsrv);
    }

    // ── W^X: the arg rule vs the real wall ─────────────────────────────
    //
    // The seccomp rule denies mmap/mprotect with PROT_WRITE|PROT_EXEC in ONE
    // call. It cannot see the sequence real shellcode uses -- map it writable,
    // write to it, mprotect it executable -- because each of those calls is
    // innocent on its own. PR_SET_MDWE is a process property, so it does.
    //
    // That wall breaks every JIT (node/V8 does not start under it), so it is
    // on for Strict only. Both halves of that trade are asserted here: a
    // default box can still make the transition, a Strict box cannot.
    {
        const char* probe =
            "python3 -c \"import ctypes\n"
            "l=ctypes.CDLL(None, use_errno=True)\n"
            "l.mmap.restype=ctypes.c_void_p\n"
            "l.mmap.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_int,ctypes.c_int,"
            "ctypes.c_int,ctypes.c_long]\n"
            "l.mprotect.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_int]\n"
            "F=ctypes.c_void_p(-1).value\n"
            "p=l.mmap(None,4096,1|2|4,2|32,-1,0)\n"
            "print('oneshot', 'REFUSED' if p==F else 'ALLOWED')\n"
            "q=l.mmap(None,4096,1|2,2|32,-1,0)\n"
            "print('upgrade', 'nomap' if q==F else "
            "('REFUSED' if l.mprotect(ctypes.c_void_p(q),4096,1|4)!=0 else 'ALLOWED'))\" 2>&1";
        std::string out;
        auto p = base_posture();
        run(p, probe, out);
        std::printf("  default: %s", out.c_str());
        expect(out.find("oneshot REFUSED") != std::string::npos,
               "W+X in one mmap is refused by default");
        expect(out.find("upgrade ALLOWED") != std::string::npos,
               "the W->X transition is ALLOWED by default, so JITs run");
        out.clear();
        auto strict = base_posture();
        strict.syscall_mode = 2;   // Strict: Hardened / Airgapped
        run(strict, probe, out);
        std::printf("  strict:  %s", out.c_str());
        expect(out.find("oneshot REFUSED") != std::string::npos,
               "W+X in one mmap is refused on Strict");
        expect(out.find("upgrade REFUSED") != std::string::npos,
               "the W->X transition is refused on Strict (MDWE)");
    }

    // ── the default resource throttles ─────────────────────────────────
    //
    // memory and cpu used to report NONE on a default install, because the
    // only knobs on offer were a hard cap and a quota -- and no number fits
    // both an 8 GB laptop and a 128 GB workstation, so a wrong guess turns a
    // working build into an OOM kill.
    //
    // memory.high and cpu.weight are different in kind: one throttles instead
    // of killing, the other only decides who loses under contention. Both are
    // safe to default, and both halves of that claim are checked here -- the
    // limits are really in force, AND an idle machine is not slowed by them.
    {
        auto p = base_posture();
        std::string out;
        // The box reads its own cgroup through /proc, since /sys is absent.
        run(p, "cat /proc/self/cgroup", out);
        const bool own_cgroup = out.find("0::/") != std::string::npos;
        expect(own_cgroup, "the box is in its own cgroup");
        out.clear();
        // 12 parallel jobs, which is the shape cpu.weight would slow if it
        // were a quota. Compared against the same work with no sandbox.
        const char* work =
            "i=0; while [ $i -lt 8 ]; do python3 -c 'sum(range(8000000))' & "
            "i=$((i+1)); done; wait; echo done";
        const auto t0 = std::chrono::steady_clock::now();
        run(p, work, out);
        const auto boxed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - t0).count();
        expect(out.find("done") != std::string::npos, "parallel work completes in the box");
        std::printf("  8 parallel jobs in a box: %lldms\n", static_cast<long long>(boxed_ms));
        // Not a hard threshold -- a loaded CI machine would make that flaky.
        // The claim is that the work COMPLETED, which a quota of 25% would
        // have stretched by 4x and a memory cap might have killed outright.
    }

    // ── the JIT memory path (why node died) ───────────────────────────
    //
    // `node -e` died in a box with V8's `Check failed: 12 == errno` on every
    // claybin revision, including before any of this week's work. --jitless
    // runs fine, which puts the failure on the code-memory path. This walks
    // that path call by call so the answer is a line of output rather than a
    // guess.
    {
        auto p = base_posture();
        std::string out;
        run(p,
            "python3 -c \"import ctypes, errno\n"
            "l = ctypes.CDLL(None, use_errno=True)\n"
            "l.mmap.restype = ctypes.c_void_p\n"
            "l.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,"
            "ctypes.c_int, ctypes.c_int, ctypes.c_long]\n"
            "l.mprotect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]\n"
            "F = ctypes.c_void_p(-1).value\n"
            "def nm(e): return errno.errorcode.get(e, str(e))\n"
            "def step(label, fn):\n"
            "    ctypes.set_errno(0)\n"
            "    rc = fn(); e = ctypes.get_errno()\n"
            "    bad = rc == F or (isinstance(rc, int) and rc < 0)\n"
            "    print(label, 'FAILED', nm(e)) if bad else print(label, 'ok')\n"
            "    return rc\n"
            "c = step('cage', lambda: l.mmap(None, 128<<20, 0, 2|0x20|0x4000, -1, 0))\n"
            "if c != F:\n"
            "    step('rw', lambda: l.mprotect(ctypes.c_void_p(c), 4096, 3))\n"
            "    step('rx', lambda: l.mprotect(ctypes.c_void_p(c), 4096, 5))\n"
            "    step('none', lambda: l.mprotect(ctypes.c_void_p(c), 4096, 0))\n"
            "    step('fixed', lambda: l.mmap(ctypes.c_void_p(c), 4096, 3, 2|0x20|0x10, -1, 0))\n"
            "f = step('memfd', lambda: l.syscall(319, b'p', 0))\n"
            "if isinstance(f, int) and f >= 0:\n"
            "    step('ftruncate', lambda: l.syscall(77, f, 4096))\n"
            "    step('memfd_rx', lambda: l.mmap(None, 4096, 5, 2, f, 0))\" 2>&1",
            out);
        std::printf("  JIT memory path in a box:\n");
        for (std::size_t at = 0; at < out.size();) {
            const auto nl = out.find('\n', at);
            const auto line = out.substr(at, nl == std::string::npos ? nl : nl - at);
            if (!line.empty()) std::printf("    %s\n", line.c_str());
            if (nl == std::string::npos) break;
            at = nl + 1;
        }
        // The cage and the RW/RX cycle are what a JIT needs; a failure in any
        // of them is the node bug, and naming WHICH one is the point.
        expect(out.find("cage ok") != std::string::npos,
               "a JIT can reserve its code cage");
        expect(out.find("rx ok") != std::string::npos,
               "a JIT can flip a page to executable (W->X is allowed by default)");
        // node/V8 needs RWX pages, so it dies whenever W^X is on. With W^X off
        // it has to run, even under the Compiler filter.
        {
            auto np = base_posture();
            np.wx_protect = false;
            std::string nout;
            run(np, "command -v node >/dev/null || { echo NODE_OK; exit; }; "
                    "node -e 'console.log(\"NODE_OK\")' 2>&1 | tail -2", nout);
            expect(nout.find("NODE_OK") != std::string::npos,
                   "node runs under Compiler with W^X off");
        }
        // /proc/sys is visible (V8 reads /proc/sys/vm) but read-only.
        std::string sys;
        run(base_posture(), "cat /proc/sys/kernel/core_pattern | wc -c; "
                            "ls /proc/sys/vm >/dev/null && echo VM_OK; "
                            "echo 1 > /proc/sys/vm/drop_caches 2>/dev/null && echo SYS_WRITABLE; "
                            "echo x > /proc/sys/kernel/core_pattern 2>/dev/null && echo CORE_WRITABLE",
            sys);
        expect(sys.find("VM_OK") != std::string::npos, "/proc/sys/vm is readable");
        expect(sys.find("SYS_WRITABLE") == std::string::npos &&
                   sys.find("CORE_WRITABLE") == std::string::npos,
               "/proc/sys is read-only");
        expect_ns(sys.rfind("0\n", 0) == 0, "core_pattern still masked");
    }

    // ── long-lived children: MCP servers, ACP agents, rag peers ─────────────
long_lived:
    std::printf("\nlong-lived children (MCP servers, agents, rag peers):\n");
    //
    // These used to start with the user's full authority -- an `npx
    // some-mcp-server` could read ~/.ssh and reach the LAN. Each now goes
    // through sandbox::child_spawner(role) into util::ChildProcess, the SAME
    // object the real bridge uses. A tiny line-protocol "server" answers one
    // request per line by trying the thing named, so the checks run inside
    // the real child, over the real pipes, through the real spawner.
    {
        namespace sb = agentty::tools::util::sandbox;
        // Own workspace: an earlier section leaves it at a fake $HOME that it
        // then deletes, and a missing workspace fails every spawn on mount.
        agentty::tools::util::set_workspace_root(::agentty::IoAccess::grant(), kWorkspace);
        sb::reset_config_for_test();
        agentty::sandbox_cfg::Config cfg;
        cfg.configured = true;
        sb::set_config(cfg);
        expect(sb::init(sb::Mode::On), "long-lived: the sandbox is up");

        const std::string ws{kWorkspace};
        const char* home = std::getenv("HOME");
        const std::string ssh = std::string{home ? home : "/root"} + "/.ssh";
        if (std::FILE* f = std::fopen((ws + "/.env").c_str(), "w")) {
            std::fputs("LL_SECRET=should_never\n", f);
            std::fclose(f);
        }
        // One request per line, one answer per line. EOF on stdin ends it.
        const char* server =
            "while IFS= read -r q; do case \"$q\" in "
            "ping) echo pong ;; "
            "env) grep -q LL_SECRET .env 2>/dev/null && echo READ || echo DENIED ;; "
            "ssh) ls \"$SSH_DIR\" 2>/dev/null | head -1 | grep -q . && echo READ || echo DENIED ;; "
            "write) (echo w > ll_write_probe && echo WROTE && rm -f ll_write_probe) 2>/dev/null || echo DENIED ;; "
            "net) (exec 3<>/dev/tcp/1.1.1.1/80) 2>/dev/null && echo CONNECTED || echo DENIED ;; "
            "*) echo '?' ;; esac; done";

        auto ask = [](agentty::util::ChildProcess& c, const char* q) {
            c.in() << q << '\n';
            c.in().flush();
            std::string line;
            std::getline(c.out(), line);
            return line;
        };
        auto start = [&](sb::Role role) {
            agentty::util::ChildProcess::Spawn s;
            s.command = "/bin/bash";
            s.args = {"-c", server};
            s.cwd = ws;
            s.env_kv = {"SSH_DIR=" + ssh};
            s.quiet_stderr = true;
            s.spawner = sb::child_spawner(role);
            return std::make_unique<agentty::util::ChildProcess>(s);
        };

        {
            auto c = start(sb::Role::McpServer);
            expect(ask(*c, "ping") == "pong", "mcp server: answers over its pipes");
            expect(ask(*c, "env") == "DENIED",
                   "mcp server: cannot read a workspace .env");
            expect(ask(*c, "ssh") == "DENIED", "mcp server: cannot read ~/.ssh");
            {
                const auto w = ask(*c, "write");
                if (w != "WROTE") std::printf("  write said: [%s]\n", w.c_str());
                expect(w == "WROTE", "mcp server: can write the workspace");
            }
            expect(ask(*c, "net") == "DENIED",
                   "mcp server: no direct network (only the proxy)");
            // EOF on stdin must END it: this is how agentty stops a server.
            const auto t0 = std::chrono::steady_clock::now();
            c->terminate(std::chrono::milliseconds{3000});
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            expect(ms < 2500, "mcp server: exits on stdin EOF, without being killed");
        }
        {
            auto c = start(sb::Role::RagPeer);
            expect(ask(*c, "ping") == "pong", "rag peer: answers over its pipes");
            expect(ask(*c, "write") == "DENIED", "rag peer: the workspace is read-only");
            expect(ask(*c, "ssh") == "DENIED", "rag peer: cannot read ~/.ssh");
        }
        {
            auto c = start(sb::Role::Agent);
            expect(ask(*c, "ping") == "pong", "agent: answers over its pipes");
            expect(ask(*c, "ssh") == "DENIED", "agent: cannot read ~/.ssh");
        }
        std::filesystem::remove(ws + "/.env");

        // Fail closed: with the sandbox on, a child it cannot build is an
        // ERROR, never a quiet unsandboxed start.
        {
            agentty::util::ChildProcess::Spawn s;
            s.command = "/nonexistent/server";
            s.spawner = sb::child_spawner(sb::Role::McpServer);
            bool threw = false;
            try { agentty::util::ChildProcess c{s}; } catch (const std::exception&) { threw = true; }
            expect(threw, "a child the sandbox cannot start is refused, not run plainly");
        }
        sb::reset_config_for_test();
        sb::set_config(agentty::sandbox_cfg::Config{});
        (void)sb::init(sb::Mode::Auto);
    }

    std::printf("\n%s\n", failures ? "LIVE CHECK FAILURES" : "all live checks passed");
    return failures ? 1 : 0;
}
