#include "agentty/provider/parallel_fanout.hpp"

#include <cctype>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "agentty/runtime/msg.hpp"
#include "agentty/util/logx.hpp"

namespace agentty::provider {

namespace {

using nlohmann::json;

bool ieq(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i]))
            != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

std::string_view strip_functions(std::string_view n) noexcept {
    while (!n.empty() && (n.front() == ' ' || n.front() == '\t')) n.remove_prefix(1);
    while (!n.empty() && (n.back() == ' ' || n.back() == '\t')) n.remove_suffix(1);
    constexpr std::string_view p = "functions.";
    if (n.size() > p.size() && ieq(n.substr(0, p.size()), p)) n.remove_prefix(p.size());
    return n;
}

struct Held {
    std::string id;
    std::string name;
    std::string args;
    bool decided = false;   // false: still probing the first key
};

// What the first key of a streaming args object says. Probing holds a call
// only until its first key is known (a few bytes), so a normal call's card
// is not visibly delayed.
enum class Probe { NeedMore, Wrapper, Plain };
Probe probe_first_key(std::string_view s) noexcept {
    constexpr std::size_t kMaxProbe = 96;
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    std::size_t i = 0;
    while (i < s.size() && ws(s[i])) ++i;
    if (i == s.size()) return s.size() > kMaxProbe ? Probe::Plain : Probe::NeedMore;
    if (s[i] != '{') return Probe::Plain;
    ++i;
    while (i < s.size() && ws(s[i])) ++i;
    if (i == s.size()) return Probe::NeedMore;
    if (s[i] != '"') return Probe::Plain;
    const std::size_t k = ++i;
    while (i < s.size() && s[i] != '"' && s[i] != '\\') ++i;
    if (i == s.size()) return s.size() > kMaxProbe ? Probe::Plain : Probe::NeedMore;
    if (s[i] == '\\') return Probe::Plain;   // no wrapper key needs an escape
    const auto key = s.substr(k, i - k);
    return key == "tool_uses" ? Probe::Wrapper : Probe::Plain;
}

} // namespace

struct ParallelFanout::State {
    EventSink sink;
    std::vector<Held> held;   // a handful at most; ordered by start
    bool passthrough = false; // an external agent's stream: forward as is

    Held* find(const ToolCallId& id) {
        for (auto& h : held) if (h.id == id.value) return &h;
        return nullptr;
    }
    // Remove a held call and hand it back. By position, never by moving
    // first and matching after: a moved-from entry has an empty id, so it
    // stayed behind as a ghost that flush_open replayed as a phantom call.
    Held take(Held* h) {
        const auto i = static_cast<std::size_t>(h - held.data());
        Held out = std::move(held[i]);
        held.erase(held.begin() + static_cast<std::ptrdiff_t>(i));
        return out;
    }
    void emit(msg::StreamMsg m) { sink(Msg{std::move(m)}); }

    // Forward a held call as the model sent it.
    void replay(const Held& h, bool with_end, json extra = nullptr) {
        emit(StreamToolUseStart{ToolCallId{h.id}, ToolName{h.name}});
        if (!h.args.empty()) emit(StreamToolUseDelta{ToolCallId{h.id}, h.args});
        if (with_end) emit(StreamToolUseEnd{ToolCallId{h.id}, std::move(extra)});
    }

    // A probing call is decided: a plain one is released (Start + what was
    // buffered) and stops being held; a wrapper is kept until its End.
    void decide(Held& h) {
        if (h.decided) return;
        switch (probe_first_key(h.args)) {
            case Probe::NeedMore: return;
            case Probe::Wrapper:  h.decided = true; return;
            case Probe::Plain: {
                Held out = take(&h);
                replay(out, false);
                return;
            }
        }
    }

    // The wrapped calls, or empty when `args` isn't a usable tool_uses list.
    static std::vector<std::pair<std::string, std::string>> unpack(const std::string& args) {
        std::vector<std::pair<std::string, std::string>> out;
        auto j = json::parse(args, nullptr, false);
        if (!j.is_object()) return out;
        auto t = j.find("tool_uses");
        if (t == j.end() || !t->is_array() || t->empty()) return out;
        for (const auto& item : *t) {
            if (!item.is_object()) return {};
            auto r = item.find("recipient_name");
            if (r == item.end() || !r->is_string()) return {};
            const auto name = strip_functions(r->get_ref<const std::string&>());
            if (name.empty() || is_parallel_wrapper_name(name)) return {};
            json params = json::object();
            if (auto p = item.find("parameters"); p != item.end()) {
                if (p->is_object()) params = *p;
                else if (p->is_string()) {
                    params = json::parse(p->get_ref<const std::string&>(), nullptr, false);
                    if (!params.is_object()) return {};
                } else if (!p->is_null()) return {};
            }
            out.emplace_back(std::string{name}, params.dump());
        }
        return out;
    }

    void end(const Held& h, json extra) {
        auto calls = unpack(h.args);
        if (calls.empty()) {
            AGT_LOG(Model, Warn, "tool_calls.parallel_undecodable", "id={} args={}",
                    h.id, ::agentty::logx::body(h.args));
            replay(h, true, std::move(extra));
            return;
        }
        AGT_LOG(Model, Info, "tool_calls.parallel_fanout", "id={} calls={}", h.id,
                calls.size());
        for (std::size_t i = 0; i < calls.size(); ++i) {
            ToolCallId id{h.id + "__" + std::to_string(i)};
            emit(StreamToolUseStart{id, ToolName{calls[i].first}});
            emit(StreamToolUseDelta{id, std::move(calls[i].second)});
            emit(StreamToolUseEnd{id, i == 0 ? extra : json(nullptr)});
        }
    }

    // A stream ending with calls still open: let the reducer see them as cut
    // off, exactly as it would have without this adapter.
    void flush_open() {
        auto open = std::move(held);
        held.clear();
        for (const auto& h : open) replay(h, false);
    }

    void operator()(Msg m) {
        auto* sm = std::get_if<msg::StreamMsg>(&m);
        if (!sm) { sink(std::move(m)); return; }
        // An external agent runs its own tools and reports them by id. Its
        // calls are not ours to split: release anything held and stop
        // holding for the rest of the stream.
        if (std::holds_alternative<StreamObservedToolResult>(*sm)
            || std::holds_alternative<StreamToolUseSnapshot>(*sm)) {
            if (!passthrough) {
                passthrough = true;
                flush_open();
            }
        }
        if (passthrough) { sink(std::move(m)); return; }
        if (auto* s = std::get_if<StreamToolUseStart>(sm)) {
            // The pseudo tool is held whole; any other call only while its
            // first key is unknown (a `{"tool_uses":[...]}` under a real name).
            held.push_back(Held{s->id.value, s->name.value, {},
                                is_parallel_wrapper_name(s->name.value)});
            return;
        }
        if (!held.empty()) {
            if (auto* d = std::get_if<StreamToolUseDelta>(sm))
                if (auto* h = find(d->id)) {
                    h->args += d->partial_json;
                    decide(*h);
                    return;
                }
            if (auto* e = std::get_if<StreamToolUseEnd>(sm))
                if (auto* h = find(e->id)) {
                    Held done = take(h);
                    if (done.decided) end(done, std::move(e->extra_content));
                    else replay(done, true, std::move(e->extra_content));
                    return;
                }
            if (std::holds_alternative<StreamFinished>(*sm)
                || std::holds_alternative<StreamError>(*sm))
                flush_open();
        }
        sink(std::move(m));
    }
};

bool is_parallel_wrapper_name(std::string_view name) noexcept {
    const auto n = strip_functions(name);
    return ieq(n, "multi_tool_use.parallel") || ieq(n, "multi_tool_use");
}

ParallelFanout::ParallelFanout(EventSink downstream) : st_(std::make_shared<State>()) {
    st_->sink = std::move(downstream);
}

EventSink ParallelFanout::sink() const {
    return [st = st_](Msg m) { (*st)(std::move(m)); };
}

void ParallelFanout::finish() const { st_->flush_open(); }

} // namespace agentty::provider
