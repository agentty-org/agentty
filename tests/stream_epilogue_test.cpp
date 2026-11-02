// stream_epilogue_test — how a stream that ends is reported.
//
// A dialect that always sends a terminal event (Anthropic message_stop,
// Responses response.completed) and then closes WITHOUT one was cut off in
// transit. That has to come out as a retryable error, not a clean turn that
// silently stops half way.

#include "agentty/provider/error_class.hpp"
#include "agentty/provider/stream_epilogue.hpp"

#include "agtest.hpp"

#include <variant>
#include <vector>

using namespace agentty;
namespace P = agentty::provider;

namespace {

struct Run {
    std::vector<Msg> got;
    P::StreamResult  result;
};

Run finish(bool terminated, bool expects_terminal) {
    Run r;
    bool term = terminated;
    const P::EventSink sink = [&](Msg m) { r.got.push_back(std::move(m)); };
    r.result = P::finish_stream({
        .terminated  = term,
        .sink        = sink,
        .result_ok   = true,
        .http_status = 200,
        .cancel      = nullptr,
        .stop        = StopReason::EndTurn,
        .http_error_message      = [] { return std::string{}; },
        .transport_error_message = [] { return std::string{}; },
        .expects_terminal_event  = expects_terminal,
    });
    return r;
}

const StreamError* error_in(const std::vector<Msg>& v) {
    for (const auto& m : v)
        if (const auto* s = std::get_if<msg::StreamMsg>(&m))
            if (const auto* e = std::get_if<StreamError>(s)) return e;
    return nullptr;
}

bool finished_in(const std::vector<Msg>& v) {
    for (const auto& m : v)
        if (const auto* s = std::get_if<msg::StreamMsg>(&m))
            if (std::holds_alternative<StreamFinished>(*s)) return true;
    return false;
}

}  // namespace

TEST_CASE("epilogue: a cut-off stream is an error that gets retried") {
    const Run r = finish(/*terminated=*/false, /*expects_terminal=*/true);
    const StreamError* e = error_in(r.got);
    REQUIRE(e != nullptr);
    CHECK(!finished_in(r.got), "not reported as a clean end");
    CHECK(P::classify_stream_error(e->message, e->http_status) == P::ErrorClass::Transient,
          "a connection that closed early is worth one more try");
}

TEST_CASE("epilogue: the terminal event already ended the turn") {
    const Run r = finish(/*terminated=*/true, /*expects_terminal=*/true);
    CHECK(r.got.empty(), "nothing more is emitted");
    CHECK(error_in(r.got) == nullptr);
}

TEST_CASE("epilogue: a dialect where a plain close is normal still ends cleanly") {
    const Run r = finish(/*terminated=*/false, /*expects_terminal=*/false);
    CHECK(error_in(r.got) == nullptr);
    CHECK(finished_in(r.got));
}
