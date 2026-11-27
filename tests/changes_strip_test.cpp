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

TEST_CASE("changes strip: one row with the count, totals and keys") {
    Model m = with_edits(26);
    m.d.show_changes_strip = true;
    const auto cfg = ui::changes_strip_config(m);
    const std::string out = maya::render_to_string(maya::ChangesStrip{cfg}.build(), 120);
    INFO(out);
    CHECK(rows_of(out) == 1);
    CHECK(out.find("26 files changed") != std::string::npos);
    CHECK(out.find("+260 \xe2\x88\x92" "104") != std::string::npos);
    CHECK(out.find("^R review") != std::string::npos);
    CHECK(out.find("^A keep all") != std::string::npos);   // the review pane's words
    CHECK(out.find("^X undo all") != std::string::npos);
    CHECK(out.find("\xe2\x96\x8e") == std::string::npos);  // no ▎ stub on the border
    CHECK(out.find("file_3.go") == std::string::npos);     // no per-file rows

    // Narrow: undo, then keep, then "changed" give way; never wraps.
    const std::string narrow = maya::render_to_string(maya::ChangesStrip{cfg}.build(), 44);
    INFO(narrow);
    CHECK(rows_of(narrow) == 1);
    CHECK(narrow.find("26 files") != std::string::npos);
    CHECK(narrow.find("+260") != std::string::npos);
    CHECK(narrow.find("^R review") != std::string::npos);
    CHECK(narrow.find("undo all") == std::string::npos);

    const std::string tiny = maya::render_to_string(maya::ChangesStrip{cfg}.build(), 20);
    INFO(tiny);
    CHECK(rows_of(tiny) == 1);
    CHECK(tiny.find("26 files") != std::string::npos);
}

TEST_CASE("changes strip: sits as one row between the thread and the composer") {
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
    // The very next row is the composer's top border (╭).
    const auto eol = frame.find('\n', at);
    REQUIRE(eol != std::string::npos);
    const auto next_eol = frame.find('\n', eol + 1);
    CHECK(frame.substr(eol + 1, next_eol - eol - 1).find("\xe2\x95\xad") != std::string::npos);
}
