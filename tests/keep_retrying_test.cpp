// keep_retrying_test — Settings › Keep retrying on errors (#131), through
// the real reducer: a 503 that won't go away gives up after the normal
// budget when the setting is off, and keeps scheduling retries when it's on.
// Errors retrying can't fix stop either way.
#include <chrono>
#include <string>
#include <utility>

#include "agtest.hpp"

#include "agentty/provider/error_class.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"

namespace A = agentty;

namespace {

A::Model streaming(bool keep) {
    A::Model m;
    m.d.persisted.keep_retrying = keep;
    A::Message u;
    u.role = A::Role::User;
    u.text = "go";
    m.d.current.messages.push_back(std::move(u));
    A::Message a;
    a.role = A::Role::Assistant;
    m.d.current.messages.push_back(std::move(a));
    m.s.phase = A::phase::Streaming{A::phase::Active{}};
    return m;
}

// One failed attempt: the error lands, and if a retry was scheduled the
// stream relaunches (RetryStream) before the next error.
A::Model fail(A::Model m, std::string msg, int status) {
    A::StreamError e;
    e.message = std::move(msg);
    e.http_status = status;
    auto [next, cmd] = agentty::app::update(std::move(m), A::Msg{std::move(e)});
    (void)cmd;
    if (next.s.is_streaming()) {
        auto [again, c2] = agentty::app::update(std::move(next), A::Msg{A::RetryStream{}});
        (void)c2;
        return std::move(again);
    }
    return std::move(next);
}

}  // namespace

TEST_CASE("keep retrying: off gives up on a 503 that won't clear") {
    auto m = streaming(false);
    for (int i = 0; i < 12 && m.s.is_streaming(); ++i)
        m = fail(std::move(m), "HTTP 503: service unavailable", 503);
    CHECK(m.s.is_idle());
}

TEST_CASE("keep retrying: on keeps retrying a 503, and says so") {
    auto m = streaming(true);
    for (int i = 0; i < 20; ++i) {
        m = fail(std::move(m), "HTTP 503: service unavailable", 503);
        REQUIRE(m.s.is_streaming());
    }
    const auto* a = A::active_ctx(m.s.phase);
    REQUIRE(a != nullptr);
    CHECK(a->retry_count.find("until Esc") != std::string::npos);
    CHECK(a->retry_why == "HTTP 503: service unavailable");
}

TEST_CASE("retry banner: gone once the stream works, even with no StreamStarted") {
    // The OpenAI-compatible transport (OpenCode Go, OpenRouter, custom hosts)
    // never sends StreamStarted, which was the only thing that cleared the
    // banner: one retry left "retrying …" up for the rest of the session.
    auto m = streaming(false);
    m = fail(std::move(m), "HTTP 503: service unavailable", 503);
    REQUIRE(m.s.is_streaming());
    // Narrated in the activity row now, so the toast stays free.
    REQUIRE(!A::active_ctx(m.s.phase)->retry_why.empty());
    CHECK(!A::status_is_retry(m.s.status));
    m.s.status = "retrying (stale)";   // and a leftover toast still clears
    auto [next, cmd] = agentty::app::update(
        std::move(m), A::Msg{A::StreamTextDelta{"hello"}});
    (void)cmd;
    CHECK(next.s.status.empty());
    CHECK(A::active_ctx(next.s.phase)->retry_why.empty());
    // An unrelated toast is left alone.
    next.s.status = "context compacted";
    auto [after, c2] = agentty::app::update(
        std::move(next), A::Msg{A::StreamTextDelta{" there"}});
    (void)c2;
    CHECK(after.s.status == "context compacted");
}

TEST_CASE("keep retrying: on still stops on errors retrying can't fix") {
    auto m = streaming(true);
    m = fail(std::move(m), "HTTP 404: model not found", 404);
    CHECK(m.s.is_idle());
}

TEST_CASE("keep retrying: the wait levels off, it doesn't grow forever") {
    using namespace std::chrono_literals;
    namespace P = agentty::provider;
    for (int attempt = 6; attempt < 200; attempt += 37) {
        const auto t = P::persistent_backoff(P::ErrorClass::Transient, attempt);
        CHECK(t >= 48s);
        CHECK(t <= 72s);
        const auto r = P::persistent_backoff(P::ErrorClass::RateLimit, attempt);
        CHECK(r >= 72s);   // never shorter than the rate-limit ladder's 90 s step
        CHECK(r <= 108s);
    }
}
