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
#include <maya/render/renderer.hpp>
#include <maya/render/scrollback_replay.hpp>
#include <maya/style/theme.hpp>

#if defined(AGENTTY_USE_MIMALLOC)
#include <mimalloc.h>
#include <mimalloc-stats.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <tuple>
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
#if defined(AGENTTY_USE_MIMALLOC)
    mi_stats_t_decl(st);
    mi_stats_get(&st);
    std::printf("%-22s %5ld MB  (live %lld MB, page-committed %lld MB, committed %lld MB, pages %lld)\n",
                name, rss_mb(),
                (long long)((st.malloc_normal.current + st.malloc_huge.current) >> 20),
                (long long)(st.page_committed.current >> 20),
                (long long)(st.committed.current >> 20), (long long)st.pages.current);
#else
    std::printf("%-22s %5ld MB\n", name, rss_mb());
#endif
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
    agentty::tune_allocator();
    maya::host_detail::executor_scope pool;
    stage("start");

    std::optional<Thread> loaded;
    if (p.extension() == ".jsonl") {
        if (auto log = ThreadLog::open_path(p)) loaded = log->load_thread();
    } else if (auto r = persistence::load_thread_file(::agentty::IoAccess::grant(), p)) {
        loaded = std::move(*r);
    }
    if (!loaded) { std::fprintf(stderr, "load failed: %s\n", p.c_str()); return 2; }
    collect_everywhere();
    stage("parsed Thread only");
    std::printf("file %.1f MB, %zu messages\n",
                std::filesystem::file_size(p) / 1e6, loaded->messages.size());
    {
        std::size_t text = 0, think = 0, out = 0, args_json = 0, calls = 0, enc = 0;
        for (const auto& msg : loaded->messages) {
            text += msg.text.size();
            think += msg.thinking.size();
            enc += msg.reasoning_encrypted.size();
            for (const auto& b : msg.thinking_blocks) think += b.text.size() + b.signature.size();
            for (const auto& tc : msg.tool_calls) {
                ++calls;
                out += tc.output().size();
                args_json += tc.args.dump().size();
            }
        }
        std::printf("text %.1f MB, thinking %.1f MB, encrypted %.1f MB, tool output %.1f MB, "
                    "tool args %.1f MB (%zu calls)\n",
                    text / 1e6, think / 1e6, enc / 1e6, out / 1e6, args_json / 1e6, calls);
        std::printf("sizeof Message=%zu ToolUse=%zu json=%zu\n", sizeof(Message),
                    sizeof(ToolUse), sizeof(nlohmann::json));
    }

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
        src.max_rows  = std::getenv("ROWS") ? std::atoi(std::getenv("ROWS")) : lim.first;
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
    std::printf("  replay cache %.1f MB, view_cache settled=%zu, history chunks=%zu\n",
                maya::replay::cache_bytes() / 1e6, m.ui.view_cache.settled_count(),
                m.ui.history.empty() ? 0 : m.ui.history.front().chunks.size());
    if (std::getenv("SPLIT")) {
        maya::replay::clear_cache();
        collect_everywhere();
        stage("  - replay cache");
        m.ui.history.clear();
        collect_everywhere();
        stage("  - history chunks");
        m.ui.view_cache.clear();
        collect_everywhere();
        stage("  - view_cache");
#if defined(AGENTTY_USE_MIMALLOC)
        {
            struct Acc { std::map<std::size_t, std::pair<std::size_t, std::size_t>> by; };
            Acc acc;
            auto area_fn = [](const mi_heap_t*, const mi_heap_area_t* a, void* blk, std::size_t,
                              void* arg) -> bool {
                if (blk) return true;
                auto& e = static_cast<Acc*>(arg)->by[a->block_size];
                e.first += a->used * a->block_size;
                e.second += a->committed;
                return true;
            };
            mi_heap_visit_blocks(mi_heap_main(), false, area_fn, &acc);
            mi_heap_visit_abandoned_blocks(mi_heap_main(), false, area_fn, &acc);
            if (std::getenv("SAMPLE")) {
                auto blk_fn = [](const mi_heap_t*, const mi_heap_area_t* a, void* blk, std::size_t,
                                 void* arg) -> bool {
                    if (!blk) return true;
                    auto& n = *static_cast<int*>(arg);
                    if ((a->block_size == 3064 || a->block_size == 1784 || a->block_size == 376 || a->block_size == 760) && n < 16) {
                        ++n;
                        std::printf("    sample %zu B @%p:", a->block_size, blk);
                        const auto* w = static_cast<const std::uint64_t*>(blk);
                        for (int i = 0; i < 6; ++i) std::printf(" %016llx", (unsigned long long)w[i]);
                        std::printf("  |");
                        const auto* c = static_cast<const unsigned char*>(blk);
                        for (int i = 0; i < 48; ++i) std::putchar(c[i] >= 32 && c[i] < 127 ? c[i] : '.');
                        std::printf("|\n");
                    }
                    return true;
                };
                int n = 0;
                mi_heap_visit_blocks(mi_heap_main(), true, blk_fn, &n);
            }
            std::size_t used = 0, com = 0;
            std::vector<std::tuple<std::size_t, std::size_t, std::size_t>> rows;
            for (auto& [bs, uc] : acc.by) { used += uc.first; com += uc.second; rows.push_back({uc.first, uc.second, bs}); }
            std::sort(rows.rbegin(), rows.rend());
            std::printf("  heap areas: used %.1f MB of %.1f MB committed\n", used / 1e6, com / 1e6);
            for (std::size_t i = 0; i < rows.size() && i < 8; ++i)
                std::printf("    block %7zu B  used %6.2f MB  committed %6.2f MB\n", std::get<2>(rows[i]),
                            std::get<0>(rows[i]) / 1e6, std::get<1>(rows[i]) / 1e6);
        }
#endif
        maya::exec::parallel_for(std::thread::hardware_concurrency() * 4,
                                 [](std::size_t) { maya::render_detail::clear_component_cache(); });
        maya::render_detail::clear_component_cache();
        collect_everywhere();
        stage("  - all comp caches");
        m.ui.frozen = {};
        collect_everywhere();
        stage("  - frozen ledger");
        m.ui.history_runs = {};
        m.d.current.messages.clear();
        m.d.current.messages.shrink_to_fit();
        collect_everywhere();
        stage("  - messages");
        return 0;
    }
    (void)replay(width - 20);
    collect_everywhere();
    stage("after resize replay");
    return 0;
}
