// spawn_work_budget_test — the per-command setup does no work it doesn't need.
//
// Every sandboxed shell call builds a posture: it snapshots the workspace's
// host-trusted files (handoff gate), sweeps for secret files to mask, and
// compiles a claybin plan against the host's capabilities. Each of those was,
// at some point, redone from scratch on EVERY command -- a full directory
// walk, an lstat per entry, six forked probe children -- because nothing
// failed when it was. A latency number drifts with the machine; a COUNT of
// work does not. So this asserts counts:
//
//   unchanged tree  -> 0 directories re-read, 0 mask walks
//   a real change   -> noticed (a walk happens, the new secret is masked)
//
// The second half matters as much as the first: a cache that never walks is
// trivially "fast" and wrong.

#include <doctest/doctest.h>

#include "agentty/domain/sandbox_config.hpp"
#include "agentty/tool/util/handoff_gate.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace agentty;
namespace fs = std::filesystem;
namespace hg = tools::util::handoff;

namespace {

void write(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

// Both caches refuse to trust a directory modified in the last couple of
// seconds (the "racy" rule: a same-tick second change would be invisible).
// Age every directory past that so the test measures the steady state.
void age_tree(const fs::path& root) {
    const auto old = fs::file_time_type::clock::now() - std::chrono::hours(1);
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_directory(ec)) fs::last_write_time(it->path(), old, ec);
    fs::last_write_time(root, old, ec);
}

fs::path make_ws() {
#ifndef _WIN32
    const auto id = std::to_string(::getpid());
#else
    const auto id = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
    const auto ws = fs::temp_directory_path() / ("agentty_spawn_budget_" + id);
    fs::remove_all(ws);
    write(ws / ".git" / "config", "[core]\n\tbare = false\n");
    write(ws / ".git" / "hooks" / "pre-commit.sample", "#!/bin/sh\n");
    write(ws / ".vscode" / "settings.json", "{}\n");
    for (int d = 0; d < 6; ++d)
        for (int f = 0; f < 10; ++f)
            write(ws / "src" / ("m" + std::to_string(d)) / "sub" / ("f" + std::to_string(f) + ".cpp"),
                  "int x;\n");
    age_tree(ws);
    return ws;
}

}  // namespace

TEST_CASE("spawn budget: an unchanged workspace is not re-read") {
    const auto ws = make_ws();

    (void)hg::snapshot_trusted(ws.string());              // warm
    const auto before = hg::listings_built_for_test();
    for (int i = 0; i < 5; ++i) {
        const auto s = hg::snapshot_trusted(ws.string());
        CHECK(!s.entries.empty());
    }
    CHECK(hg::listings_built_for_test() == before);

    // ...and a real change is still seen: a new hook is re-listed and found.
    write(ws / ".git" / "hooks" / "post-checkout", "#!/bin/sh\n");
    const auto s = hg::snapshot_trusted(ws.string());
    CHECK(hg::listings_built_for_test() > before);
    bool found = false;
    for (const auto& e : s.entries) found = found || e.path.ends_with("post-checkout");
    CHECK(found);
    fs::remove_all(ws);
}

TEST_CASE("spawn budget: the secret mask is not re-swept when nothing changed") {
    const auto ws = make_ws();
    sandbox_cfg::Config cfg;
    cfg.configured = true;

    (void)sandbox_cfg::mask_paths(cfg, ws.string(), "");   // warm (arms the watch)
    const auto before = sandbox_cfg::mask_tree_walks_for_test();
    for (int i = 0; i < 5; ++i) (void)sandbox_cfg::mask_paths(cfg, ws.string(), "");
#if defined(__linux__)
    // The inotify watch answers "unchanged" with no walk at all.
    CHECK(sandbox_cfg::mask_tree_walks_for_test() == before);
#endif

    // A new secret file is masked on the very next call -- never stale.
    write(ws / "src" / "m2" / ".env", "TOKEN=x\n");
    const auto masks = sandbox_cfg::mask_paths(cfg, ws.string(), "");
    CHECK(std::ranges::any_of(masks, [](const std::string& m) { return m.ends_with("/m2/.env"); }));
    CHECK(sandbox_cfg::mask_tree_walks_for_test() > before);

    // A renamed directory (not a new file) is also a change.
    fs::rename(ws / "src" / "m3", ws / "src" / "m3_renamed");
    write(ws / "src" / "m3_renamed" / "sub" / "id_rsa", "k\n");
    const auto masks2 = sandbox_cfg::mask_paths(cfg, ws.string(), "");
    CHECK(std::ranges::any_of(masks2, [](const std::string& m) { return m.ends_with("/m3_renamed/sub/id_rsa"); }));
    fs::remove_all(ws);
}
