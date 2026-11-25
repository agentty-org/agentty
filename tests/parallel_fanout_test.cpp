// ParallelFanout (provider/parallel_fanout.hpp): a leaked
// `multi_tool_use.parallel` call reaches the loops as the calls it holds.

#include "agtest.hpp"

#include "agentty/provider/parallel_fanout.hpp"
#include "agentty/runtime/msg.hpp"

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace agentty;
using provider::ParallelFanout;

namespace {

// What a loop would see, flattened: "S id name", "D id json", "E id", "F".
struct Tape {
    std::vector<std::string> ev;
    std::vector<std::string> started;
    // Every Start names a real id, once. A phantom (empty id) or a repeat is
    // a call the loops would run that the model never made.
    ~Tape() {
        for (std::size_t i = 0; i < started.size(); ++i) {
            CHECK_MESSAGE(!started[i].empty(), "phantom tool call (empty id)");
            for (std::size_t j = i + 1; j < started.size(); ++j)
                CHECK_MESSAGE(started[i] != started[j], "call started twice: " << started[i]);
        }
    }
    provider::EventSink sink() {
        return [this](Msg m) {
            auto* sm = std::get_if<msg::StreamMsg>(&m);
            if (!sm) return;
            std::visit([&](auto&& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::same_as<T, StreamToolUseStart>) {
                    started.push_back(e.id.value);
                    ev.push_back("S " + e.id.value + " " + e.name.value);
                }
                else if constexpr (std::same_as<T, StreamToolUseDelta>)
                    ev.push_back("D " + e.id.value + " " + e.partial_json);
                else if constexpr (std::same_as<T, StreamToolUseEnd>)
                    ev.push_back("E " + e.id.value);
                else if constexpr (std::same_as<T, StreamFinished>)
                    ev.push_back("F");
                else if constexpr (std::same_as<T, StreamTextDelta>)
                    ev.push_back("T " + e.text);
            }, *sm);
        };
    }
    // Args per call id, concatenated.
    std::string args(const std::string& id) const {
        std::string out;
        for (const auto& e : ev)
            if (e.starts_with("D " + id + " ")) out += e.substr(3 + id.size());
        return out;
    }
    bool has(const std::string& e) const {
        for (const auto& x : ev) if (x == e) return true;
        return false;
    }
};

void start(const provider::EventSink& s, const std::string& id, const std::string& name) {
    s(Msg{msg::StreamMsg{StreamToolUseStart{ToolCallId{id}, ToolName{name}}}});
}
void delta(const provider::EventSink& s, const std::string& id, const std::string& j) {
    s(Msg{msg::StreamMsg{StreamToolUseDelta{ToolCallId{id}, j}}});
}
void end(const provider::EventSink& s, const std::string& id) {
    s(Msg{msg::StreamMsg{StreamToolUseEnd{ToolCallId{id}}}});
}
void finish(const provider::EventSink& s) {
    s(Msg{msg::StreamMsg{StreamFinished{StopReason::ToolUse}}});
}

const std::string kTwo =
    R"({"tool_uses":[{"recipient_name":"functions.read","parameters":{"path":"a.go"}},)"
    R"({"recipient_name":"functions.grep","parameters":{"pattern":"x","path":"src"}}]})";

} // namespace

TEST_CASE("parallel fanout: the pseudo tool becomes the calls it holds") {
    Tape t;
    ParallelFanout fan{t.sink()};
    auto s = fan.sink();
    start(s, "c1", "multi_tool_use.parallel");
    // arrives in fragments, split mid-key
    for (std::size_t i = 0; i < kTwo.size(); i += 7) delta(s, "c1", kTwo.substr(i, 7));
    end(s, "c1");
    finish(s);
    fan.finish();

    CHECK_FALSE(t.has("S c1 multi_tool_use.parallel"));
    CHECK(t.has("S c1__0 read"));
    CHECK(t.has("S c1__1 grep"));
    CHECK(t.has("E c1__0"));
    CHECK(t.has("E c1__1"));
    const json want0 = {{"path", "a.go"}};
    CHECK(json::parse(t.args("c1__0")) == want0);
    CHECK(json::parse(t.args("c1__1")).value("pattern", "") == "x");
    CHECK(t.ev.back() == "F");
}

TEST_CASE("parallel fanout: a tool_uses list under a real tool's name fans out") {
    Tape t;
    ParallelFanout fan{t.sink()};
    auto s = fan.sink();
    start(s, "r1", "read");
    delta(s, "r1", "{\"tool");
    delta(s, "r1", kTwo.substr(6));
    end(s, "r1");
    fan.finish();
    CHECK_FALSE(t.has("S r1 read"));
    CHECK(t.has("S r1__0 read"));
    CHECK(t.has("S r1__1 grep"));
}

TEST_CASE("parallel fanout: ordinary calls pass through, early and intact") {
    Tape t;
    ParallelFanout fan{t.sink()};
    auto s = fan.sink();
    start(s, "a", "read");
    CHECK(t.ev.empty());   // held only while the first key is unknown
    delta(s, "a", "{\"pa");
    delta(s, "a", "th\":\"x.go");
    // released as soon as the key is known, before the call ends
    CHECK(t.has("S a read"));
    delta(s, "a", "\"}");
    end(s, "a");
    CHECK(t.args("a") == "{\"path\":\"x.go\"}");
    CHECK(t.has("E a"));

    // no arguments at all
    start(s, "b", "git_status");
    end(s, "b");
    CHECK(t.has("S b git_status"));
    CHECK(t.has("E b"));

    // a `tool_uses` key deeper in is not the wrapper
    start(s, "c", "write");
    delta(s, "c", R"({"path":"t.json","content":"{\"tool_uses\":[]}"})");
    end(s, "c");
    CHECK(t.has("S c write"));
    CHECK(t.args("c") == R"({"path":"t.json","content":"{\"tool_uses\":[]}"})");

    // order between calls is preserved
    std::vector<std::string> starts;
    for (const auto& e : t.ev) if (e.starts_with("S ")) starts.push_back(e);
    const std::vector<std::string> want_starts = {"S a read", "S b git_status", "S c write"};
    CHECK(starts == want_starts);
}

TEST_CASE("parallel fanout: undecodable or unfinished wrappers are forwarded as sent") {
    {
        Tape t;
        ParallelFanout fan{t.sink()};
        auto s = fan.sink();
        start(s, "u", "multi_tool_use.parallel");
        delta(s, "u", R"({"tool_uses":"nope"})");
        end(s, "u");
        // unknown-tool handling downstream reports it
        CHECK(t.has("S u multi_tool_use.parallel"));
        CHECK(t.has("E u"));
    }
    {
        Tape t;
        ParallelFanout fan{t.sink()};
        auto s = fan.sink();
        start(s, "v", "multi_tool_use.parallel");
        delta(s, "v", kTwo.substr(0, 30));
        finish(s);   // stream cut before End
        CHECK(t.has("S v multi_tool_use.parallel"));
        CHECK_FALSE(t.has("E v"));   // still open: the reducer sees it truncated
        CHECK(t.ev.back() == "F");
    }
    {
        Tape t;
        ParallelFanout fan{t.sink()};
        auto s = fan.sink();
        start(s, "w", "read");
        delta(s, "w", "{");
        fan.finish();   // transport returned without a terminal event
        CHECK(t.has("S w read"));
    }
}

TEST_CASE("parallel fanout: an external agent's stream passes through") {
    Tape t;
    ParallelFanout fan{t.sink()};
    auto s = fan.sink();
    start(s, "acp1", "read");
    s(Msg{msg::StreamMsg{StreamToolUseSnapshot{ToolCallId{"acp1"}, R"({"path":"a"})"}}});
    start(s, "acp2", "multi_tool_use.parallel");   // not ours to split
    end(s, "acp1");
    CHECK(t.ev.size() >= 3);
    CHECK(t.ev[0] == "S acp1 read");
    CHECK(t.has("S acp2 multi_tool_use.parallel"));
    CHECK(t.has("E acp1"));
}

TEST_CASE("parallel fanout: name spellings") {
    using provider::is_parallel_wrapper_name;
    CHECK(is_parallel_wrapper_name("multi_tool_use.parallel"));
    CHECK(is_parallel_wrapper_name("functions.multi_tool_use.parallel"));
    CHECK(is_parallel_wrapper_name(" Multi_Tool_Use.Parallel "));
    CHECK(is_parallel_wrapper_name("multi_tool_use"));
    CHECK_FALSE(is_parallel_wrapper_name("read"));
    CHECK_FALSE(is_parallel_wrapper_name("parallel"));
}
