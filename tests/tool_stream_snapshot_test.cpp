// tool_stream_snapshot_test — canonical ID-addressed append/snapshot semantics.

#include <chrono>
#include <string>
#include <utility>

#include "agtest.hpp"

#include "agentty/io/http.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_body_preview.hpp"   // task_status_row
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_helpers.hpp"        // tool_event_status

namespace A = agentty;
namespace D = agentty::app::detail;

static A::Model apply(A::Model m, A::msg::StreamMsg event) {
    m.now = agentty::app::kTestEpoch;   // the fold's time
    auto [next, cmd] = D::step(::agentty::app::detail::stream_update, std::move(m), std::move(event));
    (void)cmd;
    // `next` is a structured binding: no implicit move-on-return before C++23,
    // and Model's copy ctor is deleted.
    return std::move(next);
}

TEST_CASE("thinking clock: starts at the block, stops when the turn ends") {
    using namespace std::chrono_literals;
    auto at = [](A::Model m, A::msg::StreamMsg ev, std::chrono::milliseconds t) {
        m.now = agentty::app::kTestEpoch + t;
        auto [next, cmd] = D::step(::agentty::app::detail::stream_update, std::move(m), std::move(ev));
        (void)cmd;
        return std::move(next);
    };
    A::Model m;
    m.s.phase = A::phase::Streaming{};
    A::Message a; a.role = A::Role::Assistant;
    m.d.current.messages.push_back(std::move(a));
    // Block opens at 0s with no text; the summary arrives in one burst at 3s.
    m = at(std::move(m), A::StreamThinkingDelta{"", "", true}, 0ms);
    CHECK(m.d.current.messages.back().thinking_open());
    m = at(std::move(m), A::StreamThinkingDelta{"summary", ""}, 3000ms);
    m = at(std::move(m), A::StreamTextDelta{"answer"}, 3100ms);
    CHECK(!m.d.current.messages.back().thinking_open());
    CHECK(m.d.current.messages.back().reasoning_ms >= 3000);

    // A turn that errors mid-thought doesn't leave the clock running.
    A::Model m2;
    m2.s.phase = A::phase::Streaming{};
    A::Message b; b.role = A::Role::Assistant;
    m2.d.current.messages.push_back(std::move(b));
    m2 = at(std::move(m2), A::StreamThinkingDelta{"hmm", ""}, 0ms);
    A::StreamError err; err.message = "boom";
    m2 = at(std::move(m2), std::move(err), 2000ms);
    CHECK(!m2.d.current.messages.back().thinking_open());
    CHECK(m2.d.current.messages.back().reasoning_ms == 2000);
}

TEST_CASE("ACP: prose after a tool card continues in a new message, not the finished one") {
    A::Model m;
    m.d.selection.kind = A::provider::Kind::ExternalAcp;
    m.d.selection.acp_agent_id = "devin";
    m.s.phase = A::phase::Streaming{};
    A::Message a; a.role = A::Role::Assistant;
    m.d.current.messages.push_back(std::move(a));
    m = apply(std::move(m), A::StreamTextDelta{"Let me read it."});
    m = apply(std::move(m), A::StreamToolUseStart{A::ToolCallId{"t1"}, A::ToolName{"Read File"}});
    m = apply(std::move(m), A::StreamToolUseEnd{A::ToolCallId{"t1"}});
    for (const char* d : {"# agentty\n\n", "**An agent", " harness** for", " your terminal."})
        m = apply(std::move(m), A::StreamTextDelta{d});
    // A late update for the card above still finds it.
    m = apply(std::move(m), A::StreamObservedToolResult{A::ToolCallId{"t1"}, false, "file body"});
    const auto& msgs = m.d.current.messages;
    REQUIRE(msgs.size() == 2u);
    CHECK(msgs[0].text == "Let me read it.");
    CHECK(msgs[0].tool_calls.size() == 1u);
    CHECK(msgs[0].tool_calls[0].output() == "file body");
    CHECK(msgs[1].tool_calls.empty());
    const std::string body = msgs[1].text + msgs[1].streaming_text + msgs[1].pending_stream;
    CHECK(body == "# agentty\n\n**An agent harness** for your terminal.");
}

TEST_CASE("ACP: thinking after a tool card opens a new block below it") {
    A::Model m;
    m.d.selection.kind = A::provider::Kind::ExternalAcp;
    m.d.selection.acp_agent_id = "devin";
    m.s.phase = A::phase::Streaming{};
    A::Message a; a.role = A::Role::Assistant;
    m.d.current.messages.push_back(std::move(a));
    m = apply(std::move(m), A::StreamThinkingDelta{"plan first", {}});
    m = apply(std::move(m), A::StreamToolUseStart{A::ToolCallId{"t1"}, A::ToolName{"Listed ./"}});
    m = apply(std::move(m), A::StreamToolUseEnd{A::ToolCallId{"t1"}});
    m = apply(std::move(m), A::StreamThinkingDelta{"then this", {}});
    const auto& msgs = m.d.current.messages;
    REQUIRE(msgs.size() == 2u);
    CHECK(msgs[0].thinking.find("then this") == std::string::npos);
    CHECK(msgs[1].thinking.find("then this") != std::string::npos);
}

TEST_CASE("subagent: Esc settles its in-flight calls, and reads as cancelled, not failed") {
    A::ToolUse tc;
    tc.id = A::ToolCallId{"t"};
    tc.name = A::ToolName{"task"};
    A::ToolUse::Running r; r.started_at = agentty::app::kTestEpoch;
    tc.status = r;
    using C = A::ToolUse::Child;
    tc.children = {C{"a", "glob", {}, C::State::Done, "x", 5},
                   C{"b", "read", {}, C::State::Running, "", 0}};
    tc.settle_cancelled(agentty::app::kTestEpoch);
    CHECK(tc.cancelled_by_user());
    CHECK(tc.children[0].state == C::State::Done);
    CHECK(tc.children[1].state == C::State::Failed);
    CHECK(tc.children[1].output == A::ToolUse::kCancelledOutput);
    CHECK(A::ui::tool_event_status(tc) == maya::AgentEventStatus::Cancelled);
    // A real failure stays a failure.
    A::ToolUse f = tc;
    f.settle_failed(agentty::app::kTestEpoch, "[subagent failed: HTTP 529]");
    CHECK(!f.cancelled_by_user());
    CHECK(A::ui::tool_event_status(f) == maya::AgentEventStatus::Failed);
}

TEST_CASE("subagent status row: the clock leads, so a narrow card keeps it") {
    CHECK(A::ui::task_status_row("\xe2\x97\x86 coder \xc2\xb7 turn 7/80 \xc2\xb7 5 tools \xc2\xb7 1m 12s")
          == "\xe2\x96\xb0\xe2\x96\xb1\xe2\x96\xb1\xe2\x96\xb1\xe2\x96\xb1\xe2\x96\xb1\xe2\x96\xb1"
             "\xe2\x96\xb1\xe2\x96\xb1\xe2\x96\xb1  1m 12s \xc2\xb7 turn 7/80 \xc2\xb7 5 tools");
    CHECK(A::ui::task_status_row("not a status line").empty());
}

TEST_CASE("tool stream snapshot") {
    {
        auto decoded = A::http::test::decode_chunked({
            "4\r\nWi", "ki\r\n5;ext=yes\r\nped", "ia\r\n0\r\n", "\r\n"});
        check(decoded && *decoded == "Wikipedia",
              "incremental chunk decoder accepts valid extensions and splits");
        check(!A::http::test::decode_chunked({"1Z\r\nx\r\n0\r\n\r\n"}),
              "chunk decoder rejects non-hex size suffixes");
        check(!A::http::test::decode_chunked({"1\r\nxXX0\r\n\r\n"}),
              "chunk decoder rejects missing data CRLF");
        check(!A::http::test::decode_chunked({"5\r\nabc"}),
              "chunk decoder rejects truncated streams");
        auto upper = A::http::test::is_chunked({
            {"transfer-encoding", "Chunked"}});
        check(upper && *upper,
              "transfer coding token is parsed case-insensitively");
        check(!A::http::test::is_chunked({
                  {"transfer-encoding", "gzip, chunked"}}),
              "unsupported transfer coding stacks are rejected");
    }

    A::Model m;
    A::Message assistant;
    assistant.role = A::Role::Assistant;
    m.d.current.messages.push_back(std::move(assistant));
    A::phase::Active active;
    active.transient_retries = 3;
    // apply() folds at kTestEpoch, so "now" for the reducer is that.
    active.last_event_at = agentty::app::kTestEpoch - std::chrono::seconds{119};
    m.s.phase = A::phase::Streaming{std::move(active)};

    m = apply(std::move(m), A::StreamHeartbeat{.transport_only = true});
    auto* transport_ctx = A::active_ctx(m.s.phase);
    check(transport_ctx && transport_ctx->transport_activity,
          "transport heartbeat records proxy/socket activity");
    check(transport_ctx && transport_ctx->transient_retries == 3,
          "transport heartbeat does not erase retry history");
    check(transport_ctx
              && transport_ctx->last_event_at == agentty::app::kTestEpoch,
          "transport heartbeat refreshes the stall clock");

    m = apply(std::move(m), A::StreamBufferedWait{});
    auto* buffered_ctx = A::active_ctx(m.s.phase);
    check(buffered_ctx && buffered_ctx->transport_activity,
          "buffered gateway wait is tracked as transport activity");
    check(buffered_ctx && buffered_ctx->gateway_buffered,
          "buffered gateway wait is shown by the phase chip");
    check(m.s.status.find("buffering output") == std::string::npos,
          "buffered gateway wait never takes over the status row (hid ctx)");

    m = apply(std::move(m), A::StreamHeartbeat{});
    auto* model_ctx = A::active_ctx(m.s.phase);
    check(model_ctx && model_ctx->transient_retries == 0,
          "semantic model heartbeat resets transient retries");

    m = apply(std::move(m), A::StreamToolUseStart{
        A::ToolCallId{"call-a"}, A::ToolName{"edit"}});
    m = apply(std::move(m), A::StreamToolUseStart{
        A::ToolCallId{"call-b"}, A::ToolName{"read"}});

    m = apply(std::move(m), A::StreamToolUseSnapshot{
        A::ToolCallId{"call-a"}, R"({"path":"a.cpp","old_string":"x"})"});
    m = apply(std::move(m), A::StreamToolUseSnapshot{
        A::ToolCallId{"call-a"},
        R"({"path":"a.cpp","old_string":"x","new_string":"y"})"});
    m = apply(std::move(m), A::StreamToolUseSnapshot{
        A::ToolCallId{"call-b"}, R"({"path":"b.cpp"})"});

    const auto& calls_before_end = m.d.current.messages.back().tool_calls;
    check(calls_before_end.size() == 2, "both interleaved calls remain distinct");
    check(calls_before_end[0].args_streaming ==
              R"({"path":"a.cpp","old_string":"x","new_string":"y"})",
          "new ACP snapshot replaces rather than concatenates prior JSON");
    check(calls_before_end[1].args_streaming == R"({"path":"b.cpp"})",
          "snapshot is routed by tool call id, not the newest call");

    m = apply(std::move(m), A::StreamToolUseEnd{A::ToolCallId{"call-a"}});
    m = apply(std::move(m), A::StreamObservedToolResult{
        A::ToolCallId{"call-a"}, false, "external edit complete"});
    auto& calls = m.d.current.messages.back().tool_calls;
    check(calls[0].args_streaming.empty(), "ID-addressed end consumes target snapshot");
    check(calls[0].args.value("new_string", "") == "y",
          "final executable arguments equal the latest snapshot");
    check(std::holds_alternative<A::ToolUse::Done>(calls[0].status),
          "delegated ACP result settles the card without host execution");
    check(calls[0].output() == "external edit complete",
          "delegated ACP output is preserved");
    check(calls[1].args_streaming == R"({"path":"b.cpp"})",
          "ending one call does not close an interleaved call");

    m = apply(std::move(m), A::StreamToolUseStart{
        A::ToolCallId{"call-c"}, A::ToolName{"write"}});
    m = apply(std::move(m), A::StreamToolUseStart{
        A::ToolCallId{"call-d"}, A::ToolName{"shell"}});
    m = apply(std::move(m), A::StreamToolUseDelta{
        A::ToolCallId{"call-c"}, R"({"path":"c.cpp",")"});
    m = apply(std::move(m), A::StreamToolUseDelta{
        A::ToolCallId{"call-d"}, R"({"command":"true"})"});
    m = apply(std::move(m), A::StreamToolUseDelta{
        A::ToolCallId{"call-c"}, R"(content":"ok"})"});

    const auto& interleaved = m.d.current.messages.back().tool_calls;
    check(interleaved[2].args_streaming ==
              R"({"path":"c.cpp","content":"ok"})",
          "append deltas are assembled by call id across interleaving");
    check(interleaved[3].args_streaming == R"({"command":"true"})",
          "a sibling delta never appends to the newest call implicitly");
}
