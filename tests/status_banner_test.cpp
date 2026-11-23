// status_banner_test — the status bar's toast band describes the present.
//
// Three ways it used to lie, each pinned here:
//   * a retry line outlived its turn (the banner stuck on OpenCode Go);
//   * "retrying in 21s" froze for 21 s, then pointed at the past;
//   * a rate-limit retry painted calm blue ("rate_limit" missed the view's
//     "rate limit" prefix).
// And the one layout change: the band no longer blanks the gauges.
#include <chrono>
#include <string>

#include "agtest.hpp"

#include <maya/core/anim_clock.hpp>
#include <maya/print.hpp>

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/view/status_bar/phase_chip.hpp"
#include "agentty/runtime/view/status_bar/status_banner.hpp"
#include "agentty/runtime/view/status_bar/status_bar.hpp"

namespace A = agentty;
using Kind = maya::StatusBanner::Kind;

namespace {

A::Model with_status(std::string s, bool streaming) {
    A::Model m;
    if (streaming) m.s.phase = A::phase::Streaming{A::phase::Active{}};
    m.s.status = std::move(s);
    m.s.status_until = {};   // no expiry: only the rule under test can hide it
    return m;
}

}  // namespace

TEST_CASE("status banner: one definition of a retry line") {
    CHECK(A::status_is_retry("retrying (upstream cut off)\xe2\x80\xa6"));
    CHECK(A::status_is_retry("transient \xe2\x80\x94 retrying in 5s (attempt 2/6)\xe2\x80\xa6"));
    CHECK(A::status_is_retry("rate limited \xe2\x80\x94 retrying in 90s (attempt 1, until Esc)\xe2\x80\xa6"));
    CHECK(A::status_is_retry("compacting \xe2\x80\x94 retrying in 4s\xe2\x80\xa6"));
    CHECK(A::status_is_retry(
        "reasoning used the whole budget \xc2\xb7 retrying with less\xe2\x80\xa6"));
    // The settings toast names the feature, it is not a retry.
    CHECK(!A::status_is_retry("keep retrying: on \xc2\xb7 temporary errors retry until Esc"));
    CHECK(!A::status_is_retry("context compacted"));
}

TEST_CASE("status banner: a retry line does not outlive its turn") {
    const auto live = A::ui::status_banner_config(
        with_status("retrying (upstream cut off)\xe2\x80\xa6", /*streaming=*/true));
    CHECK(!live.text.empty());
    CHECK(live.kind == Kind::Warn);

    // Same text, turn over: history, whatever its TTL says.
    const auto done = A::ui::status_banner_config(
        with_status("retrying (upstream cut off)\xe2\x80\xa6", /*streaming=*/false));
    CHECK(done.text.empty());

    // Everything else keeps its TTL semantics when idle.
    const auto other = A::ui::status_banner_config(
        with_status("context compacted", /*streaming=*/false));
    CHECK(other.text == "context compacted");
}

TEST_CASE("status banner: the retry wait counts down, then says now") {
    auto m = with_status(
        "transient \xe2\x80\x94 retrying in 21s (attempt 2/6)\xe2\x80\xa6", true);
    auto* a = A::active_ctx(m.s.phase);
    REQUIRE(a != nullptr);
    a->retry    = A::retry::Scheduled{};
    a->retry_at = maya::anim_now() + std::chrono::seconds{7};
    const auto waiting = A::ui::status_banner_config(m);
    CHECK(waiting.text.find("retrying in 7s (attempt 2/6)") != std::string::npos);

    // The retry fired (Scheduled → Fresh): the next attempt is connecting.
    a->retry    = A::retry::Fresh{};
    a->retry_at = {};
    const auto firing = A::ui::status_banner_config(m);
    CHECK(firing.text.find("retrying now (attempt 2/6)") != std::string::npos);
    CHECK(firing.text.find("21s") == std::string::npos);
}

TEST_CASE("status banner: severity reads off what happened") {
    auto kind = [](const char* s) {
        return A::ui::status_banner_config(with_status(s, true)).kind;
    };
    CHECK(kind("error: HTTP 500") == Kind::Error);
    CHECK(kind("giving up after 6 failed attempts with no model output") == Kind::Error);
    CHECK(kind("could not write mcp.json") == Kind::Error);
    CHECK(kind("rewind failed: dirty tree") == Kind::Error);
    CHECK(kind("rate limited \xe2\x80\x94 retrying in 30s (attempt 1/6)\xe2\x80\xa6") == Kind::Warn);
    CHECK(kind("auth expired \xe2\x80\x94 refreshing token\xe2\x80\xa6") == Kind::Warn);
    CHECK(kind("cannot rewind while the agent is working") == Kind::Warn);
    CHECK(kind("copied to clipboard") == Kind::Info);
}

TEST_CASE("status bar: the phase chip counts down a scheduled retry") {
    auto m = with_status("", /*streaming=*/true);
    auto* a = A::active_ctx(m.s.phase);
    REQUIRE(a != nullptr);
    a->retry    = A::retry::Scheduled{};
    a->retry_at = maya::anim_now() + std::chrono::seconds{12};
    auto chip = A::ui::phase_chip_config(m);
    CHECK(chip.verb == "retry 12s");
    CHECK(chip.elapsed_secs < 0.0f);   // one clock, not two

    a->retry_at = maya::anim_now() + std::chrono::seconds{150};
    chip = A::ui::phase_chip_config(m);
    CHECK(chip.verb == "retry 2m30");
    CHECK(chip.verb.size() <= 10);

    a->retry_at = {};                  // no deadline known: the plain verb
    CHECK(A::ui::phase_chip_config(m).verb == "retrying");
}

TEST_CASE("status bar: a toast keeps the context and cache gauges") {
    maya::StatusBar::Config c;
    c.context.used = 50'000;
    c.context.max  = 200'000;
    c.cache.state  = maya::CacheGauge::State::Hit;
    c.cache.pct    = 93;
    c.status_banner.text = "copied to clipboard";
    const std::string wide = maya::render_to_string(maya::StatusBar{c}.build(), 120);
    CHECK(wide.find("copied to clipboard") != std::string::npos);
    CHECK(wide.find("CTX") != std::string::npos);
    CHECK(wide.find("CACHE") != std::string::npos);
    CHECK(wide.find("93%") != std::string::npos);

    // Too narrow for both: the message wins, the gauges give way.
    const std::string narrow = maya::render_to_string(maya::StatusBar{c}.build(), 34);
    CHECK(narrow.find("copied") != std::string::npos);
    CHECK(narrow.find("CACHE") == std::string::npos);
}
