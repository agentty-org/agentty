// changes_strip_test — the pending-edits strip: shown or hidden.
//
// With 26 files edited the bordered list took 28 rows and pushed the
// transcript off the screen. Shown, it is ONE row in the composer's hint
// grammar (bold key, dim label, " · "): how many files, +/−, and the keys.
// The per-file list is the review pane's job (^R).
#include "agtest.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <maya/print.hpp>
#include <maya/core/render_context.hpp>

#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/view/changes_strip.hpp"
#include "agentty/runtime/view/diff_review.hpp"
#include "agentty/diff/diff.hpp"
#include "agentty/runtime/panel/top.hpp"
#include "agentty/runtime/view/view.hpp"

using namespace agentty;

namespace {

Model with_edits(int n) {
    Model m;
    for (int i = 0; i < n; ++i) {
        FileChange c;
        c.path = "internal/server/file_" + std::to_string(i) + ".go";
        c.original_contents = "x";
        c.added = 10;
        c.removed = 4;
        m.d.pending_changes.push_back(std::move(c));
    }
    return m;
}

int rows_of(const std::string& s) {
    int n = 0;
    for (std::size_t i = 0; i < s.size(); ++i) n += s[i] == '\n';
    return n + (!s.empty() && s.back() != '\n');
}

Model toggle(Model m) {
    auto [next, cmd] = app::update(std::move(m), Msg{ToggleChangesStrip{}});
    (void)cmd;
    return std::move(next);
}

}  // namespace

TEST_CASE("changes strip: the setting is show / hide") {
    Model m = with_edits(3);
    CHECK(!m.d.show_changes_strip);
    m = toggle(std::move(m));
    CHECK(m.d.show_changes_strip);
    CHECK(m.d.persisted.show_changes_strip);
    CHECK(ui::changes_strip_config(m).compact);
    m = toggle(std::move(m));
    CHECK(!m.d.show_changes_strip);
    CHECK(!m.d.persisted.show_changes_strip);
    CHECK(ui::changes_strip_config(m).changes.empty());   // nothing drawn
}

TEST_CASE("changes strip: one row in a box with the count, totals and keys") {
    Model m = with_edits(26);
    m.d.show_changes_strip = true;
    const auto cfg = ui::changes_strip_config(m);
    const std::string out = maya::render_to_string(maya::ChangesStrip{cfg}.build(), 120);
    INFO(out);
    CHECK(rows_of(out) == 3);                               // ╭, one row, ╰
    CHECK(out.find("\xe2\x95\xad") != std::string::npos);   // ╭
    CHECK(out.find("\xe2\x95\xb0") != std::string::npos);   // ╰
    CHECK(out.find("26 files changed") != std::string::npos);
    CHECK(out.find("+260 \xe2\x88\x92" "104") != std::string::npos);
    CHECK(out.find("^R review") != std::string::npos);
    CHECK(out.find("^A keep all") != std::string::npos);   // the review pane's words
    CHECK(out.find("^X undo all") != std::string::npos);
    CHECK(out.find("\xe2\x96\x8e") == std::string::npos);  // no ▎ stub on the border
    CHECK(out.find("file_3.go") == std::string::npos);     // no per-file rows
    {   // keys flush right: "undo all" ends right before the padding + │
        const auto l1 = out.find('\n');
        const std::string row = out.substr(l1 + 1, out.find('\n', l1 + 1) - l1 - 1);
        CHECK(row.find("undo all \xe2\x94\x82") != std::string::npos);
        CHECK(row.find("104   ") != std::string::npos);   // a gap between the halves
    }

    // Narrow: undo, then keep, then "changed" give way; never wraps.
    const std::string narrow = maya::render_to_string(maya::ChangesStrip{cfg}.build(), 44);
    INFO(narrow);
    CHECK(rows_of(narrow) == 3);
    CHECK(narrow.find("26 files") != std::string::npos);
    CHECK(narrow.find("+260") != std::string::npos);
    CHECK(narrow.find("^R review") != std::string::npos);
    CHECK(narrow.find("undo all") == std::string::npos);

    const std::string tiny = maya::render_to_string(maya::ChangesStrip{cfg}.build(), 20);
    INFO(tiny);
    CHECK(rows_of(tiny) == 3);
    CHECK(tiny.find("26 files") != std::string::npos);
}

TEST_CASE("changes strip: a one-row box between the thread and the composer") {
    Model m = with_edits(12);
    m.d.show_changes_strip = true;
    constexpr int W = 100, H = 24;
    maya::RenderContext ctx{W, H, maya::render_generation(), /*auto_height=*/true};
    maya::RenderContextGuard guard(ctx);
    const std::string frame = maya::render_to_string(ui::view(m), W);
    INFO(frame);
    if (std::getenv("AGT_SHOW")) std::fputs(frame.c_str(), stdout);
    const auto at = frame.find("12 files changed");
    REQUIRE(at != std::string::npos);
    // Below it: the strip's own bottom edge (╰), then the composer (╭).
    const auto eol = frame.find('\n', at);
    REQUIRE(eol != std::string::npos);
    const auto eol2 = frame.find('\n', eol + 1);
    const auto eol3 = frame.find('\n', eol2 + 1);
    CHECK(frame.substr(eol + 1, eol2 - eol - 1).find("\xe2\x95\xb0") != std::string::npos);
    CHECK(frame.substr(eol2 + 1, eol3 - eol2 - 1).find("\xe2\x95\xad") != std::string::npos);
}

// The review pane (^R) is bounded by Settings → Appearance → Panel height,
// however big the change: a file with 40 separate changes listed one row
// each, and a 300-line change, used to make a pane taller than the screen.
TEST_CASE("review pane: height follows Panel height, never the size of the diff") {
    std::string before, many, huge;
    for (int i = 0; i < 400; ++i) {
        before += "line " + std::to_string(i) + "\n";
        many   += (i % 10 == 5 ? "CHANGED " : "line ") + std::to_string(i) + "\n";
    }
    huge = before;
    for (int i = 0; i < 300; ++i) huge.insert(0, "added " + std::to_string(i) + "\n");

    auto height = [&](const std::string& after, ui_prefs::Density d, int term_h) {
        Model m;
        m.d.ui().density = d;
        m.d.pending_changes.push_back(diff::compute("src/big.go", before, after));
        m.ui.panel.descend(ui::panel::DiffReview{{0, 0}});
        maya::RenderContext ctx{100, term_h, maya::render_generation(), /*auto_height=*/true};
        maya::RenderContextGuard guard(ctx);
        const std::string out = maya::render_to_string(ui::diff_review(m), 100);
        if (std::getenv("AGT_SHOW")) std::fputs(out.c_str(), stdout);
        return rows_of(out);
    };
    using D = ui_prefs::Density;
    for (const auto* after : {&many, &huge}) {
        const int compact = height(*after, D::Compact, 200);
        const int normal  = height(*after, D::Normal,  200);
        const int tall    = height(*after, D::Tall,    200);
        INFO("compact=" << compact << " normal=" << normal << " tall=" << tall);
        CHECK(compact < normal);                // the setting decides
        CHECK(normal  < tall);
        CHECK(tall    <= 28 + 9 + 22);          // body + change list + chrome
        CHECK(height(*after, D::Tall, 30) <= 30);   // and the terminal caps it
        CHECK(height(*after, D::Normal, 26) <= 26);
    }
}
