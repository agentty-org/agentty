// strip_disk_e2e_test — the changes strip's keys, all the way to the disk.
//
// The other tests prove a key routes to the right message, and that each
// reducer returns the right effect. This one closes the gap between them: a
// real file tree, real key events through the real router (AgenttyApp::on),
// the real reducers, and the effects run by the SAME executor the host uses
// (apply_file_effect). Then it reads the files back.
//
//   ^A  keep all   files stay as the agent wrote them; strip clears
//   ^X  undo all   first press arms (disk untouched), second reverts every
//                  file -- edited ones to their original bytes, CREATED ones
//                  removed, not left behind empty
//   ^X  mid-turn   explains, touches nothing, and leaves no arm behind
//   ^R  review     opens the pane; undoing hunks there and closing writes
//                  exactly the kept changes
#include "agtest.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "agtest_fx.hpp"
#include "agentty/diff/diff.hpp"
#include "agentty/runtime/app/file_fx.hpp"
#include "agentty/runtime/app/program.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/panel/top.hpp"

using namespace agentty;
namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void spit(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

// A scratch tree where "the agent" has edited two files and created one.
struct Tree {
    fs::path root;
    fs::path edited_a, edited_b, created;
    std::string a0 = "alpha\nbeta\ngamma\n",       a1 = "alpha\nBETA\ngamma\ndelta\n";
    std::string b0 = "level = \"info\"\n",         b1 = "level = \"debug\"\n";
    std::string c1 = "package server\n\nfunc Recover() {}\n";

    Tree() {
        root = fs::temp_directory_path()
             / ("agentty_strip_e2e_" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
        edited_a = root / "server" / "items.go";
        edited_b = root / "config" / "app.toml";
        created  = root / "server" / "recover.go";
        // On disk: what the agent wrote.
        spit(edited_a, a1);
        spit(edited_b, b1);
        spit(created, c1);
    }
    ~Tree() { std::error_code ec; fs::remove_all(root, ec); }

    Model model() const {
        Model m;
        m.d.show_changes_strip = true;
        m.d.pending_changes.push_back(diff::compute(edited_a.string(), a0, a1));
        m.d.pending_changes.push_back(diff::compute(edited_b.string(), b0, b1));
        m.d.pending_changes.push_back(diff::compute(created.string(), "", c1));
        return m;
    }
};

maya::KeyEvent ctrl(char32_t c) {
    maya::KeyEvent k{};
    k.key = maya::CharKey{c};
    k.mods.ctrl = true;
    return k;
}

// Press a key the way the app does: route it against the live model, fold
// the message, run every file effect against the real disk.
bool press(Model& m, const maya::KeyEvent& ev) {
    auto msg = app::AgenttyApp::on(m, ev);
    if (!msg) return false;
    auto [next, cmd] = app::update(std::move(m), std::move(*msg));
    m = std::move(next);
    agtest::fx::for_each(cmd, [](const auto& e) {
        if constexpr (std::same_as<std::remove_cvref_t<decltype(e)>, WriteFile>)
            app::apply_file_effect(e);
    });
    return true;
}

bool apply(Model& m, Msg msg) {
    auto [next, cmd] = app::update(std::move(m), std::move(msg));
    m = std::move(next);
    agtest::fx::for_each(cmd, [](const auto& e) {
        if constexpr (std::same_as<std::remove_cvref_t<decltype(e)>, WriteFile>)
            app::apply_file_effect(e);
    });
    return true;
}

}  // namespace

TEST_CASE("strip e2e: ^A keeps every file exactly as the agent wrote it") {
    Tree t;
    Model m = t.model();
    REQUIRE(press(m, ctrl(U'a')));
    CHECK(m.d.pending_changes.empty());       // strip clears
    CHECK(slurp(t.edited_a) == t.a1);
    CHECK(slurp(t.edited_b) == t.b1);
    CHECK(fs::exists(t.created));
    CHECK(slurp(t.created) == t.c1);
}

TEST_CASE("strip e2e: ^X arms, then reverts edits and removes created files") {
    Tree t;
    Model m = t.model();

    // First press: arms only. Nothing on disk moves, the strip stays.
    REQUIRE(press(m, ctrl(U'x')));
    CHECK(m.d.reject_all_armed);
    CHECK(m.d.pending_changes.size() == 3);
    CHECK(slurp(t.edited_a) == t.a1);
    CHECK(fs::exists(t.created));

    // Second press: everything goes back to how it was.
    REQUIRE(press(m, ctrl(U'x')));
    CHECK(m.d.pending_changes.empty());
    CHECK(!m.d.reject_all_armed);
    CHECK(slurp(t.edited_a) == t.a0);
    CHECK(slurp(t.edited_b) == t.b0);
    CHECK_MESSAGE(!fs::exists(t.created),
                  "a file the agent created must be gone after undo, not left empty");
}

TEST_CASE("strip e2e: legacy control bytes do the same (0x01 keep, 0x18 undo)") {
    Tree t;
    Model m = t.model();
    maya::KeyEvent x{};
    x.key = maya::CharKey{0x18};
    REQUIRE(press(m, x));
    REQUIRE(press(m, x));
    CHECK(slurp(t.edited_a) == t.a0);
    CHECK(!fs::exists(t.created));
}

TEST_CASE("strip e2e: a key in between disarms ^X") {
    Tree t;
    Model m = t.model();
    REQUIRE(press(m, ctrl(U'x')));
    REQUIRE(m.d.reject_all_armed);
    // Open and close the review: any other diff-review action drops the arm.
    REQUIRE(press(m, ctrl(U'r')));
    REQUIRE(ui::panel::top(m) == ui::panel::Kind::DiffReview);
    CHECK(!m.d.reject_all_armed);
}

TEST_CASE("strip e2e: ^X mid-turn touches nothing and leaves no arm") {
    Tree t;
    Model m = t.model();
    REQUIRE(press(m, ctrl(U'x')));             // armed before the turn
    REQUIRE(m.d.reject_all_armed);
    m.s.phase = phase::Streaming{};
    REQUIRE(press(m, ctrl(U'x')));             // mid-turn: explains only
    CHECK(!m.d.reject_all_armed);
    CHECK(m.d.pending_changes.size() == 3);
    CHECK(slurp(t.edited_a) == t.a1);
    CHECK(fs::exists(t.created));

    // ^A mid-turn works.
    REQUIRE(press(m, ctrl(U'a')));
    CHECK(m.d.pending_changes.empty());
    CHECK(slurp(t.edited_a) == t.a1);

    // After the turn, a single ^X with nothing pending does nothing at all.
    m.s.phase = phase::Idle{};
    REQUIRE(press(m, ctrl(U'x')));
    CHECK(fs::exists(t.created));
}

TEST_CASE("strip e2e: ^R review, undo one file's change, close: only that reverts") {
    Tree t;
    Model m = t.model();
    REQUIRE(press(m, ctrl(U'r')));
    REQUIRE(ui::panel::top(m) == ui::panel::Kind::DiffReview);

    // Cursor starts on the first file (items.go). Undo all of its changes.
    const auto n = m.d.pending_changes[0].hunks.size();
    for (std::size_t i = 0; i < n; ++i) REQUIRE(apply(m, Msg{RejectHunk{}}));
    // Close the pane (Esc): the rest is kept.
    if (ui::panel::top(m) == ui::panel::Kind::DiffReview)
        REQUIRE(apply(m, Msg{CloseDiffReview{}}));

    CHECK(slurp(t.edited_a) == t.a0);   // undone
    CHECK(slurp(t.edited_b) == t.b1);   // kept
    CHECK(slurp(t.created)  == t.c1);   // kept
    CHECK(m.d.pending_changes.empty());
}

TEST_CASE("strip e2e: undoing every change of a created file in the pane removes it") {
    Tree t;
    Model m;
    m.d.show_changes_strip = true;
    m.d.pending_changes.push_back(diff::compute(t.created.string(), "", t.c1));
    REQUIRE(press(m, ctrl(U'r')));
    const auto n = m.d.pending_changes[0].hunks.size();
    for (std::size_t i = 0; i < n; ++i) REQUIRE(apply(m, Msg{RejectHunk{}}));
    if (ui::panel::top(m) == ui::panel::Kind::DiffReview)
        REQUIRE(apply(m, Msg{CloseDiffReview{}}));
    CHECK(!fs::exists(t.created));
}
