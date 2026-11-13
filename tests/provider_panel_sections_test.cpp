// provider_panel_sections_test — the ^P provider picker's SECTIONED browse
// view, asserted against real rendered cells.
//
// `^P` is the surface where you MANAGE backends, so browsing leads with the
// providers you can actually use and collapses the rest of the registry into
// one "N more providers · type to search" signpost. The headers and that
// signpost are SPELLED in the view, so this renders through the real view +
// maya and reads them back off the canvas: a source-text assertion can't tell
// you the hint is reachable, or that hiding a row didn't also make it
// unfindable.

#include "agtest.hpp"

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/provider_rows.hpp"
#include "agentty/runtime/fused_models.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/subscribe.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/view/panels.hpp"

#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>

#include <string>
#include <vector>

using namespace agentty;
namespace pn = agentty::ui::panel;

namespace {

// Non-ASCII folds to '?' — the section titles and the hint are ASCII, and the
// badge chrome around them is not what we're asserting on.
std::string render_panel(const Model& m, int width = 110, int height = 220) {
    auto root = ui::providers_panel(m);
    maya::StylePool pool;
    maya::Canvas canvas(width, height, &pool);
    canvas.clear();
    maya::render_tree(root, canvas, pool, maya::theme::native, true);

    std::string out;
    const int max_row = canvas.max_content_row();
    for (int y = 0; y <= max_row; ++y) {
        std::string line;
        for (int x = 0; x < width; ++x) {
            char32_t ch = canvas.get(x, y).character;
            if (ch == 0) ch = U' ';
            line.push_back(ch < 128 ? static_cast<char>(ch) : '?');
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line;
        out += '\n';
    }
    return out;
}

bool has(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}

Model picker_model(std::string query = {}) {
    Model m;
    m.ui.panel.descend(pn::Providers{});
    if (auto* p = m.ui.panel.get<pn::Providers>()) p->query = std::move(query);
    return m;
}

}  // namespace

TEST_CASE("provider picker: browse hides unconfigured behind one signpost") {
    // Nothing configured — the fresh-install shape. The vendor registry must
    // NOT be dumped as a wall of identical amber rows.
    Model m = picker_model();
    const std::string screen = render_panel(m);
    INFO(screen);

    // The signpost is present and names the VERB, not just a count.
    CHECK(has(screen, "more providers"));
    CHECK(has(screen, "Show"));

    // Providers you haven't set up are not in the browse list.
    CHECK_FALSE(has(screen, "Cerebras"));
    CHECK_FALSE(has(screen, "Fireworks"));

    // "Custom host…" stays reachable: it's the one row that always applies.
    CHECK(has(screen, "ADD"));
}

TEST_CASE("provider picker: ^E expands the withheld providers in place") {
    // The signpost must be ACTIONABLE, not just informative. Expanding keeps
    // the picker open and reveals the rows it was standing in for.
    Model m = picker_model();
    auto* p = m.ui.panel.get<pn::Providers>();
    REQUIRE(p != nullptr);
    p->show_all = true;

    const std::string screen = render_panel(m, 110, 400);
    INFO(screen);

    CHECK(has(screen, "Cerebras"));
    CHECK(has(screen, "DeepSeek"));
    CHECK(has(screen, "NOT CONFIGURED"));
    // Nothing is withheld any more, so the signpost retires.
    CHECK_FALSE(has(screen, "more providers"));
    // The footer offers the way back.
    CHECK(has(screen, "fewer"));
}

TEST_CASE("provider picker: Enter on the signpost expands, doesn't close") {
    // The signpost shares the NewCustomHost payload with the "Custom host..."
    // sentinel, so Enter on it must be intercepted BEFORE the close/switch
    // path or it would open the host prompt instead of expanding.
    namespace P = agentty::provider;
    Model m;
    m.ui.panel.descend(pn::Providers{});

    // Must mirror the reducer's own build exactly -- it passes the active
    // provider id, which can re-order rows, so an index derived without it
    // would point at a different row.
    const auto active = m.d.selection.catalog_id();
    const auto rows = ui::build_sectioned_provider_rows(
        {}, "", m.d.persisted, m.d.auth, active, false);
    int sign_idx = -1;
    for (int i = 0; i < static_cast<int>(rows.rows.size()); ++i)
        if (rows.rows[static_cast<std::size_t>(i)].more_signpost) {
            sign_idx = i;
            break;
        }
    REQUIRE(sign_idx >= 0);
    REQUIRE(rows.n_hidden > 0);

    m.ui.panel.get<pn::Providers>()->index = sign_idx;
    auto [m2, cmd] = app::update(std::move(m), Msg{ProvidersSelect{}});

    auto* p2 = m2.ui.panel.get<pn::Providers>();
    REQUIRE(p2 != nullptr);              // still open -- not a destination
    CHECK(p2->show_all);
    // The cursor parked on the first revealed row, so the payoff is under it.
    const auto after = ui::build_sectioned_provider_rows(
        {}, "", m2.d.persisted, m2.d.auth, active, true);
    REQUIRE(p2->index < static_cast<int>(after.rows.size()));
    CHECK(after.rows[static_cast<std::size_t>(p2->index)].section
              == ui::ProviderSection::More);
}

// ── Key path ─────────────────────────────────────────────────────────────
//
// The rows above are built by a pure function; these press REAL keys through
// app::on_key so a binding that never reaches the reducer is caught here
// rather than by hand in the terminal.

TEST_CASE("provider picker: ^E toggles through the real key path") {
    // A terminal sends ^E as the bare control byte 0x05 with no ctrl flag;
    // some send the letter with mods.ctrl. BOTH must work -- char_view folds
    // 0x01..0x1A to a letter and synthesises ctrl, and if either encoding
    // missed the binding the key would look flaky rather than broken.
    auto press = [](Model&& mm, maya::KeyEvent ev) {
        if (auto msg = app::on_key(mm, ev))
            mm = app::update(std::move(mm), std::move(*msg)).first;
        return std::move(mm);
    };
    auto fresh = [] {
        Model mm;
        mm.ui.panel.descend(pn::Providers{});
        return mm;
    };

    {   // raw 0x05, no modifier flag
        maya::KeyEvent ev{maya::CharKey{static_cast<char32_t>(0x05)}, {}, {}};
        Model m2 = press(fresh(), ev);
        auto* p = m2.ui.panel.get<pn::Providers>();
        REQUIRE(p != nullptr);
        CHECK(p->show_all);
    }
    {   // 'e' with ctrl set
        maya::KeyEvent ev{maya::CharKey{U'e'}, {}, {}};
        ev.mods.ctrl = true;
        Model m2 = press(fresh(), ev);
        auto* p = m2.ui.panel.get<pn::Providers>();
        REQUIRE(p != nullptr);
        CHECK(p->show_all);
    }
    {   // and it TOGGLES -- two presses return to collapsed
        maya::KeyEvent ev{maya::CharKey{static_cast<char32_t>(0x05)}, {}, {}};
        Model m2 = press(press(fresh(), ev), ev);
        auto* p = m2.ui.panel.get<pn::Providers>();
        REQUIRE(p != nullptr);
        CHECK_FALSE(p->show_all);
    }
}

TEST_CASE("provider picker: no binding on a multiplexer prefix chord") {
    // ^A is tmux's default prefix and ^B is screen's: under a multiplexer
    // the FIRST press is eaten and never reaches us, so a binding there
    // looks like it needs pressing twice. nav.hpp warns about this; the
    // rule only holds if something checks it.
    auto fresh = [] {
        Model mm;
        mm.ui.panel.descend(pn::Providers{});
        return mm;
    };
    for (char32_t c : {U'a', U'b'}) {
        Model m = fresh();
        maya::KeyEvent ev{maya::CharKey{c}, {}, {}};
        ev.mods.ctrl = true;
        const auto msg = app::on_key(m, ev);
        // Either nothing, or at least NOT the expand toggle.
        if (msg) {
            const auto* pm = std::get_if<msg::ProvidersMsg>(&*msg);
            if (pm)
                CHECK(!std::holds_alternative<ProvidersToggleAll>(*pm));
        }
    }
}

TEST_CASE("provider picker: typing stays typing, ^A is not swallowed") {
    // ^A must not land in the filter query, and a bare 'a' must not toggle.
    Model m;
    m.ui.panel.descend(pn::Providers{});

    maya::KeyEvent bare{maya::CharKey{U'a'}, {}, {}};
    if (auto msg = app::on_key(m, bare))
        m = app::update(std::move(m), std::move(*msg)).first;
    auto* p = m.ui.panel.get<pn::Providers>();
    REQUIRE(p != nullptr);
    CHECK(p->query == "a");
    CHECK_FALSE(p->show_all);
}

TEST_CASE("provider picker: the row list is never a stale snapshot") {
    // THE bug that read as a flaky key. providers_update used to build the
    // row list ONCE on entry, so any arm running after a mutation saw the
    // PREVIOUS state: after ^E expanded the list, `n` was still the
    // collapsed length, ProvidersMove clamped the cursor to it, and Enter
    // or ^D on a revealed row did nothing. Single-threaded, fully
    // deterministic -- but it looked like a race because it depended on
    // the key SEQUENCE, not on any one key.
    auto press = [](Model&& mm, Msg msg) {
        return app::update(std::move(mm), std::move(msg)).first;
    };

    Model m;
    m.ui.panel.descend(pn::Providers{});

    const int collapsed = static_cast<int>(
        ui::build_sectioned_provider_rows(
            {}, "", m.d.persisted, m.d.auth,
            m.d.selection.catalog_id(), false).rows.size());
    const int expanded = static_cast<int>(
        ui::build_sectioned_provider_rows(
            {}, "", m.d.persisted, m.d.auth,
            m.d.selection.catalog_id(), true).rows.size());
    REQUIRE(expanded > collapsed);       // there is something to reveal

    m = press(std::move(m), Msg{ProvidersToggleAll{}});
    REQUIRE(m.ui.panel.get<pn::Providers>()->show_all);

    // Walk to the very end. With the stale snapshot this stopped at the
    // COLLAPSED length and the last rows were unreachable.
    for (int i = 0; i < expanded * 2; ++i)
        m = press(std::move(m), Msg{ProvidersJump{ProvidersJump::Where::End}});

    auto* p = m.ui.panel.get<pn::Providers>();
    REQUIRE(p != nullptr);
    CHECK(p->index == expanded - 1);
    CHECK(p->index >= collapsed);        // genuinely past the old ceiling

    // And a step down from the end wraps rather than sticking.
    m = press(std::move(m), Msg{ProvidersMove{+1}});
    CHECK(m.ui.panel.get<pn::Providers>()->index == 0);
}

TEST_CASE("provider picker: collapsing never strands the cursor") {
    // The mirror case: ^E while parked on a revealed row must pull the
    // cursor back inside the shorter list, not leave it pointing past the
    // end where Enter would silently do nothing.
    auto press = [](Model&& mm, Msg msg) {
        return app::update(std::move(mm), std::move(msg)).first;
    };

    Model m;
    m.ui.panel.descend(pn::Providers{});
    m = press(std::move(m), Msg{ProvidersToggleAll{}});          // expand
    m = press(std::move(m), Msg{ProvidersJump{ProvidersJump::Where::End}});
    m = press(std::move(m), Msg{ProvidersToggleAll{}});          // collapse

    auto* p = m.ui.panel.get<pn::Providers>();
    REQUIRE(p != nullptr);
    CHECK_FALSE(p->show_all);
    const int n = static_cast<int>(
        ui::build_sectioned_provider_rows(
            {}, "", m.d.persisted, m.d.auth,
            m.d.selection.catalog_id(), false).rows.size());
    CHECK(p->index >= 0);
    CHECK(p->index < n);
}

TEST_CASE("provider picker: each section header appears at most once") {
    // Rows must be emitted in SECTION order. Saved hosts and ACP agents are
    // tagged YOURS but were once appended AFTER the not-configured band,
    // which made the YOURS divider render a second time mid-list.
    auto count_of = [](const std::string& s, const std::string& needle) {
        int n = 0;
        for (std::size_t at = 0; (at = s.find(needle, at)) != std::string::npos; ++at)
            ++n;
        return n;
    };

    // Collapsed, expanded, and searching -- all three assemble the list.
    {
        Model m = picker_model();
        m.d.persisted.provider_keys["my-box:8080"] = "k";
        const std::string s = render_panel(m, 110, 400);
        INFO(s);
        CHECK(count_of(s, "YOURS") == 1);
        // The saved custom host is in YOURS, above any later band.
        const auto host = s.find("my-box");
        if (host != std::string::npos) {
            const auto add = s.find("ADD");
            if (add != std::string::npos) CHECK(host < add);
        }
    }
    {
        Model m = picker_model();
        m.d.persisted.provider_keys["my-box:8080"] = "k";
        m.ui.panel.get<pn::Providers>()->show_all = true;
        const std::string s = render_panel(m, 110, 600);
        INFO(s);
        CHECK(count_of(s, "YOURS") == 1);
        CHECK(count_of(s, "NOT CONFIGURED") == 1);
        // Every YOURS row precedes the not-configured divider.
        const auto host = s.find("my-box");
        const auto notc = s.find("NOT CONFIGURED");
        REQUIRE(host != std::string::npos);
        REQUIRE(notc != std::string::npos);
        CHECK(host < notc);
    }
}

TEST_CASE("provider picker: the active provider leads YOURS even unauthed") {
    // You are running on it, so the ● marker belongs in YOURS -- but the
    // badge must still tell the truth about the missing credential, and it
    // must not render as a dim "Enter to set up" offer.
    Model m = picker_model();
    const auto rows = ui::build_sectioned_provider_rows(
        {}, "", m.d.persisted, m.d.auth, "anthropic", false);
    REQUIRE(!rows.rows.empty());
    const auto& first = rows.rows.front();
    const auto* p = first.row.preset();
    REQUIRE(p != nullptr);
    CHECK(std::string{p->id} == "anthropic");
    CHECK(first.section == ui::ProviderSection::Yours);
    CHECK_FALSE(first.offer);          // never "Enter to set up"
}

TEST_CASE("provider picker: a configured provider is promoted into YOURS") {
    Model m = picker_model();
    m.d.persisted.provider_keys["groq"] = "gsk_test";
    const std::string screen = render_panel(m);
    INFO(screen);

    CHECK(has(screen, "YOURS"));
    CHECK(has(screen, "Groq"));
    // Still hidden: configuring one vendor must not unhide its neighbours.
    CHECK_FALSE(has(screen, "Cerebras"));
}

TEST_CASE("provider picker: search reaches a provider browse hides") {
    // The no-dead-end contract. Hiding is a BROWSE decision; typing a name
    // must still find it, or we've made the feature unreachable.
    Model m = picker_model("cerebras");
    const std::string screen = render_panel(m);
    INFO(screen);

    CHECK(has(screen, "Cerebras"));
    // It renders as an offer with its single action, not as a key warning.
    CHECK(has(screen, "Enter to set up"));
    // The signpost belongs to the browse view only.
    CHECK_FALSE(has(screen, "more providers"));
}

TEST_CASE("provider picker: configured rows lead the unconfigured in search") {
    Model m = picker_model("e");            // matches many vendors
    m.d.persisted.provider_keys["cerebras"] = "k";
    const std::string screen = render_panel(m);
    INFO(screen);

    const auto configured = screen.find("CONFIGURED");
    const auto notconf    = screen.find("NOT CONFIGURED");
    REQUIRE(configured != std::string::npos);
    if (notconf != std::string::npos)
        CHECK(configured < notconf);

    // Cerebras is the configured one, so it sorts above the band divider.
    const auto cer = screen.find("Cerebras");
    REQUIRE(cer != std::string::npos);
    if (notconf != std::string::npos) CHECK(cer < notconf);
}

// ── Multi-account surfacing ──────────────────────────────────────────────
//
// Entitlement is account-scoped, so two accounts on one provider can serve
// DIFFERENT model lists. The identity is named on the picker's section
// header (and the status badge), never per row: the account is not part of
// what a model IS. See docs/IDENTITY_CAPABILITY_ENTITLEMENT.md.

TEST_CASE("model picker: footer hints describe the HIGHLIGHTED row") {
    // A hint is a promise about what the key will do NOW. ^F is a toggle,
    // so on a row that is already starred it unstars -- saying "favorite"
    // there describes the state instead of the action. And a sign-in offer
    // has no model to star at all, so the hint must go rather than name an
    // action that silently does nothing.
    auto render = [](const Model& mm) {
        auto root = ui::models_panel(mm);
        maya::StylePool pool;
        maya::Canvas canvas(120, 200, &pool);
        canvas.clear();
        maya::render_tree(root, canvas, pool, maya::theme::native, true);
        std::string s;
        for (int y = 0; y <= canvas.max_content_row(); ++y) {
            for (int x = 0; x < 120; ++x) {
                char32_t ch = canvas.get(x, y).character;
                s.push_back(ch && ch < 128 ? static_cast<char>(ch) : ' ');
            }
            s.push_back('\n');
        }
        return s;
    };

    {   // A plain row: ^F adds a favorite.
        Model m;
        m.ui.panel.descend(pn::Models{});
        FusedRow r;
        r.provider_id = "anthropic";
        r.label       = "Anthropic";
        r.model.id    = ModelId{"claude-opus-4-5"};
        r.authed      = true;
        m.d.fused_rows.push_back(r);
        const std::string s = render(m);
        INFO(s);
        CHECK(has(s, "favorite"));
        CHECK_FALSE(has(s, "unfavorite"));
    }
    {   // Already a favorite: the same key REMOVES it.
        Model m;
        m.ui.panel.descend(pn::Models{});
        FusedRow r;
        r.provider_id    = "anthropic";
        r.label          = "Anthropic";
        r.model.id       = ModelId{"claude-opus-4-5"};
        r.model.favorite = true;
        r.authed         = true;
        m.d.fused_rows.push_back(r);
        const std::string s = render(m);
        INFO(s);
        CHECK(has(s, "unfavorite"));
    }
}

TEST_CASE("model picker: a scoped list never claims to search all providers") {
    // The title said "Anthropic only" while the search box invited you to
    // filter "across providers" -- two contradictory claims on one screen.
    // And ^/ is the way back out, so its hint must say so or a narrowed
    // list looks like a trap.
    Model m;
    m.ui.panel.descend(pn::Models{});
    auto* mp = m.ui.panel.get<pn::Models>();
    REQUIRE(mp != nullptr);

    FusedRow r;
    r.provider_id = "anthropic";
    r.label       = "Anthropic";
    r.model.id    = ModelId{"claude-opus-4-5"};
    r.authed      = true;
    m.d.fused_rows.push_back(r);

    auto render = [&] {
        auto root = ui::models_panel(m);
        maya::StylePool pool;
        maya::Canvas canvas(120, 200, &pool);
        canvas.clear();
        maya::render_tree(root, canvas, pool, maya::theme::native, true);
        std::string s;
        for (int y = 0; y <= canvas.max_content_row(); ++y) {
            for (int x = 0; x < 120; ++x) {
                char32_t ch = canvas.get(x, y).character;
                s.push_back(ch && ch < 128 ? static_cast<char>(ch) : ' ');
            }
            s.push_back('\n');
        }
        return s;
    };

    {   // Unscoped: the cross-provider promise is true, keep it.
        const std::string s = render();
        INFO(s);
        CHECK(has(s, "across providers"));
    }
    {   // Scoped: it must not claim to reach past the one provider, and the
        // footer must name the way back.
        mp->provider_scope = "anthropic";
        const std::string s = render();
        INFO(s);
        CHECK_FALSE(has(s, "across providers"));
        CHECK(has(s, "this provider"));
        CHECK(has(s, "all providers"));      // ^/ hint, the escape hatch
    }
}

TEST_CASE("model picker: the scoped header names the active account") {
    Model m;
    m.ui.panel.descend(pn::Models{});
    auto* mp = m.ui.panel.get<pn::Models>();
    REQUIRE(mp != nullptr);
    mp->provider_scope = "anthropic";

    m.d.auth.loaded = true;
    m.d.auth.providers["anthropic"] = auth::ProviderAuth{
        .stored = true, .accounts = {"work", "home"}, .active_account = "work"};

    FusedRow r;
    r.provider_id = "anthropic";
    r.label       = "Anthropic";
    r.model.id    = ModelId{"claude-opus-4-5"};
    r.model.display_name = "Claude Opus 4.5";
    r.authed      = true;
    m.d.fused_rows.push_back(r);

    auto root = ui::models_panel(m);
    maya::StylePool pool;
    maya::Canvas canvas(120, 200, &pool);
    canvas.clear();
    maya::render_tree(root, canvas, pool, maya::theme::native, true);
    std::string screen;
    for (int y = 0; y <= canvas.max_content_row(); ++y) {
        for (int x = 0; x < 120; ++x) {
            char32_t ch = canvas.get(x, y).character;
            screen.push_back(ch && ch < 128 ? static_cast<char>(ch) : ' ');
        }
        screen.push_back('\n');
    }
    INFO(screen);

    // The header carries the identity whose catalog this is.
    CHECK(has(screen, "WORK"));
    // The model row itself stays clean — the account is named once, on the
    // header, not repeated down the list where it would steal width.
    const auto hdr = screen.find("ANTHROPIC MODELS");
    REQUIRE(hdr != std::string::npos);
    const auto hdr_eol = screen.find('\n', hdr);
    // Exactly one WORK on screen, and it's on the header line.
    CHECK(screen.find("WORK") < hdr_eol);
    CHECK(screen.find("WORK", hdr_eol) == std::string::npos,
          "the account appears once, on the header — not per model row");
}

TEST_CASE("model picker: a single-account provider adds no chrome") {
    Model m;
    m.ui.panel.descend(pn::Models{});
    auto* mp = m.ui.panel.get<pn::Models>();
    REQUIRE(mp != nullptr);
    mp->provider_scope = "anthropic";

    m.d.auth.loaded = true;
    m.d.auth.providers["anthropic"] = auth::ProviderAuth{
        .stored = true, .accounts = {"work"}, .active_account = "work"};

    FusedRow r;
    r.provider_id = "anthropic";
    r.label       = "Anthropic";
    r.model.id    = ModelId{"claude-opus-4-5"};
    r.model.display_name = "Claude Opus 4.5";
    r.authed      = true;
    m.d.fused_rows.push_back(r);

    auto root = ui::models_panel(m);
    maya::StylePool pool;
    maya::Canvas canvas(120, 200, &pool);
    canvas.clear();
    maya::render_tree(root, canvas, pool, maya::theme::native, true);
    std::string screen;
    for (int y = 0; y <= canvas.max_content_row(); ++y) {
        for (int x = 0; x < 120; ++x) {
            char32_t ch = canvas.get(x, y).character;
            screen.push_back(ch && ch < 128 ? static_cast<char>(ch) : ' ');
        }
        screen.push_back('\n');
    }
    INFO(screen);

    // One account is no choice at all — naming it would be noise.
    CHECK_FALSE(has(screen, "WORK"));
}

TEST_CASE("model picker: an unused Ollama failure isn't a warning") {
    auto render_models = [](const Model& m) {
        auto root = ui::models_panel(m);
        maya::StylePool pool;
        maya::Canvas canvas(110, 120, &pool);
        canvas.clear();
        maya::render_tree(root, canvas, pool, maya::theme::native, true);
        std::string out;
        for (int y = 0; y <= canvas.max_content_row(); ++y) {
            for (int x = 0; x < 110; ++x) {
                char32_t ch = canvas.get(x, y).character;
                out.push_back(ch > 0 && ch < 128 ? static_cast<char>(ch) : ' ');
            }
            out += '\n';
        }
        return out;
    };
    auto failed = [](std::string id, std::string label) {
        ProviderCatalog c;
        c.provider_id = std::move(id);
        c.label = std::move(label);
        c.state = ProviderCatalog::State::Failed;
        c.error = "can't reach localhost: connect";
        return c;
    };

    Model m;
    m.d.selection = provider::parse_selection("openai");
    m.ui.panel.descend(pn::Models{});
    m.d.provider_catalogs = {failed("ollama", "Ollama")};
    CHECK_FALSE(has(render_models(m), "Ollama"));

    // A provider you use still warns.
    m.d.provider_catalogs.push_back(failed("groq", "Groq"));
    CHECK(has(render_models(m), "Groq"));

    // Ollama warns once it's the active provider.
    m.d.selection = provider::parse_selection("ollama");
    CHECK(has(render_models(m), "Ollama"));
}

