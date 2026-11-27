// key_routing_test: a keypress is routed by the CURRENT model.
//
// Most tests dispatch Msg values straight into update(), which skips the
// half of the input path that decides WHICH handler a key reaches. These
// drive it: route a key through AgenttyApp::on (what jaal calls, with the
// live model), apply the result, route the next key.
//
// Routing used to be a router closed over a snapshot built by subscribe(),
// and a message that changed which surface owned the keyboard left the
// next key routed against the old one ("the input doesn't register until
// you hit it again"). jaal D43 removed routers, so this can't go stale any
// more; these tests keep it that way.
#include "agtest.hpp"

#include <optional>
#include <vector>

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/app/program.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/subscribe.hpp"
#include "agentty/runtime/panel/appearance.hpp"
#include "agentty/runtime/panel/top.hpp"
#include "agentty/runtime/panel/settings/categories.hpp"
#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/runtime/app/deps.hpp"

using namespace agentty;
namespace pn = agentty::ui::panel;

namespace {

// Route one key the way the kernel does: the program's on() for KeyEvent,
// called with the model as it is now.
std::optional<Msg> route(const Model& m, const maya::KeyEvent& ev) {
    return app::AgenttyApp::on(m, ev);
}

// Press a key: route it against the current model, then apply whatever it
// produced.
[[nodiscard]] bool press(Model& m, const maya::KeyEvent& ev) {
    auto msg = route(m, ev);
    if (!msg) return false;
    m = app::update(std::move(m), std::move(*msg)).first;
    return true;
}

// Escape routes through a handler that reaches app::deps() (it persists the
// restored theme), so the stub has to be installed even though this test
// only cares about routing.
void install_stub_deps() {
    app::install_deps(app::Deps{
        .stream        = [](const maya::unlocked&, provider::Request, provider::EventSink) {},
        .save_thread   = [](const Thread&) {},
        .delete_thread = [](const ThreadId&) {},
        .load_threads  = [] { return std::vector<Thread>{}; },
        .load_thread   = [](const ThreadId&) { return std::optional<Thread>{}; },
        .load_settings = [] { return store::Settings{}; },
        .save_settings = [](const store::Settings&) {},
        .new_thread_id = [] { return ThreadId{}; },
        .title_from    = [](std::string_view t) { return std::string{t}; },
        .write_file    = [](const std::string&, const std::string&) {},
        .auth          = {},
    });
}

maya::KeyEvent special(maya::SpecialKey k) {
    maya::KeyEvent ev;
    ev.key = k;
    return ev;
}

} // namespace

TEST_CASE("routing: the first arrow after Enter reaches the theme browser") {
    Model m;
    m = app::update(std::move(m), Msg{OpenAppearance{}}).first;

    // Walk to the Theme row the way a user does, so the form's focus is
    // real rather than assumed.
    const auto* pane = m.ui.panel.get<pn::Appearance>();
    REQUIRE(pane != nullptr);

    // Open the browser with Enter. This is the panel-CHANGING key: after it
    // the Appearance pane owns the keyboard in a different mode.
    m = app::update(std::move(m), Msg{AppearancePickTheme{}}).first;
    const auto* opened = m.ui.panel.get<pn::Appearance>();
    REQUIRE(opened != nullptr);
    CHECK(opened->pane.picking, "Enter did not open the browser");

    const int idx_before    = opened->pane.picker.picker.index();
    const int cursor_before = opened->pane.form.cursor;

    // THE ASSERTION: one ↓, routed through a sub built from the model as it
    // is NOW. It must move the browser's highlight. If subscribe() were
    // routing against the pre-Enter snapshot this would move the form
    // cursor underneath instead, and the user would see nothing happen.
    CHECK(press(m, special(maya::SpecialKey::Down)),
          "the arrow produced no message at all");

    const auto* after = m.ui.panel.get<pn::Appearance>();
    REQUIRE(after != nullptr);
    CHECK(after->pane.picker.picker.index() != idx_before,
          "the FIRST arrow after Enter did not move the browser — this is "
          "the 'input doesn't register until you hit it again' bug");
    CHECK(after->pane.form.cursor == cursor_before,
          "the arrow leaked into the form behind the browser");
}

TEST_CASE("routing: Escape leaves the browser and the next key hits the form") {
    install_stub_deps();
    Model m;
    m = app::update(std::move(m), Msg{OpenAppearance{}}).first;
    m = app::update(std::move(m), Msg{AppearancePickTheme{}}).first;
    REQUIRE(m.ui.panel.get<pn::Appearance>() != nullptr);
    CHECK(m.ui.panel.get<pn::Appearance>()->pane.picking);

    // Esc closes the browser — the same transition in reverse. The next key
    // must reach the FORM, not a browser that is no longer up.
    CHECK(press(m, special(maya::SpecialKey::Escape)),
          "Escape produced no message");
    const auto* closed = m.ui.panel.get<pn::Appearance>();
    REQUIRE(closed != nullptr);
    CHECK(!closed->pane.picking, "Escape did not close the browser");

    const int idx_before    = closed->pane.picker.picker.index();
    const int cursor_before = closed->pane.form.cursor;

    CHECK(press(m, special(maya::SpecialKey::Down)),
          "the arrow after Escape produced no message");

    const auto* after = m.ui.panel.get<pn::Appearance>();
    REQUIRE(after != nullptr);
    CHECK(after->pane.form.cursor != cursor_before,
          "the first arrow after closing the browser did not move the form");
    CHECK(after->pane.picker.picker.index() == idx_before,
          "the arrow still reached the closed browser");
}

// ^A/^X once shipped with their arming gate captured by the router and
// missing from SubsKey, so they were dead for a frame after edits landed and
// stale-live after the queue cleared. Routing reads the live model now.
TEST_CASE("routing: the changes-strip chords follow the live model") {
    install_stub_deps();
    Model m;
    m.d.show_changes_strip = true;
    const auto ctrl_x = [] {
        maya::KeyEvent k{};
        k.key = maya::CharKey{U'x'};
        k.mods.ctrl = true;
        return k;
    }();
    const auto is_reject = [](const std::optional<Msg>& msg) {
        const auto* dr = msg ? std::get_if<msg::DiffReviewMsg>(&*msg) : nullptr;
        return dr && std::holds_alternative<RejectAllChanges>(*dr);
    };

    CHECK_FALSE(is_reject(route(m, ctrl_x)));   // nothing queued: not armed

    FileChange fc;
    fc.path = "src/main.cpp";
    m.d.pending_changes.push_back(fc);
    CHECK_MESSAGE(is_reject(route(m, ctrl_x)),
                  "queued changes arm ^X on the very next key");

    m.d.show_changes_strip = false;
    CHECK_MESSAGE(!is_reject(route(m, ctrl_x)),
                  "hiding the strip disarms ^X on the very next key");
}

// The strip shows ^A/^X whenever it is up, so the keys must never fall
// through to the composer (where ^A is line-start and ^X nothing): the
// "something eats them" report was a turn running in the background.
TEST_CASE("routing: advertised strip chords are never eaten") {
    install_stub_deps();
    Model m;
    m.d.show_changes_strip = true;
    FileChange fc;
    fc.path = "src/main.cpp";
    m.d.pending_changes.push_back(fc);
    auto key = [](char32_t c, bool ctrl) {
        maya::KeyEvent k{};
        k.key = maya::CharKey{c};
        k.mods.ctrl = ctrl;
        return k;
    };
    auto is = [](const std::optional<Msg>& msg, auto tag) {
        const auto* dr = msg ? std::get_if<msg::DiffReviewMsg>(&*msg) : nullptr;
        return dr && std::holds_alternative<decltype(tag)>(*dr);
    };
    auto check_all = [&](const char* where) {
        CAPTURE(where);
        CHECK(is(route(m, key(U'a', true)), AcceptAllChanges{}));
        CHECK(is(route(m, key(U'x', true)), RejectAllChanges{}));
        CHECK(is(route(m, key(0x01, false)), AcceptAllChanges{}));   // legacy ^A byte
        CHECK(is(route(m, key(0x18, false)), RejectAllChanges{}));   // legacy ^X byte
        CHECK(is(route(m, key(0x12, false)), OpenDiffReview{}));     // legacy ^R byte
        CHECK(is(route(m, key(U'A', true)), AcceptAllChanges{}));    // kitty ctrl+shift
        CHECK(is(route(m, key(U'r', true)), OpenDiffReview{}));
        auto esc_x = key(U'x', true);                                // Esc folded in
        esc_x.mods.alt = true;
        CHECK(is(route(m, esc_x), RejectAllChanges{}));
        auto super_x = key(U'x', true);                              // the WM's
        super_x.mods.super_ = true;
        CHECK(!is(route(m, super_x), RejectAllChanges{}));
    };

    check_all("idle");
    m.ui.composer.text = "I think the";
    check_all("with a draft in the composer");

    m.s.phase = phase::Streaming{};
    check_all("mid-turn");

    // Mid-turn ^X explains instead of silently arming or reverting files the
    // agent is still writing; ^A keeps them.
    {
        auto [after, cmd] = app::update(std::move(m), Msg{msg::DiffReviewMsg{RejectAllChanges{}}});
        (void)cmd;
        CHECK(!after.d.reject_all_armed);
        CHECK(after.d.pending_changes.size() == 1);
        m = std::move(after);
    }

    m.s.phase = phase::Idle{};
    m = app::update(std::move(m), Msg{OpenTodoModal{}}).first;
    REQUIRE(ui::panel::top(m) == ui::panel::Kind::Todo);
    check_all("with the ambient Todo panel open");
}

// A permission prompt sits above the strip mid-turn. Its letters are plain
// y / n / a; kitty's Ctrl+A ('a' + ctrl) used to match 'a' and grant
// "always allow" instead of keeping the changes.
TEST_CASE("routing: ^A under a permission prompt keeps changes, never grants") {
    install_stub_deps();
    Model m;
    m.d.show_changes_strip = true;
    FileChange fc;
    fc.path = "src/main.cpp";
    m.d.pending_changes.push_back(fc);
    m.s.phase = phase::AwaitingPermission{};
    m.d.pending_permission = PendingPermission{};
    REQUIRE(ui::panel::top(m) == ui::panel::Kind::Permission);

    maya::KeyEvent ctrl_a{};
    ctrl_a.key = maya::CharKey{U'a'};
    ctrl_a.mods.ctrl = true;
    const auto msg = route(m, ctrl_a);
    REQUIRE(msg.has_value());
    const auto* dr = std::get_if<msg::DiffReviewMsg>(&*msg);
    CHECK((dr && std::holds_alternative<AcceptAllChanges>(*dr)));

    // With the strip hidden, ^A still must not grant anything.
    m.d.show_changes_strip = false;
    const auto hidden = route(m, ctrl_a);
    const auto* tm = hidden ? std::get_if<msg::ToolMsg>(&*hidden) : nullptr;
    CHECK(!(tm && std::holds_alternative<PermissionApproveAlways>(*tm)));

    // The plain letter still answers.
    maya::KeyEvent a{};
    a.key = maya::CharKey{U'a'};
    const auto plain = route(m, a);
    REQUIRE(plain.has_value());
    const auto* ptm = std::get_if<msg::ToolMsg>(&*plain);
    CHECK((ptm && std::holds_alternative<PermissionApproveAlways>(*ptm)));
}

// ^/ is the byte 0x1F, the same as ^_. Plain terminals send the raw byte;
// kitty sends ctrl+'/'; tmux with extended keys turns the byte into ctrl+'_'
// once the app asks for modifyOtherKeys. All three must open the picker.
TEST_CASE("routing: every way ^/ arrives opens the model picker") {
    install_stub_deps();
    auto key = [](char32_t c, bool ctrl) {
        maya::KeyEvent ev;
        ev.key = maya::CharKey{c};
        ev.mods.ctrl = ctrl;
        return ev;
    };
    for (const auto& ev : {key(0x1F, false), key(U'/', true), key(U'_', true)}) {
        Model m;
        REQUIRE(press(m, ev));
        CHECK(m.ui.panel.get<pn::Models>() != nullptr);

        // In the open picker the same key is handled (scopes to a provider),
        // not typed into the filter.
        CHECK(route(m, ev).has_value());
        CHECK(press(m, ev));
        CHECK(m.ui.panel.get<pn::Models>() != nullptr);
    }
}

// The caret-based text inputs must all answer Delete (forward erase) and
// Home/End, through the real router, not only the reducer.
TEST_CASE("routing: Delete in the main prompt erases forward, a chip whole") {
    install_stub_deps();
    Model m;
    m.ui.composer.text   = "caf\xc3\xa9!";   // 2-byte code point before '!'
    m.ui.composer.cursor = 3;
    REQUIRE(press(m, special(maya::SpecialKey::Delete)));
    CHECK(m.ui.composer.text == "caf!");
    CHECK(m.ui.composer.cursor == 3);
    REQUIRE(press(m, special(maya::SpecialKey::End)));
    REQUIRE(press(m, special(maya::SpecialKey::Delete)));   // at end: no-op
    CHECK(m.ui.composer.text == "caf!");

    Model c;
    Attachment att;
    att.kind = Attachment::Kind::Paste;
    att.body.set_bytes("x");
    c.ui.composer.attachments.push_back(std::move(att));
    c.ui.composer.text   = "a" + attachment::make_placeholder(0) + "b";
    c.ui.composer.cursor = 1;
    REQUIRE(press(c, special(maya::SpecialKey::Delete)));
    CHECK(c.ui.composer.text == "ab");
    CHECK(c.ui.composer.cursor == 1);
}

TEST_CASE("routing: login text fields answer Delete, Home and End") {
    install_stub_deps();
    Model m;
    m.ui.login = ui::login::ApiKeyInput{.key_input = "sk-\xc3\xa9x", .cursor = 0};
    const auto field = [&] { return std::get<ui::login::ApiKeyInput>(m.ui.login); };
    REQUIRE(press(m, special(maya::SpecialKey::End)));
    CHECK(field().cursor == 6);
    REQUIRE(press(m, special(maya::SpecialKey::Home)));
    CHECK(field().cursor == 0);
    REQUIRE(press(m, special(maya::SpecialKey::Right)));
    REQUIRE(press(m, special(maya::SpecialKey::Right)));
    REQUIRE(press(m, special(maya::SpecialKey::Right)));
    REQUIRE(press(m, special(maya::SpecialKey::Delete)));   // the whole e-acute
    CHECK(field().key_input == "sk-x");
    CHECK(field().cursor == 3);
    REQUIRE(press(m, special(maya::SpecialKey::End)));
    REQUIRE(press(m, special(maya::SpecialKey::Delete)));   // at end: no-op
    CHECK(field().key_input == "sk-x");
}

TEST_CASE("routing: the settings add prompt answers Delete, arrows, Home and End") {
    install_stub_deps();
    Model m = app::update(Model{}, Msg{OpenSettingsList{settings::Category::Plugins}}).first;
    // `n` is the one-line add prompt (`a` on Plugins opens the add form).
    maya::KeyEvent a;
    a.key = maya::CharKey{U'n'};
    REQUIRE(press(m, a));
    const auto list = [&] { return m.ui.panel.get<pn::SettingsList>(); };
    REQUIRE(list() != nullptr);
    REQUIRE(list()->input_active);
    for (char32_t c : std::u32string{U"abc"}) {
        maya::KeyEvent ev;
        ev.key = maya::CharKey{c};
        REQUIRE(press(m, ev));
    }
    REQUIRE(press(m, special(maya::SpecialKey::Home)));
    CHECK(list()->cursor == 0);
    REQUIRE(press(m, special(maya::SpecialKey::Delete)));
    CHECK(list()->input == "bc");
    REQUIRE(press(m, special(maya::SpecialKey::Right)));
    CHECK(list()->cursor == 1);
    REQUIRE(press(m, special(maya::SpecialKey::Left)));
    CHECK(list()->cursor == 0);
    REQUIRE(press(m, special(maya::SpecialKey::End)));
    CHECK(list()->cursor == 2);
    REQUIRE(press(m, special(maya::SpecialKey::Delete)));   // at end: no-op
    CHECK(list()->input == "bc");
    CHECK(list()->input_active);
}
