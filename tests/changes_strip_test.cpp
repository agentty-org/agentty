// changes_strip_test — the pending-edits banner has a compact form.
//
// With 26 files edited the bordered list took 28 rows and pushed the
// transcript off the screen; the list is the review pane's job (^R). The
// setting cycles hidden → compact (one line) → full, compact first.
#include "agtest.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <maya/print.hpp>

#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/view/changes_strip.hpp"

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

TEST_CASE("changes strip: hidden -> compact -> full -> hidden") {
    Model m = with_edits(3);
    CHECK(!m.d.show_changes_strip);
    m = toggle(std::move(m));
    CHECK(m.d.show_changes_strip);
    CHECK(!m.d.changes_strip_full);
    CHECK(m.d.persisted.show_changes_strip);
    m = toggle(std::move(m));
    CHECK(m.d.changes_strip_full);
    CHECK(m.d.persisted.changes_strip_full);
    m = toggle(std::move(m));
    CHECK(!m.d.show_changes_strip);
    CHECK(!m.d.changes_strip_full);
}

TEST_CASE("changes strip: compact is one line with the count, totals and keys") {
    Model m = with_edits(26);
    m.d.show_changes_strip = true;
    const auto cfg = ui::changes_strip_config(m);
    REQUIRE(cfg.compact);
    const std::string out = maya::render_to_string(maya::ChangesStrip{cfg}.build(), 120);
    INFO(out);
    if (std::getenv("AGT_SHOW")) std::fputs(out.c_str(), stdout);
    CHECK(rows_of(out) == 1);
    CHECK(out.find("26 files") != std::string::npos);
    CHECK(out.find("+260") != std::string::npos);
    CHECK(out.find("104") != std::string::npos);
    CHECK(out.find("^R") != std::string::npos);
    CHECK(out.find("file_3.go") == std::string::npos);   // no per-file rows

    // Narrow: the count and totals stay, the key hints give way.
    const std::string narrow = maya::render_to_string(maya::ChangesStrip{cfg}.build(), 30);
    CHECK(rows_of(narrow) == 1);
    CHECK(narrow.find("26 files") != std::string::npos);
}

TEST_CASE("changes strip: the full list caps at 8 files and points at review") {
    Model m = with_edits(26);
    m.d.show_changes_strip = true;
    m.d.changes_strip_full = true;
    const auto cfg = ui::changes_strip_config(m);
    REQUIRE(!cfg.compact);
    const std::string out = maya::render_to_string(maya::ChangesStrip{cfg}.build(), 120);
    INFO(out);
    if (std::getenv("AGT_SHOW")) std::fputs(out.c_str(), stdout);
    CHECK(out.find("file_7.go") != std::string::npos);
    CHECK(out.find("file_8.go") == std::string::npos);
    CHECK(out.find("18 more") != std::string::npos);
    CHECK(rows_of(out) < 16);
}
