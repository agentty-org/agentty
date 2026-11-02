// thread_mem_probe — RAM cost of opening a thread.
//
// Loads a thread file the way the TUI does when you pick it (load,
// rehydrate_frozen, scrollback replay on the worker pool) and prints the
// process RSS after each step, so memory per transcript byte is a number
// you can compare before and after a change.
//
//   ./build/agentty_standalone_tests thread_mem_probe ~/.agentty/threads/<id>.jsonl [width]
//
// No thread given: prints usage and succeeds (ctest runs it bare).

#include "agentty/io/persistence.hpp"
#include "agentty/io/thread_log.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/mem.hpp"
#include "agentty/runtime/model.hpp"

#include <maya/core/executor.hpp>
#include <maya/host/executor.hpp>
#include <maya/render/scrollback_replay.hpp>
#include <maya/style/theme.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>

using namespace agentty;

namespace {

long rss_mb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("VmRSS:", 0) == 0) return std::atol(line.c_str() + 6) / 1024;
    return 0;
}

// Same cleanup the app does after a replay: collect on every worker too,
// since mimalloc keeps freed pages per thread.
void collect_everywhere() {
    maya::exec::parallel_for(std::max(1u, std::thread::hardware_concurrency()) * 4,
                             [](std::size_t) { release_to_kernel(); });
    release_to_kernel();
}

void stage(const char* name) {
    std::printf("%-22s %5ld MB\n", name, rss_mb());
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("thread_mem_probe: no thread file given, nothing to do.\n"
                    "usage: %s <thread.jsonl|thread.json> [width]\n", argv[0]);
        return 0;
    }
    const std::filesystem::path p = argv[1];
    const int width = argc > 2 ? std::atoi(argv[2]) : 140;

    // The app renders replay blocks on a worker pool; do the same.
    maya::host_detail::executor_scope pool;
    stage("start");

    std::optional<Thread> loaded;
    if (p.extension() == ".jsonl") {
        if (auto log = ThreadLog::open_path(p)) loaded = log->load_thread();
    } else if (auto r = persistence::load_thread_file(::agentty::IoAccess::grant(), p)) {
        loaded = std::move(*r);
    }
    if (!loaded) { std::fprintf(stderr, "load failed: %s\n", p.c_str()); return 2; }
    std::printf("file %.1f MB, %zu messages\n",
                std::filesystem::file_size(p) / 1e6, loaded->messages.size());

    Model m;
    m.ui.term_cols = width;
    m.ui.term_rows = 50;
    m.d.current = std::move(*loaded);
    app::detail::rehydrate_frozen(m);
    collect_everywhere();
    stage("loaded");

    auto replay = [&](int w) {
        maya::ScrollbackReplay src;
        src.chunk = [&](std::size_t cur) { return app::detail::history_chunk(m, cur); };
        const auto lim = app::detail::scrollback_replay_limits(m);
        src.max_rows  = lim.first;
        src.max_bytes = lim.second;
        auto r = maya::replay::render_history(src, w, maya::theme::native);
        if (const char* dump = std::getenv("DUMP"))
            std::ofstream(dump, std::ios::binary) << r.bytes;
        return r;
    };
    {
        const auto r = replay(width);
        std::printf("replay %d blocks, %d rows, %.1f MB\n", r.blocks, r.rows, r.bytes.size() / 1e6);
    }
    collect_everywhere();
    stage("after open replay");
    (void)replay(width - 20);
    collect_everywhere();
    stage("after resize replay");
    return 0;
}
