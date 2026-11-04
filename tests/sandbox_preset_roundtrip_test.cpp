// Every sandbox preset must survive a RESTART.
//
// Reported: "I select Airgapped and restart and it shows Custom." The pane's
// Preset row is derived (detect_posture compares the config against what each
// preset would produce), so a preset that does not round-trip through the
// settings file comes back as Custom -- and worse, the walls the user chose are
// not the walls that load.
//
// This drives the real path a restart takes: apply the preset, save to an
// on-disk settings file, load it back the way main() does, and ask the pane's
// own question. Plus the field-by-field diff, so a failure names the field
// that did not survive instead of just saying "Custom".
#include "agtest.hpp"
#include "agtest_cap.hpp"

#include "agentty/domain/sandbox_config.hpp"
#include "agentty/io/persistence.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
namespace sb = agentty::sandbox_cfg;
namespace persistence = agentty::persistence;

namespace {

struct TmpHome {
    fs::path dir;
    std::string old_home, old_agentty;
    bool had_home = false, had_agentty = false;
    TmpHome() {
        char tmpl[] = "/tmp/agentty_preset_rt_XXXXXX";
        dir = ::mkdtemp(tmpl);
        if (const char* h = ::getenv("HOME"))         { old_home = h; had_home = true; }
        if (const char* a = ::getenv("AGENTTY_HOME")) { old_agentty = a; had_agentty = true; }
        ::setenv("HOME", dir.c_str(), 1);
        ::setenv("AGENTTY_HOME", dir.c_str(), 1);
    }
    ~TmpHome() {
        if (had_home) ::setenv("HOME", old_home.c_str(), 1); else ::unsetenv("HOME");
        if (had_agentty) ::setenv("AGENTTY_HOME", old_agentty.c_str(), 1); else ::unsetenv("AGENTTY_HOME");
        fs::remove_all(dir);
    }
};

const char* name(sb::Posture p) {
    switch (p) {
        case sb::Posture::Permissive: return "permissive";
        case sb::Posture::Balanced:   return "balanced";
        case sb::Posture::Hardened:   return "hardened";
        case sb::Posture::Airgapped:  return "airgapped";
        case sb::Posture::Custom:     return "custom";
    }
    return "?";
}

// Which fields differ, by name -- so a failure says what was lost.
std::string diff(const sb::Config& a, const sb::Config& b) {
    std::string d;
    auto f = [&](const char* n, bool same) { if (!same) d += std::string{" "} + n; };
    f("backend", a.backend == b.backend);
    f("fs_scope", a.fs_scope == b.fs_scope);
    f("read_paths", a.read_paths == b.read_paths);
    f("write_paths", a.write_paths == b.write_paths);
    f("deny_paths", a.deny_paths == b.deny_paths);
    f("net_mode", a.net_mode == b.net_mode);
    f("allow_ports", a.allow_ports == b.allow_ports);
    f("allow_hosts", a.allow_hosts == b.allow_hosts);
    f("syscall_mode", a.syscall_mode == b.syscall_mode);
    f("wx_protect", a.wx_protect == b.wx_protect);
    f("memory_mb", a.memory_mb == b.memory_mb);
    if (a.memory_mb != b.memory_mb)
        d += "(" + std::to_string(a.memory_mb) + "->" + std::to_string(b.memory_mb) + ")";
    f("max_procs", a.max_procs == b.max_procs);
    f("cpu_percent", a.cpu_percent == b.cpu_percent);
    f("max_open_files", a.max_open_files == b.max_open_files);
    f("wall_clock_secs", a.wall_clock_secs == b.wall_clock_secs);
    f("cpu_secs", a.cpu_secs == b.cpu_secs);
    f("tmp_mb", a.tmp_mb == b.tmp_mb);
    f("scope_ipc", a.scope_ipc == b.scope_ipc);
    f("close_inherited_fds", a.close_inherited_fds == b.close_inherited_fds);
    f("fake_hostname", a.fake_hostname == b.fake_hostname);
    f("mask_scan_depth", a.mask_scan_depth == b.mask_scan_depth);
    f("handoff", a.handoff == b.handoff);
    f("configured", a.configured == b.configured);
    if (d.empty() && !(a == b)) d = " <a field this diff does not list -- add it>";
    return d;
}

sb::Config reload(const sb::Config& cfg) {
    auto s = persistence::load_settings(::agentty::IoAccess::grant());
    s.sandbox = cfg;
    persistence::save_settings(::agentty::IoAccess::grant(), agtest::cap(), s);
    return persistence::load_settings(::agentty::IoAccess::grant()).sandbox;
}

constexpr sb::Posture kPresets[] = {sb::Posture::Permissive, sb::Posture::Balanced,
                                    sb::Posture::Hardened, sb::Posture::Airgapped};

}  // namespace

TEST_CASE("sandbox presets: each one is still itself after a restart") {
    TmpHome home;
    for (auto p : kPresets) {
        CAPTURE(name(p));
        const auto chosen = sb::apply_posture(sb::Config{}, p);
        REQUIRE(sb::detect_posture(chosen) == p);   // sanity: detect knows it
        const auto back = reload(chosen);
        INFO("lost on reload:" << diff(chosen, back));
        CHECK(back == chosen);
        CHECK(sb::detect_posture(back) == p);
    }
}

TEST_CASE("sandbox presets: switching between them survives a restart") {
    // The reported sequence is a SWITCH, not a fresh pick: the user was on
    // one preset, chose another, and restarted. Every ordered pair, so a
    // field one preset sets and the next one forgets to reset is caught.
    TmpHome home;
    for (auto from : kPresets) {
        for (auto to : kPresets) {
            CAPTURE(name(from));
            CAPTURE(name(to));
            const auto start = reload(sb::apply_posture(sb::Config{}, from));
            const auto chosen = sb::apply_posture(start, to);
            CHECK(sb::detect_posture(chosen) == to);
            const auto back = reload(chosen);
            INFO("lost on reload:" << diff(chosen, back));
            CHECK(sb::detect_posture(back) == to);
        }
    }
}

TEST_CASE("sandbox settings: every numeric field keeps the value it was saved with") {
    // The root cause, pinned below the presets: the loader range-checked a
    // 64-bit unsigned field by casting its max to long long (-1), so EVERY
    // positive memory_mb / tmp_mb reloaded as the default. Each numeric
    // field, at a value no preset uses, plus the extremes of each width.
    TmpHome home;
    sb::Config c = sb::apply_posture(sb::Config{}, sb::Posture::Balanced);
    c.memory_mb = 3072;
    c.tmp_mb = 1024;
    c.max_procs = 777;
    c.cpu_percent = 250;
    c.max_open_files = 2048;
    c.wall_clock_secs = 900;
    c.cpu_secs = 600;
    c.mask_scan_depth = 5;
    auto back = reload(c);
    INFO("lost on reload:" << diff(c, back));
    CHECK(back == c);

    // the width boundaries: still accepted at max, rejected (default) past it.
    c.memory_mb = 1ull << 40;   // well past 32 bits
    c.max_procs = 0xFFFFFFFFu;
    back = reload(c);
    CHECK(back.memory_mb == c.memory_mb);
    CHECK(back.max_procs == c.max_procs);
}

TEST_CASE("sandbox presets: the user's own lists do not knock a preset to Custom") {
    // Paths, ports and hosts are deliberately NOT part of a posture (see
    // apply_posture). A user with an extra read path or an allowlist who
    // picks Airgapped is on Airgapped.
    TmpHome home;
    for (auto p : kPresets) {
        CAPTURE(name(p));
        auto cfg = sb::Config{};
        cfg.read_paths = {"/opt/sdk"};
        cfg.write_paths = {"/tmp/out"};
        cfg.deny_paths = {"~/.cargo/credentials"};
        cfg.allow_hosts = {"github.com"};
        cfg.allow_ports = {443, 8443};
        const auto chosen = sb::apply_posture(cfg, p);
        CHECK(sb::detect_posture(chosen) == p);
        CHECK(sb::detect_posture(reload(chosen)) == p);
    }
}
