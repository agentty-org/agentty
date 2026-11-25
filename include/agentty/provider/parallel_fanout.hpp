#pragma once
// Fan a leaked `multi_tool_use.parallel` call out into the calls it holds.
//
// OpenAI-family models batch tool calls internally as a call to a pseudo
// tool named `multi_tool_use.parallel` whose arguments are
//
//   {"tool_uses":[{"recipient_name":"functions.read","parameters":{...}}, ...]}
//
// Normally the host unpacks that into separate tool calls. Sometimes it
// reaches the client as-is: one call to a tool nobody advertised, which used
// to fail as "unknown tool" and cost the model a round trip to re-send each
// call by hand.
//
// ParallelFanout wraps a stream sink. A call named that way is held until
// it ends; any other call is held only until its first argument key is
// known (a few bytes), and kept only when that key is `tool_uses` (the same
// list leaked under a real tool's name). A held call's wrapped calls are
// then emitted as their own Start/Delta/End with id `<original>__<i>`.
// Every transport replays the full history each turn (no server-side call
// state), so the made-up ids round-trip like any other. A held call that
// doesn't decode, or a stream that ends mid-call, is forwarded unchanged
// so the existing unknown-tool / truncated-args / call_leak handling still
// reports it.

#include <memory>
#include <string_view>

#include "agentty/provider/provider.hpp"

namespace agentty::provider {

// True for `multi_tool_use.parallel` and its spellings (`functions.` prefix,
// case, the bare `multi_tool_use`).
[[nodiscard]] bool is_parallel_wrapper_name(std::string_view name) noexcept;

// Wraps a sink for one stream. sink() is the wrapped sink to hand the
// transport; finish() forwards any call still held when the transport
// returns (a stream that ended with no StreamFinished / StreamError).
class ParallelFanout {
public:
    explicit ParallelFanout(EventSink downstream);
    [[nodiscard]] EventSink sink() const;
    void finish() const;

private:
    struct State;
    std::shared_ptr<State> st_;
};

} // namespace agentty::provider
