// thread_switch_peek_test — opening a thread from ^J reads it once.
//
// The ^J peek already reads and parses the thread under the cursor (to draw
// its card). Opening that thread used to read and parse the SAME file again:
// on a 16 MB thread that second read was most of the switch. These pin that
// the open reuses the peek's parse, waits for the peek when Enter lands while
// it is still reading, and still reads fresh when the log changed since.
#include "agtest.hpp"
#include "agtest_fx.hpp"

#include <atomic>
#include <optional>
#include <string>

#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/panel/common.hpp"

using namespace agentty;
namespace pn = agentty::ui::panel;
namespace detail = agentty::app::detail;

namespace {

std::atomic<int> g_reads{0};
LogStamp         g_stamp{100, 7};

Thread big_thread(const std::string& id) {
    Thread t;
    t.id = ThreadId{id};
    t.title = "big";
    for (int i = 0; i < 6; ++i) {
        Message u; u.role = Role::User; u.text = "q" + std::to_string(i);
        Message a; a.role = Role::Assistant; a.text = "a" + std::to_string(i);
        t.messages.push_back(std::move(u));
        t.messages.push_back(std::move(a));
    }
    return t;
}

void install() {
    app::install_deps(app::Deps{
        .stream        = [](const maya::unlocked&, provider::Request, provider::EventSink) {},
        .save_thread   = [](const Thread&) {},
        .delete_thread = [](const ThreadId&) {},
        .load_threads  = [] { return std::vector<Thread>{}; },
        .load_thread   = [](const ThreadId& id) {
            ++g_reads;
            return std::optional<Thread>{big_thread(id.value)};
        },
        .log_stamp     = [](const ThreadId&) { return g_stamp; },
        .load_settings = [] { return store::Settings{}; },
        .save_settings = [](const store::Settings&) {},
        .new_thread_id = [] { return ThreadId{"fresh"}; },
        .title_from    = [](std::string_view t) { return std::string{t}; },
        .auth          = {},
    });
}

// A picker open on the one saved thread "big", current = an empty thread.
Model picker_on_big() {
    Model m;
    Thread meta; meta.id = ThreadId{"big"}; meta.title = "big";
    m.d.threads.push_back(meta);
    m.d.current.id = ThreadId{"cur"};
    m.ui.panel.descend(pn::ThreadList{{0}});
    if (auto* p = m.ui.panel.get<pn::ThreadList>()) p->peek_id = "big";
    return m;
}

// One fold, then run the tasks it returned; fold whatever they sent.
Model fold(Model m, Msg msg, int depth = 0) {
    auto [next, cmd] = app::update(std::move(m), std::move(msg));
    if (depth > 4) return std::move(next);
    for (auto& out : agtest::fx::run_tasks(std::move(cmd)))
        next = fold(std::move(next), std::move(out), depth + 1);
    return std::move(next);
}

// The peek's own load, landing with the whole thread.
Msg peek_landed() {
    auto t = big_thread("big");
    ThreadPeekLoaded e;
    e.id = ThreadId{"big"};
    e.turns = std::vector<ui::PeekTurn>{};
    e.thread = std::move(t);
    e.stamp = g_stamp;
    return Msg{msg::ThreadListMsg{std::move(e)}};
}

}  // namespace

TEST_CASE("thread switch: Enter after the peek landed opens its parse, no second read") {
    install();
    g_stamp = {100, 7};
    Model m = picker_on_big();
    m = fold(std::move(m), peek_landed());
    REQUIRE(m.ui.peek_full.has_value());
    g_reads = 0;
    m = fold(std::move(m), Msg{msg::ThreadListMsg{ThreadListSelect{}}});
    CHECK(g_reads == 0);
    CHECK(m.d.current.id.value == "big");
    CHECK(m.d.current.messages.size() == 12);
    CHECK(!m.ui.peek_full.has_value());
}

TEST_CASE("thread switch: Enter during the peek waits for it instead of reading twice") {
    install();
    g_stamp = {100, 7};
    Model m = picker_on_big();
    m.ui.peek_loading = "big";        // the peek is reading the file now
    g_reads = 0;
    m = fold(std::move(m), Msg{msg::ThreadListMsg{ThreadListSelect{}}});
    CHECK(g_reads == 0);              // no second read started
    CHECK(m.s.thread_loading);
    m = fold(std::move(m), peek_landed());
    CHECK(g_reads == 0);
    CHECK(m.d.current.id.value == "big");
}

TEST_CASE("thread switch: a log written since the peek is read fresh") {
    install();
    g_stamp = {100, 7};
    Model m = picker_on_big();
    m = fold(std::move(m), peek_landed());
    g_stamp = {140, 9};               // another agentty appended to it
    g_reads = 0;
    m = fold(std::move(m), Msg{msg::ThreadListMsg{ThreadListSelect{}}});
    CHECK(g_reads == 1);
    CHECK(m.d.current.id.value == "big");
}

TEST_CASE("thread switch: the peek's parse is dropped when the picker closes") {
    install();
    Model m = picker_on_big();
    m = fold(std::move(m), peek_landed());
    REQUIRE(m.ui.peek_full.has_value());
    m.ui.panel.close<pn::ThreadList>();
    m = fold(std::move(m), Msg{Tick{}});
    CHECK(!m.ui.peek_full.has_value());
}

TEST_CASE("thread switch: no scrollback replay, the content swap doesn't need one") {
    install();
    Model m = picker_on_big();
    m = fold(std::move(m), peek_landed());
    auto [opened, c1] = app::update(std::move(m), Msg{msg::ThreadListMsg{ThreadListSelect{}}});
    auto loaded = agtest::fx::run_tasks(std::move(c1));
    REQUIRE(loaded.size() == 1);
    auto [after, c2] = app::update(std::move(opened), std::move(loaded.front()));
    const auto* r = agtest::fx::find<maya::ResetInline>(c2);
    REQUIRE(r != nullptr);
    CHECK(!r->replay_history);
    (void)after;
}

TEST_CASE("thread switch: an explicit Scrollback level still replays history") {
    install();
    g_stamp = {100, 7};
    Model m = picker_on_big();
    m.d.persisted.ui.scrollback = ui_prefs::Scrollback::Big;   // "last ~20k rows"
    m = fold(std::move(m), peek_landed());
    auto [opened, c1] = app::update(std::move(m), Msg{msg::ThreadListMsg{ThreadListSelect{}}});
    auto loaded = agtest::fx::run_tasks(std::move(c1));
    REQUIRE(loaded.size() == 1);
    auto [after, c2] = app::update(std::move(opened), std::move(loaded.front()));
    const auto* r = agtest::fx::find<maya::ResetInline>(c2);
    REQUIRE(r != nullptr);
    CHECK(r->replay_history);
    (void)after;
}
