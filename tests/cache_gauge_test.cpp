// cache_gauge_test — the status bar tells a healthy cache from a broken one.
//
// The usage numbers have always been there (and on the perf log channel),
// but a ratio cannot tell an EXPECTED cold start from the one that matters.
// These cases pin the decision table in StreamState::note_cache_usage, then
// render the real status bar to show the gauge survives every width without
// costing the phase verb or the ctx gauge.

#include "agtest.hpp"

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/view/status_bar/status_bar.hpp"

#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>
#include <maya/widget/status_bar.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace agentty;
using V = StreamState::CacheVerdict;
using namespace std::chrono_literals;

namespace {

// One request through the state machine, the way the reducer drives it.
void request(StreamState& s, int input, int read, int write,
             std::chrono::seconds idle = 0s, const char* model = "opus") {
    s.cache_gap = idle;
    s.note_cache_usage(input, read, write, model);
}

std::string ascii_row(const maya::Element& root, int width) {
    maya::StylePool pool;
    maya::Canvas canvas(width, 8, &pool);
    canvas.clear();
    maya::render_tree(root, canvas, pool, maya::theme::native, true);
    std::string out;
    for (int y = 0; y <= canvas.max_content_row(); ++y) {
        std::string s;
        for (int x = 0; x < width; ++x) {
            const char32_t c = canvas.get(x, y).character;
            s.push_back(c && c < 128 ? static_cast<char>(c) : ' ');
        }
        if (s.find("CACHE") != std::string::npos || s.find("CTX") != std::string::npos)
            out = s;
    }
    return out;
}

}  // namespace

TEST_CASE("cache verdict: the first request warms, the next one hits") {
    StreamState s;
    request(s, 200, 0, 40'000);
    CHECK(s.cache_verdict == V::Warming);
    request(s, 900, 40'000, 0);
    CHECK(s.cache_verdict == V::Hit);
    CHECK(s.cache_reuse_pct == 99);   // 40k of the 40.2k the first request left warm
}

TEST_CASE("cache verdict: a big new tool output is not a miss") {
    // Prefix was 20k; this turn adds 60k fresh. 25% by ratio, but all of
    // the warm prefix was reused — healthy.
    StreamState s;
    request(s, 0, 0, 20'000);
    request(s, 60'000, 20'000, 0);
    CHECK(s.cache_verdict == V::Hit);
    CHECK(s.cache_reuse_pct == 100);
}

TEST_CASE("cache verdict: a warm prefix that comes back cold is a MISS") {
    StreamState s;
    request(s, 0, 0, 50'000);
    request(s, 0, 50'000, 500);
    request(s, 51'000, 0, 0, 20s);   // 20 s later, nothing read
    CHECK(s.cache_verdict == V::Miss);
}

TEST_CASE("cache verdict: cold after a long idle is EXPIRED, not a bug") {
    StreamState s;
    request(s, 0, 0, 50'000);
    request(s, 50'500, 0, 0, 12min);
    CHECK(s.cache_verdict == V::Expired);
}

TEST_CASE("cache verdict: expected cold starts warm instead of missing") {
    StreamState s;
    request(s, 0, 0, 50'000);
    // After a compaction the history is rewritten.
    s.cache_cold_expected = true;
    request(s, 0, 2'000, 8'000);
    CHECK(s.cache_verdict == V::Warming);
    // A different model has its own cache.
    request(s, 0, 0, 10'000, 5s, "sonnet");
    CHECK(s.cache_verdict == V::Warming);
    // A new thread starts from nothing.
    s.reset_usage_for_thread(0);
    request(s, 0, 0, 30'000);
    CHECK(s.cache_verdict == V::Warming);
}

TEST_CASE("cache verdict: providers that never say are n/a, small prompts silent") {
    StreamState s;
    request(s, 30'000, 0, 0);
    CHECK(s.cache_verdict == V::None);         // first request: maybe just cold
    request(s, 31'000, 0, 0, 5s);
    CHECK(s.cache_verdict == V::Unreported);   // a second silence: it never says

    StreamState tiny;
    request(tiny, 300, 0, 0);
    request(tiny, 400, 0, 0, 2s);
    CHECK(tiny.cache_verdict == V::None);      // nothing cacheable to report on
}

TEST_CASE("cache gauge: renders every verdict with the right words") {
    using S = maya::CacheGauge::State;
    auto paint = [](maya::CacheGauge::Config c) {
        return ascii_row(maya::CacheGauge{c}.build(), 60);
    };
    CHECK(paint({.state = S::Hit, .hit_pct = 94, .tokens = 115'000}).find("CACHE 94% 115k")
          != std::string::npos);
    CHECK(paint({.state = S::Warming, .tokens = 120'000}).find("CACHE warming +120k")
          != std::string::npos);
    CHECK(paint({.state = S::Expired, .idle_secs = 725}).find("CACHE expired idle 12m")
          != std::string::npos);
    CHECK(paint({.state = S::Miss, .tokens = 51'000}).find("CACHE MISS 51k cold")
          != std::string::npos);
    CHECK(paint({.state = S::Unreported}).find("CACHE n/a") != std::string::npos);
    CHECK(paint({.state = S::Hidden}).find("CACHE") == std::string::npos);
}

TEST_CASE("cache gauge: the status bar sheds it before phase or ctx") {
    Model m;
    m.s.phase       = phase::Streaming{};
    m.s.tokens_in   = 115'000;
    m.s.context_max = 1'000'000;
    m.s.cache_read_tokens = 108'000;
    m.s.cache_verdict     = V::Hit;
    m.s.cache_reuse_pct   = 94;

    auto bar = [&](int w) {
        return ascii_row(maya::StatusBar{ui::status_bar_config(m)}.build(), w);
    };
    const std::string wide = bar(160);
    INFO(wide);
    CHECK(wide.find("CACHE 94% 108k") != std::string::npos);
    CHECK(wide.find("CTX") != std::string::npos);

    for (int w : {120, 100, 80, 60}) {
        const std::string row = bar(w);
        CAPTURE(w);
        INFO(row);
        // The ctx gauge outranks the cache gauge at every width.
        if (row.find("CACHE") != std::string::npos)
            CHECK(row.find("CTX") != std::string::npos);
    }
    if (std::getenv("CACHE_CHIP_DUMP"))
        for (int w : {160, 120, 100, 80, 60})
            std::printf("%3d |%s|\n", w, bar(w).c_str());
}
