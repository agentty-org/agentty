// agentty::question — see the header.

#include "agentty/domain/question.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

namespace agentty::question {

namespace {

[[nodiscard]] std::string trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n')) s.remove_suffix(1);
    return std::string{s};
}

[[nodiscard]] std::string str(const nlohmann::json& o, const char* key) {
    if (!o.is_object()) return {};
    const auto it = o.find(key);
    return it != o.end() && it->is_string() ? trimmed(it->get_ref<const std::string&>())
                                            : std::string{};
}

// The next question still waiting, after `from`, wrapping. nullopt: none left.
[[nodiscard]] std::optional<std::size_t> next_open(const State& s, std::size_t from) {
    for (std::size_t k = 1; k <= s.questions.size(); ++k) {
        const std::size_t i = (from + k) % s.questions.size();
        if (!s.answers[i].answered) return i;
    }
    return std::nullopt;
}

[[nodiscard]] Step advance(State& s) {
    s.typing = false;
    const auto n = next_open(s, s.current);
    if (!n) return Step::Done;
    s.current = *n;
    s.cursor = 0;
    return Step::Stay;
}

}  // namespace

Parsed parse(const nlohmann::json& args) {
    Parsed out;
    if (!args.is_object()) {
        out.error = "arguments must be an object with a `questions` array";
        return out;
    }
    // A single question at the top level is the commonest slip; take it.
    nlohmann::json list;
    if (const auto it = args.find("questions"); it != args.end() && it->is_array())
        list = *it;
    else if (args.contains("question"))
        list = nlohmann::json::array({args});
    if (!list.is_array() || list.empty()) {
        out.error = "`questions` is required: an array of 1 to " +
                    std::to_string(kMaxQuestions) + " questions";
        return out;
    }
    if (list.size() > kMaxQuestions) {
        out.error = "at most " + std::to_string(kMaxQuestions) +
                    " questions per call; ask the most important ones";
        return out;
    }
    for (const auto& q : list) {
        Question parsed;
        parsed.question = str(q, "question");
        if (parsed.question.empty()) {
            out.error = "every question needs a non-empty `question`";
            return out;
        }
        parsed.header = str(q, "header");
        if (q.is_object()) {
            if (const auto m = q.find("multiple"); m != q.end() && m->is_boolean())
                parsed.multiple = m->get<bool>();
            if (const auto os = q.find("options"); os != q.end() && os->is_array()) {
                for (const auto& o : *os) {
                    Option opt;
                    if (o.is_string()) opt.label = trimmed(o.get_ref<const std::string&>());
                    else {
                        opt.label = str(o, "label");
                        opt.description = str(o, "description");
                    }
                    if (!opt.label.empty()) parsed.options.push_back(std::move(opt));
                }
            }
        }
        if (parsed.options.size() > kMaxOptions) {
            out.error = "at most " + std::to_string(kMaxOptions) +
                        " options per question; the user can always type their own answer";
            return out;
        }
        out.questions.push_back(std::move(parsed));
    }
    return out;
}

State start(std::vector<Question> qs) {
    State s;
    s.answers.resize(qs.size());
    for (std::size_t i = 0; i < qs.size(); ++i) s.answers[i].chosen.assign(qs[i].options.size(), false);
    s.questions = std::move(qs);
    // No options at all: the only way to answer is to type.
    if (!s.questions.empty() && s.questions[0].options.empty()) s.typing = true;
    return s;
}

void move(State& s, int delta) {
    if (s.questions.empty()) return;
    const auto n = static_cast<long>(s.rows());
    long c = static_cast<long>(s.cursor) + delta;
    c = ((c % n) + n) % n;
    s.cursor = static_cast<std::size_t>(c);
    s.typing = false;
}

void toggle(State& s) {
    if (s.questions.empty()) return;
    if (s.on_text_row()) { s.typing = true; return; }
    auto& chosen = s.a().chosen;
    if (s.q().multiple) {
        chosen[s.cursor] = !chosen[s.cursor];
    } else {
        std::ranges::fill(chosen, false);
        chosen[s.cursor] = true;
        s.a().typed.clear();
    }
}

Step choose(State& s) {
    if (s.questions.empty()) return Step::Done;
    auto& a = s.a();
    if (s.on_text_row()) {
        if (!s.typing) { s.typing = true; return Step::Stay; }
        if (trimmed(a.typed).empty()) return Step::Stay;   // nothing to send yet
        if (!s.q().multiple) std::ranges::fill(a.chosen, false);
        a.answered = true;
        return advance(s);
    }
    if (s.q().multiple) {
        // Enter sends what is ticked; with nothing ticked, the row under the
        // cursor is the answer.
        if (std::ranges::none_of(a.chosen, [](bool b) { return b; }) && trimmed(a.typed).empty())
            a.chosen[s.cursor] = true;
    } else {
        std::ranges::fill(a.chosen, false);
        a.chosen[s.cursor] = true;
        a.typed.clear();
    }
    a.answered = true;
    return advance(s);
}

Step pick_number(State& s, int n) {
    if (s.questions.empty() || n < 1 || static_cast<std::size_t>(n) > s.q().options.size())
        return Step::Stay;
    s.cursor = static_cast<std::size_t>(n - 1);
    s.typing = false;
    if (s.q().multiple) { toggle(s); return Step::Stay; }
    return choose(s);
}

void type(State& s, std::string_view utf8) {
    if (s.questions.empty()) return;
    s.cursor = s.q().options.size();
    s.typing = true;
    auto& t = s.a().typed;
    if (t.size() + utf8.size() <= 2000) t += utf8;
}

void backspace(State& s) {
    if (s.questions.empty() || !s.typing) return;
    auto& t = s.a().typed;
    while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80) t.pop_back();
    if (!t.empty()) t.pop_back();
}

bool escape(State& s) {
    if (s.typing && !s.questions.empty() && !s.q().options.empty()) {
        s.typing = false;
        return false;
    }
    return true;
}

void back(State& s) {
    if (s.current == 0) return;
    --s.current;
    s.cursor = 0;
    s.typing = s.q().options.empty();
}

std::string answered(const State& s) {
    const bool many = s.questions.size() > 1;
    std::string out = many ? "The user answered your questions:\n"
                           : "The user answered your question:\n";
    for (std::size_t i = 0; i < s.questions.size(); ++i) {
        const auto& q = s.questions[i];
        const auto& a = s.answers[i];
        out += many ? std::to_string(i + 1) + ". " : std::string{};
        out += q.question + "\n";
        bool any = false;
        for (std::size_t k = 0; k < q.options.size(); ++k)
            if (k < a.chosen.size() && a.chosen[k]) {
                out += "   \xe2\x86\x92 " + q.options[k].label + "\n";
                any = true;
            }
        if (const auto t = trimmed(a.typed); !t.empty()) {
            out += "   \xe2\x86\x92 (in their own words) " + t + "\n";
            any = true;
        }
        if (!any) out += "   \xe2\x86\x92 (no answer)\n";
    }
    return out;
}

std::string dismissed() {
    return "The user dismissed the question without answering. Do not ask it again "
           "right away: carry on with your best judgement and say what you assumed, "
           "or ask in plain text in your reply.";
}

std::string unavailable() {
    return "No one can answer questions in this session. Carry on with your best "
           "judgement and say what you assumed.";
}

}  // namespace agentty::question
