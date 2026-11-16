// Native multilib coverage of the shipped set_config -> init -> shell path.
// Run with --require to treat missing host capabilities as failures.
#include "agentty/tool/util/sandbox.hpp"
#include "agentty/tool/util/subprocess.hpp"
#include "agentty/domain/sandbox_config.hpp"
#include "agentty/util/io.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#if defined(__linux__)
#include <unistd.h>
#endif

namespace sb = agentty::tools::util::sandbox;
namespace util = agentty::tools::util;
// Same narrow tool-layer stub as exec_exit_probe / sandbox_live_check.
namespace agentty::tools::util { void set_workspace_root(Io, std::filesystem::path); }

namespace {
std::string quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) out += c == '\'' ? "'\\''" : std::string(1, c);
    return out + "'";
}
void save(const std::filesystem::path& p, const char* text) {
    std::ofstream f(p);
    f << text;
    if (!f) throw std::runtime_error("cannot write fixture: " + p.string());
}
bool expect(const char* label, const util::SubprocessResult& r, int code) {
    if (r.started && !r.timed_out && r.exit_code == code) {
        std::printf("PASS: %s (exit %d)\n", label, code);
        return true;
    }
    std::fprintf(stderr, "FAIL: %s: started=%d timeout=%d exit=%d expected=%d\n%s%s\n",
                 label, int(r.started), int(r.timed_out), r.exit_code, code,
                 r.start_error.c_str(), r.output.c_str());
    return false;
}
int skip(bool require, const std::string& why) {
    std::fprintf(stderr, "%s: %s\n", require ? "FAIL (--require)" : "SKIP", why.c_str());
    return require ? 1 : 77;
}
}

int main(int argc, char** argv) {
    bool require = false;
    bool explicit_loader = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--require") require = true;
        else if (arg == "--explicit-loader") explicit_loader = true;
        else {
            std::fprintf(stderr, "usage: %s [--require] [--explicit-loader]\n", argv[0]);
            return 1;
        }
    }
#if !defined(__linux__) || !defined(__x86_64__)
    return skip(require, "requires native Linux x86_64");
#else
    std::string pattern = (std::filesystem::current_path() / "agentty-multilib-XXXXXX").string();
    const char* made = ::mkdtemp(pattern.data());
    if (!made) { std::perror("mkdtemp"); return 1; }
    const std::filesystem::path root(made);
    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(p, ec);
            std::filesystem::remove(p.string() + "-outside", ec);
        }
    } cleanup{root};
    try {
        const std::string outside = root.string() + "-outside";
        save(outside, "host-writable sentinel\n");
        const auto host = [&](const std::string& cmd) {
            return util::run_command_s("cd " + quote(root.string()) + " && " + cmd,
                                       64 * 1024, std::chrono::seconds{30});
        };
        // Do not customize PT_INTERP: the system's default i386 loader is the regression.
        save(root / "baseline.c", "int main(void) { return 42; }\n");
        if (!expect("compile native baseline", host("gcc -m64 baseline.c -o baseline64"), 0)) return 1;
        if (!expect("host native baseline", host("./baseline64"), 42)) return 1;

        save(root / "library.c", "int multilib_answer(void) { return 42; }\n");
        save(root / "dynamic.c", R"C(
#include <dlfcn.h>
int main(void) {
    void *h = dlopen("./libmultilib.so", RTLD_NOW);
    if (!h) return 10;
    int (*answer)(void) = (int (*)(void))dlsym(h, "multilib_answer");
    if (!answer) return 11;
    int result = answer();
    return dlclose(h) == 0 ? result : 12;
}
)C");
        save(root / "restrictions.c", R"C(
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/sched.h>
#include <unistd.h>
static int denied(void) { return errno == EPERM || errno == EACCES || errno == EROFS; }
int main(int argc, char **argv) {
    if (argc != 3) return 20;
    if (!strcmp(argv[1], "write")) {
        int fd = open("workspace-write", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0 || write(fd, "ok", 2) != 2 || close(fd)) return 21;
        fd = open(argv[2], O_WRONLY | O_TRUNC);
        if (fd >= 0) { close(fd); return 22; }
        return denied() ? 0 : 23;
    }
    if (!strcmp(argv[1], "unshare")) {
        (void)syscall(SYS_unshare, CLONE_NEWUSER);
        return 24; /* Hard denied in both ABIs, not a brokered syscall. */
    }
    if (!strcmp(argv[1], "rwx")) {
        void *p = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) { munmap(p, 4096); return 25; }
        return denied() ? 0 : 26;
    }
    if (!strcmp(argv[1], "mprotect")) {
        void *p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return 27;
        errno = 0;
        int ok = mprotect(p, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == -1 && denied();
        munmap(p, 4096);
        return ok ? 0 : 28;
    }
    return 29;
}
)C");
        if (!expect("compile native restrictions", host("gcc -m64 restrictions.c -o restrictions64"), 0)) return 1;

        util::set_workspace_root(agentty::IoAccess::grant(), root);
        agentty::sandbox_cfg::Config cfg; // Default balanced/compiler policy; no relaxed syscall overrides.
        cfg.configured = true;
        // Visible and host-writable, but deliberately not in write_paths.
        cfg.read_paths.push_back(outside);
        cfg.net_mode = agentty::sandbox_cfg::NetMode::None;
        sb::set_config(cfg);
        if (!sb::init(sb::Mode::On) || !sb::is_active())
            return skip(require, "real sandbox unavailable: " + sb::describe_state());
        if (sb::requested_mode() != sb::Mode::On || sb::detected_backend() != sb::Backend::Claybin) {
            std::fprintf(stderr, "FAIL: expected Mode::On and claybin: %s\n", sb::describe_state().c_str());
            return 1;
        }
        const auto sandbox = [&](const std::string& cmd) {
            return sb::run_shell_command("cd " + quote(root.string()) + " && " + cmd,
                                          64 * 1024, std::chrono::seconds{30});
        };
        // Once init claims a real backend, execution failures are not downgraded to skips.
        bool ok = expect("sandbox native baseline", sandbox("./baseline64"), 42);
        const auto restrictions = [&](const char* bits) {
            bool passed = true;
            for (const char* check : {"write", "rwx", "mprotect"}) {
                const std::string cmd = "./restrictions" + std::string(bits) + " " + check + " " + quote(outside);
                passed = expect(cmd.c_str(), sandbox(cmd), 0) && passed;
            }
            // unshare is deliberately KILL in both profiles: SIGSYS maps to 159.
            const std::string cmd = "./restrictions" + std::string(bits) + " unshare " + quote(outside);
            return expect(cmd.c_str(), sandbox(cmd), 159) && passed;
        };
        ok = restrictions("64") && ok;
        if (!ok) return 1;

        // Only i386 coverage may be skipped after the native sandbox checks pass.
        const auto multilib = host("gcc -m32 baseline.c -o baseline32");
        if (!multilib.started || multilib.timed_out) {
            expect("multilib compiler invocation", multilib, 0);
            return 1;
        }
        if (multilib.exit_code != 0)
            return skip(require, "i386 coverage only: gcc -m32 cannot build the minimal libc fixture:\n" + multilib.output);
        const auto runtime = host("./baseline32");
        if (runtime.started && !runtime.timed_out &&
            (runtime.exit_code == 126 || runtime.exit_code == 127)) {
            if (explicit_loader) {
                std::fprintf(stderr, "DIAGNOSTIC ONLY: explicit loader does not satisfy default-interpreter coverage\n");
                const auto compiled = host("gcc -m32 baseline.c -Wl,--dynamic-linker=/usr/lib32/ld-linux.so.2 -o diagnostic32");
                if (expect("diagnostic explicit-loader compile", compiled, 0) &&
                    expect("diagnostic explicit-loader host baseline", host("./diagnostic32"), 42))
                    expect("diagnostic explicit-loader sandbox baseline", sandbox("./diagnostic32"), 42);
            }
            return skip(require, "i386 coverage only: host cannot execute default-interpreter ELF (kernel/loader unavailable):\n" + runtime.output);
        }
        if (!expect("host i386 baseline", runtime, 42)) return 1;
        // After the capability probe, fixture failures are never capability skips.
        for (const auto& cmd : std::vector<std::string>{
                 "gcc -m32 -fPIC -shared library.c -o libmultilib.so",
                 "gcc -m32 dynamic.c -ldl -o dynamic32",
                 "gcc -m32 restrictions.c -o restrictions32"})
            if (!expect(cmd.c_str(), host(cmd), 0)) return 1;
        if (!expect("host i386 dlopen", host("./dynamic32"), 42)) return 1;
        ok = expect("sandbox default-interpreter i386 baseline", sandbox("./baseline32"), 42) && ok;
        ok = expect("sandbox i386 dlopen shared library", sandbox("./dynamic32"), 42) && ok;
        ok = restrictions("32") && ok;
        return ok ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
#endif
}
