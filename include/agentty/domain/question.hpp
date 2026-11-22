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
    bool recommended = false;  // the model's pick; the cursor starts here
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

// The picker. Rows are the options followed by ONE free-text row. With more
// than one question, the last step is a review page: every answer at once,
// Enter sends, a digit jumps back to change one.
struct State {
    std::vector<Question> questions;
    std::vector<Answer> answers;
    std::size_t current = 0;   // which question is shown
    std::size_t cursor  = 0;   // row within it; options.size() is the text row
    bool typing = false;       // keys go to the text row
    bool reviewing = false;    // the review page is shown instead of a question

    [[nodiscard]] const Question& q() const { return questions[current]; }
    [[nodiscard]] Answer& a() { return answers[current]; }
    [[nodiscard]] const Answer& a() const { return answers[current]; }
    [[nodiscard]] std::size_t rows() const { return q().options.size() + 1; }
    [[nodiscard]] bool on_text_row() const { return cursor == q().options.size(); }
    [[nodiscard]] bool all_answered() const;
};
[[nodiscard]] State start(std::vector<Question> qs);

// What a key did. Done: every question has an answer, send them.
enum class Step { Stay, Done };

void move(State& s, int delta);             // up/down, wraps
void toggle(State& s);                      // space: tick (multiple) or pick (single)
[[nodiscard]] Step choose(State& s);        // enter (on review: send)
[[nodiscard]] Step pick_number(State& s, int n);   // 1-based digit (review: edit that question)
void type(State& s, std::string_view utf8); // a character on the text row
void backspace(State& s);                   // one code point off the text row
// Esc. On the text row it leaves typing; on the review page it goes back to
// the last question; otherwise it means "skip all". True when dismissed.
[[nodiscard]] bool escape(State& s);
// The previous / next question, like tabs (Shift+Tab or ← / Tab or →).
// Forward from the last question opens the review once everything is answered.
void back(State& s);
void forward(State& s);

// The tool result. answered() for a finished state; dismissed() when the user
// skipped. Both phrased for the model.
[[nodiscard]] std::string answered(const State& s);
[[nodiscard]] std::string dismissed();
// When no user can be asked (a headless run): what the model should do.
[[nodiscard]] std::string unavailable();

// The answers back out of an answered() result, one entry per question in
// order (empty answer: none given). For the transcript's record of the card.
// Empty when `result` is not an answered() result.
[[nodiscard]] std::vector<std::string> answers_in(std::string_view result);

}  // namespace agentty::question
