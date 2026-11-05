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
    CHECK(has(screen, "type to search"));

    // Providers you haven't set up are not in the browse list.
    CHECK_FALSE(has(screen, "Cerebras"));
    CHECK_FALSE(has(screen, "Fireworks"));

    // "Custom host…" stays reachable: it's the one row that always applies.
    CHECK(has(screen, "ADD"));
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

