// plan_footer_test — the plan rides in the composer footer's empty middle.
//
// The ^T modal shows the whole plan, but only while it is open; progress
// that has to be asked for is not ambient. The footer's middle stretch is
// otherwise blank, so it carries a meter, "3/7" and the item in flight.
//
// The claims are about what reaches the SCREEN at every width, so the real
// composer is rendered through the real widget: the model badge and the
// profile chip are never displaced, the label shortens with … instead of
// wrapping, and a narrow terminal degrades to the count rather than to a
// broken row.

#include "agtest.hpp"

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/view/composer.hpp"
#include "agentty/runtime/app/update/internal.hpp"

#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace agentty;

namespace {

// UTF-8 per cell, so the meter and the … are assertable. Returns the footer
// row: the last non-blank row above the bottom border.
std::vector<std::string> render_rows(const Model& m, int width) {
    auto root = maya::Composer{ui::composer_config(m)}.build();
    maya::StylePool pool;
    maya::Canvas canvas(width, 30, &pool);
    canvas.clear();
    maya::render_tree(root, canvas, pool, maya::theme::native, true);
    std::vector<std::string> rows;
    for (int y = 0; y <= canvas.max_content_row(); ++y) {
        std::string s;
        for (int x = 0; x < width; ++x) {
            char32_t c = canvas.get(x, y).character;
            if (c == 0) c = U' ';
            if (c < 0x80) s += static_cast<char>(c);
            else if (c < 0x800) {
                s += static_cast<char>(0xC0 | (c >> 6));
                s += static_cast<char>(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                s += static_cast<char>(0xE0 | (c >> 12));
                s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                s += static_cast<char>(0x80 | (c & 0x3F));
            } else {
                s += static_cast<char>(0xF0 | (c >> 18));
                s += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
                s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                s += static_cast<char>(0x80 | (c & 0x3F));
            }
        }
        rows.push_back(std::move(s));
    }
    return rows;
}

// The footer is the row carrying the profile chip.
std::string footer(const Model& m, int width) {
    for (const auto& r : render_rows(m, width))
        if (r.find("W R I T E") != std::string::npos
         || r.find("A S K") != std::string::npos
         || r.find("\xe2\x96\x8e") != std::string::npos)   // ▎ chip bar
            return r;
    return {};
}

Model with_plan(std::initializer_list<std::pair<const char*, TodoStatus>> items) {
    Model m;
    for (const auto& [c, s] : items) m.ui.todo.items.push_back({c, s});
    return m;
}

const auto P = TodoStatus::Pending;
const auto I = TodoStatus::InProgress;
const auto C = TodoStatus::Completed;

}  // namespace

TEST_CASE("plan footer: wide terminal shows meter, count and the item in flight") {
    Model m = with_plan({{"Read the code", C}, {"Sketch the design", C},
                         {"Wire the cache chip into the status bar", I},
                         {"Write tests", P}, {"Ship", P}});
    m.s.phase = phase::Streaming{};
    const std::string f = footer(m, 160);
    INFO(f);
    CHECK(f.find("2/5") != std::string::npos);
    CHECK(f.find("\xe2\x94\x81") != std::string::npos);          // ━ meter
    CHECK(f.find("\xe2\x97\x8f") != std::string::npos);          // ● in flight
    CHECK(f.find("Wire the cache chip into the status bar") != std::string::npos);
}

TEST_CASE("plan footer: every width keeps the row whole") {
    Model m = with_plan({{"Read the code", C},
                         {"Wire the cache chip into the status bar so a miss is visible", I},
                         {"Ship", P}});
    m.s.phase = phase::Streaming{};
    for (int w : {160, 120, 100, 80, 60, 40}) {
        const std::string f = footer(m, w);
        CAPTURE(w);
        INFO(f);
        // The profile chip is the must-keep anchor: always on the row.
        CHECK(!f.empty());
        // The full 60-char label can never fit beside the badge below ~140
        // columns; when it is shown at all it must be cut with an ellipsis,
        // never wrapped onto a second row.
        if (f.find("Wire the") != std::string::npos
            && f.find("so a miss is visible") == std::string::npos)
            CHECK(f.find("\xe2\x80\xa6") != std::string::npos);
    }
    // Even with room to spare the label is capped (56 cols): the footer is
    // ambient, and a sentence-long todo should not become the loudest thing
    // on it. Cut, never wrapped.
    const std::string wide = footer(m, 160);
    CHECK(wide.find("Wire the cache chip") != std::string::npos);
    CHECK(wide.find("\xe2\x80\xa6") != std::string::npos);
    if (std::getenv("PLAN_FOOTER_DUMP"))
        for (int w : {160, 120, 100, 80, 60, 40})
            std::printf("%3d |%s|\n", w, footer(m, w).c_str());
}

TEST_CASE("plan footer: the label never wraps onto a second footer row") {
    Model m = with_plan({{std::string(200, 'x').c_str(), I}});
    m.s.phase = phase::Streaming{};
    for (int w : {120, 80, 50}) {
        const auto rows = render_rows(m, w);
        int with_x = 0;
        for (const auto& r : rows)
            if (r.find("xxxxxxxxxx") != std::string::npos) ++with_x;
        CAPTURE(w);
        CHECK(with_x <= 1);
    }
}

TEST_CASE("plan footer: model text cannot break the row or inject escapes") {
    Model m = with_plan({{"line one\nline two\x1b[2J\xc2\x9b" "31m tail", I}});
    m.s.phase = phase::Streaming{};
    const std::string f = footer(m, 160);
    INFO(f);
    CHECK(f.find("line one line two") != std::string::npos);
    CHECK(f.find('\x1b') == std::string::npos);
}

TEST_CASE("plan footer: no plan, no slot; a finished plan goes quiet when idle") {
    {
        Model m;
        const std::string f = footer(m, 160);
        CHECK(f.find('/') == std::string::npos);
    }
    {   // Finished, turn still running: the ✓ 2/2 is the payoff.
        Model m = with_plan({{"a", C}, {"b", C}});
        m.s.phase = phase::Streaming{};
        const std::string f = footer(m, 160);
        INFO(f);
        CHECK(f.find("2/2") != std::string::npos);
    }
    {   // Finished and idle: not news any more.
        Model m = with_plan({{"a", C}, {"b", C}});
        REQUIRE(!m.s.active());
        const std::string f = footer(m, 160);
        INFO(f);
        CHECK(f.find("2/2") == std::string::npos);
    }
    {   // Unfinished and idle: "stopped at 1/3" is what you come back to.
        Model m = with_plan({{"a", C}, {"b", P}, {"c", P}});
        REQUIRE(!m.s.active());
        const std::string f = footer(m, 160);
        INFO(f);
        CHECK(f.find("1/3") != std::string::npos);
        CHECK(f.find("\xe2\x97\x8b") != std::string::npos);   // ○ next up
    }
}

TEST_CASE("plan footer: switching threads never shows another thread's plan") {
    Model m = with_plan({{"old thread's step", I}});
    m.d.current = Thread{};               // a fresh thread, no todo calls
    app::detail::restore_todo_from_thread(m);
    CHECK(m.ui.todo.items.empty());

    // A thread whose last todo call left 1/2 done gets exactly that back.
    Message a;
    a.role = Role::Assistant;
    ToolUse tu;
    tu.name = ToolName{"todo"};
    tu.args = nlohmann::json{{"todos", nlohmann::json::array({
        {{"content", "first"},  {"status", "completed"}},
        {{"content", "second"}, {"status", "in_progress"}}})}};
    a.tool_calls.push_back(tu);
    m.d.current.messages.push_back(a);
    app::detail::restore_todo_from_thread(m);
    REQUIRE(m.ui.todo.items.size() == 2);
    CHECK(m.ui.todo.items[1].status == TodoStatus::InProgress);
}

namespace {
nlohmann::json todos(std::initializer_list<std::pair<const char*, const char*>> items) {
    auto arr = nlohmann::json::array();
    for (const auto& [c, s] : items) arr.push_back({{"content", c}, {"status", s}});
    return nlohmann::json{{"todos", arr}};
}
int done_of(const Model& m) {
    int n = 0;
    for (const auto& it : m.ui.todo.items) n += it.status == TodoStatus::Completed;
    return n;
}
}  // namespace

// The model rewrites the WHOLE list on every todo call. While that call
// streams, the partial array is only a prefix of the new plan. Assigning it
// as the plan made the footer read 0/1, 1/2, 2/3... on every preview tick of
// a plan that was really 3/5 -> 4/5 -- the "doesn't update well" report.
TEST_CASE("plan footer: a streaming rewrite never shrinks or rewinds the plan") {
    Model m = with_plan({{"a", C}, {"b", C}, {"c", I}, {"d", P}, {"e", P}});

    // Two items streamed, the second still being written (its status hasn't
    // arrived, so it would read as pending). Only the complete one counts.
    app::detail::sync_todo_state_live(m, todos({{"a", "completed"}, {"b", "pend"}}));
    CHECK(m.ui.todo.items.size() == 5);
    CHECK(done_of(m) == 2);

    // Further in: c is now done, d in flight. The total holds at 5 and the
    // flip shows the moment its item is written.
    app::detail::sync_todo_state_live(m, todos({
        {"a", "completed"}, {"b", "completed"}, {"c", "completed"},
        {"d", "in_progress"}, {"e", "pend"}}));
    CHECK(m.ui.todo.items.size() == 5);
    CHECK(done_of(m) == 3);
    CHECK(m.ui.todo.items[3].status == TodoStatus::InProgress);

    // The call closes: exactly its list, e included.
    app::detail::sync_todo_state_from_args(m, todos({
        {"a", "completed"}, {"b", "completed"}, {"c", "completed"},
        {"d", "in_progress"}, {"e", "pending"}}));
    CHECK(m.ui.todo.items.size() == 5);
    CHECK(footer(m, 120).find("3/5") != std::string::npos);
}

TEST_CASE("plan footer: a new plan streams as itself; an empty list clears") {
    Model m = with_plan({{"old", C}, {"older", I}});
    // Nothing in common with the current plan: shown as it streams, without
    // the old plan's leftovers padding the total.
    app::detail::sync_todo_state_live(m, todos({{"new one", "in_progress"}, {"new tw", "pe"}}));
    REQUIRE(m.ui.todo.items.size() == 1);
    CHECK(m.ui.todo.items[0].content == "new one");

    // `todos: []` is the model clearing the plan; it used to be ignored.
    app::detail::sync_todo_state_from_args(m, nlohmann::json{{"todos", nlohmann::json::array()}});
    CHECK(m.ui.todo.items.empty());
}
