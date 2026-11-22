// question_tool_test — the `question` tool, end to end without a terminal:
// what a call may look like, what each key does, what the model gets back,
// when the tool exists at all, and the reducer path from a pending call to
// a settled one.

#include <chrono>
#include <string>
#include <utility>

#include "agtest.hpp"

#include <nlohmann/json.hpp>

#include "agentty/domain/question.hpp"
#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/panel/top.hpp"
#include "agentty/tool/question_tool.hpp"
#include "agentty/tool/registry.hpp"
#include "agentty/tool/tool.hpp"   // DynamicDispatch

namespace A = agentty;
namespace q = agentty::question;
using nlohmann::json;

namespace {

json two_questions() {
    return json{{"questions", json::array({
        {{"question", "Which database?"}, {"header", "Database"},
         {"options", json::array({{{"label", "SQLite"}, {"description", "one file"}},
                                  {{"label", "Postgres"}}})}},
        {{"question", "Which features?"}, {"multiple", true},
         {"options", json::array({"auth", "billing", "search"})}},
    })}};
}

q::State started(const json& args) {
    auto p = q::parse(args);
    REQUIRE(p.error.empty());
    return q::start(std::move(p.questions));
}

}  // namespace

TEST_CASE("question: parse takes the documented shape and the common slips") {
    auto p = q::parse(two_questions());
    REQUIRE(p.error.empty());
    REQUIRE(p.questions.size() == 2);
    CHECK(p.questions[0].header == "Database");
    CHECK(p.questions[0].options[0].description == "one file");
    CHECK(p.questions[1].multiple);
    CHECK(p.questions[1].options.size() == 3);   // bare strings are labels

    // One question at the top level instead of a list.
    auto single = q::parse(json{{"question", "Proceed?"}, {"options", json::array({"yes", "no"})}});
    REQUIRE(single.error.empty());
    CHECK(single.questions.size() == 1);

    CHECK_FALSE(q::parse(json::object()).error.empty());
    CHECK_FALSE(q::parse(json{{"questions", json::array({json{{"question", ""}}})}}).error.empty());
    json five{{"questions", json::array()}};
    for (int i = 0; i < 5; ++i) five["questions"].push_back(json{{"question", "q?"}});
    CHECK_FALSE(q::parse(five).error.empty());   // at most four
}

TEST_CASE("question: a single choice is one enter; answers come back by label") {
    auto s = started(two_questions());
    q::move(s, +1);                                    // Postgres
    CHECK(q::choose(s) == q::Step::Stay);              // on to question 2
    CHECK(s.current == 1);
    q::toggle(s);                                      // tick auth
    q::move(s, +2);
    q::toggle(s);                                      // tick search
    CHECK(q::choose(s) == q::Step::Stay);              // several questions: review first
    CHECK(s.reviewing);
    CHECK(q::choose(s) == q::Step::Done);              // enter on review sends
    const auto out = q::answered(s);
    CHECK(out.find("Which database?") != std::string::npos);
    CHECK(out.find("Postgres") != std::string::npos);
    CHECK(out.find("SQLite") == std::string::npos);
    CHECK(out.find("auth") != std::string::npos);
    CHECK(out.find("search") != std::string::npos);
    CHECK(out.find("billing") == std::string::npos);
}

TEST_CASE("question: digits pick; a typed answer comes from the composer") {
    auto s = started(two_questions());
    CHECK(q::pick_number(s, 1) == q::Step::Stay);      // SQLite, next question
    CHECK(s.current == 1);
    // Blank text answers nothing; real text answers the question on screen.
    CHECK(q::answer_text(s, "  \n") == q::Step::Stay);
    CHECK_FALSE(s.a().answered);
    q::toggle(s);                                      // tick auth too
    CHECK(q::answer_text(s, " only on weekdays ") == q::Step::Stay);   // review
    CHECK(s.answers[1].typed == "only on weekdays");
    CHECK(s.answers[1].chosen[0]);                     // several allowed: kept
    CHECK(q::choose(s) == q::Step::Done);
    CHECK(q::answered(s).find("(in their own words) only on weekdays") != std::string::npos);

    // One choice allowed: typed text replaces a pick.
    auto t = started(json{{"question", "Which?"}, {"options", json::array({"a", "b"})}});
    q::toggle(t);
    CHECK(q::answer_text(t, "neither") == q::Step::Done);
    CHECK_FALSE(t.answers[0].chosen[0]);
}

TEST_CASE("question: esc stops; only the review page takes it as back") {
    auto s = started(two_questions());
    CHECK_FALSE(q::escape_stays(s));                   // stop the turn
    (void)q::pick_number(s, 2);
    const auto out = q::stopped(s);
    CHECK(out.starts_with("The user pressed Esc"));
    CHECK(out.find("Postgres") != std::string::npos);  // what they did answer
    CHECK(q::answers_in(out).empty());                 // not an answered() result
}

TEST_CASE("question: a question with no options waits for typed text") {
    auto s = started(json{{"question", "Name?"}, {"options", json::array()}});
    CHECK(q::choose(s) == q::Step::Stay);              // enter alone does nothing
    CHECK(q::answer_text(s, "agentty") == q::Step::Done);   // one question: no review
}

TEST_CASE("question: the cursor starts on the model's suggestion") {
    auto p = q::parse(json{{"question", "Which?"}, {"options", json::array({
        {{"label", "a"}}, {{"label", "b"}, {"recommended", true}},
        {{"label", "c"}, {"recommended", true}}})}});
    REQUIRE(p.error.empty());
    CHECK(p.questions[0].options[1].recommended);
    CHECK_FALSE(p.questions[0].options[2].recommended);   // one pick only
    CHECK(q::start(p.questions).cursor == 1);
    // Claude Code's spelling in the label is taken as the mark, and dropped.
    auto cc = q::parse(json{{"question", "Which?"}, {"options", json::array({
        "x", "y (Recommended)"})}});
    CHECK(cc.questions[0].options[1].label == "y");
    CHECK(cc.questions[0].options[1].recommended);
}

TEST_CASE("question: review lets you change one answer, tabs move between questions") {
    auto s = started(two_questions());
    CHECK(q::pick_number(s, 1) == q::Step::Stay);      // SQLite
    q::forward(s);                                     // already on 2: no review yet
    CHECK_FALSE(s.reviewing);
    q::back(s);
    CHECK(s.current == 0);
    CHECK(s.cursor == 0);                              // remembers SQLite
    q::forward(s);
    CHECK(s.current == 1);
    q::toggle(s);
    CHECK(q::choose(s) == q::Step::Stay);
    REQUIRE(s.reviewing);
    CHECK(q::pick_number(s, 1) == q::Step::Stay);      // change question 1
    CHECK_FALSE(s.reviewing);
    CHECK(s.current == 0);
    CHECK(q::pick_number(s, 2) == q::Step::Stay);      // Postgres; all answered
    CHECK(s.reviewing);
    CHECK(q::escape_stays(s));                         // esc on review: back, not stop
    CHECK_FALSE(s.reviewing);

    // answers_in reads the result back for the transcript card.
    auto t = started(two_questions());
    (void)q::pick_number(t, 2);
    q::toggle(t);
    q::move(t, 2);
    q::toggle(t);
    (void)q::choose(t);
    const auto back = q::answers_in(q::answered(t));
    REQUIRE(back.size() == 2);
    CHECK(back[0] == "Postgres");
    CHECK(back[1] == "auth, search");
    CHECK(q::answers_in(q::stopped(t)).empty());
}

TEST_CASE("question: the tool exists only where someone can answer and wants to") {
    const auto& def = A::tools::question_tool::def();
    CHECK(def.name.value == "question");
    CHECK(def.effects.empty());   // pure: never prompts for permission
    // Headless (nobody installed the switch) or switched off: withheld.
    CHECK(A::tools::withheld_by_policy(def, true, false));
    CHECK_FALSE(A::tools::withheld_by_policy(def, true, true));
    // web_search's rule is unchanged by the new one.
    A::tools::ToolDef ws;
    ws.name = A::ToolName{"web_search"};
    CHECK(A::tools::withheld_by_policy(ws, false, true));
    CHECK_FALSE(A::tools::withheld_by_policy(ws, true, false));
    // Run directly (no TUI), it says so instead of pretending.
    auto r = agentty::tool::DynamicDispatch::execute(agtest::cap(), "question", two_questions());
    REQUIRE(r);
    CHECK(r->text.find("No one can answer") != std::string::npos);
}

namespace {

// One assistant turn whose only call is a question.
A::Model asking(bool questions_on = true) {
    A::Model m;
    m.d.profile = A::Profile::Write;
    m.d.persisted.questions = questions_on;
    m.ui.library.loaded = true;
    A::Message user;
    user.role = A::Role::User;
    user.text = "add a cache";
    m.d.current.messages.push_back(std::move(user));
    A::Message asst;
    asst.role = A::Role::Assistant;
    A::ToolUse tc;
    tc.id     = A::ToolCallId{"q1"};
    tc.name   = A::ToolName{"question"};
    tc.args   = json{{"question", "Which database?"},
                     {"options", json::array({"SQLite", "Postgres"})}};
    tc.status = A::ToolUse::Pending{std::chrono::steady_clock::now()};
    asst.tool_calls.push_back(std::move(tc));
    m.d.current.messages.push_back(std::move(asst));
    m.s.phase = A::phase::ExecutingTool{A::phase::Active{}};
    return m;
}

const A::ToolUse& call(const A::Model& m) {
    for (const auto& msg : m.d.current.messages)
        for (const auto& tc : msg.tool_calls)
            if (tc.id.value == "q1") return tc;
    FAIL("q1 is gone");
    static const A::ToolUse none;
    return none;
}

A::Model send(A::Model m, A::Msg msg) {
    auto [next, cmd] = agentty::app::update(std::move(m), std::move(msg));
    (void)cmd;
    return std::move(next);
}

}  // namespace

TEST_CASE("question: a call opens the picker, owns the keys, and settles with the answer") {
    auto m = asking();
    (void)agentty::app::cmd::kick_pending_tools(m);
    REQUIRE(m.d.pending_question);
    CHECK(call(m).is_pending());                       // never dispatched
    CHECK(m.s.is_awaiting_permission());               // the turn waits on the user
    CHECK(A::ui::panel::top(m) == A::ui::panel::Kind::Question);

    m = send(std::move(m), A::Msg{A::QuestionMove{+1}});
    m = send(std::move(m), A::Msg{A::QuestionChoose{}});
    CHECK_FALSE(m.d.pending_question);
    REQUIRE(call(m).is_done());
    CHECK(call(m).output().find("Postgres") != std::string::npos);
    CHECK_FALSE(m.s.is_awaiting_permission());
}

TEST_CASE("question: esc settles the call and stops the turn") {
    auto m = asking();
    (void)agentty::app::cmd::kick_pending_tools(m);
    REQUIRE(m.d.pending_question);
    m = send(std::move(m), A::Msg{A::QuestionEscape{}});
    CHECK_FALSE(m.d.pending_question);
    REQUIRE(call(m).is_done());                        // well-formed for the next turn
    CHECK(call(m).output().starts_with("The user pressed Esc"));
    CHECK(m.s.is_idle());                              // the model does not carry on
}

TEST_CASE("question: enter with text in the composer answers with it") {
    auto m = asking();
    (void)agentty::app::cmd::kick_pending_tools(m);
    REQUIRE(m.d.pending_question);
    m.ui.composer.text = "memcached, we already run it";
    m.ui.composer.cursor = static_cast<int>(m.ui.composer.text.size());
    m = send(std::move(m), A::Msg{A::QuestionAnswerText{}});
    CHECK_FALSE(m.d.pending_question);
    CHECK(m.ui.composer.text.empty());                 // the box was the answer
    REQUIRE(call(m).is_done());
    CHECK(call(m).output().find("memcached, we already run it") != std::string::npos);
}

TEST_CASE("question: switched off, a stray call is answered at once and nothing opens") {
    auto m = asking(/*questions_on=*/false);
    (void)agentty::app::cmd::kick_pending_tools(m);
    CHECK_FALSE(m.d.pending_question);
    REQUIRE(call(m).is_done());
    CHECK(call(m).output().find("switched clarifying questions off") != std::string::npos);
}

TEST_CASE("question: the switch persists and is published to the tool layer") {
    A::Model m;
    REQUIRE(m.d.persisted.questions);                  // on by default
    m = send(std::move(m), A::Msg{A::ToggleQuestions{}});
    CHECK_FALSE(m.d.persisted.questions);
    REQUIRE(m.published_questions);
    CHECK_FALSE(*m.published_questions);
}
