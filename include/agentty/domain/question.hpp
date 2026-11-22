#pragma once
// agentty::question — the `question` tool's data, as pure values.
//
// The model asks the user one or more multiple-choice questions and waits
// for the answers (OpenCode's `question`, Claude Code's AskUserQuestion). This
// header is everything about that exchange that is not UI and not I/O: what a
// valid call looks like, the picker's state, what each key does to it, and the
// text the model gets back. The tool layer advertises the schema, the runtime
// shows the card and routes keys here; both read this one definition.
//
// Free text is always available (the last row of every question), so a model
// never has to guess every answer and the user is never forced into a choice
// they would not make.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace agentty::question {

// Bounds on one call. A wall of questions is a form, not a clarification.
inline constexpr std::size_t kMaxQuestions = 4;
inline constexpr std::size_t kMaxOptions   = 8;

struct Option {
    std::string label;
    std::string description;   // may be empty
};

struct Question {
    std::string question;
    std::string header;        // short label, may be empty
    std::vector<Option> options;
    bool multiple = false;     // more than one option may be chosen
};

// Parse a call's arguments. The error is phrased for the model (it is what
// the tool returns when the call is malformed).
struct Parsed {
    std::vector<Question> questions;
    std::string error;         // non-empty: invalid
};
[[nodiscard]] Parsed parse(const nlohmann::json& args);

// One question being answered.
struct Answer {
    std::vector<bool> chosen;  // per option
    std::string typed;         // the free-text answer, when given
    bool answered = false;
};

// The picker. Rows are the options followed by ONE free-text row.
struct State {
    std::vector<Question> questions;
    std::vector<Answer> answers;
    std::size_t current = 0;   // which question is shown
    std::size_t cursor  = 0;   // row within it; options.size() is the text row
    bool typing = false;       // keys go to the text row

    [[nodiscard]] const Question& q() const { return questions[current]; }
    [[nodiscard]] Answer& a() { return answers[current]; }
    [[nodiscard]] const Answer& a() const { return answers[current]; }
    [[nodiscard]] std::size_t rows() const { return q().options.size() + 1; }
    [[nodiscard]] bool on_text_row() const { return cursor == q().options.size(); }
};
[[nodiscard]] State start(std::vector<Question> qs);

// What a key did. Done: every question has an answer, send them.
enum class Step { Stay, Done };

void move(State& s, int delta);             // up/down, wraps
void toggle(State& s);                      // space: tick (multiple) or pick (single)
[[nodiscard]] Step choose(State& s);        // enter
[[nodiscard]] Step pick_number(State& s, int n);   // 1-based digit shortcut
void type(State& s, std::string_view utf8); // a character on the text row
void backspace(State& s);                   // one code point off the text row
// Esc. On the text row it leaves typing; otherwise it means "skip all".
// Returns true when the whole question was dismissed.
[[nodiscard]] bool escape(State& s);
// Back to the previous question (Shift+Tab / left). No-op on the first.
void back(State& s);

// The tool result. answered() for a finished state; dismissed() when the user
// skipped. Both phrased for the model.
[[nodiscard]] std::string answered(const State& s);
[[nodiscard]] std::string dismissed();
// When no user can be asked (a headless run): what the model should do.
[[nodiscard]] std::string unavailable();

}  // namespace agentty::question
