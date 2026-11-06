// queued_preview_test — a queued message says WHEN it will be sent.
//
// Steering mid-task is what the queue is for: type while the agent works,
// and the message goes out the moment the turn ends. The cues used to be a
// `❚ N queued` count and a transcript row that looks exactly like a sent
// message, so pressing Enter mid-turn read as "it sent" or "nothing
// happened" rather than "this fires when the turn finishes".
//
// Rendered through the real view, because the claim is about what reaches
// the screen; a source-text assertion could not tell you the line is
// actually visible.

#include "agtest.hpp"

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/view/thread/conversation.hpp"

#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>

#include <string>

using namespace agentty;

namespace {

// Non-ASCII folds to a space: the line under test is ASCII, and the chrome
// around it is not what is being asserted.
std::string render(const Model& m, int width = 100) {
    auto root = maya::Conversation{ui::conversation_config(m)}.build();
    maya::StylePool pool;
    maya::Canvas canvas(width, 60, &pool);
    canvas.clear();
    maya::render_tree(root, canvas, pool, maya::theme::native, true);
    std::string s;
    for (int y = 0; y <= canvas.max_content_row(); ++y) {
        for (int x = 0; x < width; ++x) {
            char32_t ch = canvas.get(x, y).character;
            s.push_back(ch && ch < 128 ? static_cast<char>(ch) : ' ');
        }
        s.push_back('\n');
    }
    return s;
}

Model with_queued(std::string text) {
    Model m;
    ComposerState::QueuedMessage q;
    q.text = std::move(text);
    m.ui.composer.queued.push_back(std::move(q));
    return m;
}

}  // namespace

TEST_CASE("queued preview: mid-turn, it says when it sends") {
    Model m = with_queued("actually, use the other approach");
    m.s.phase = phase::Streaming{};
    REQUIRE(m.s.active());

    const std::string s = render(m);
    INFO(s);
    CHECK(s.find("queued #1") != std::string::npos);
    CHECK(s.find("sends when this turn ends") != std::string::npos);
}

TEST_CASE("queued preview: idle, it does not promise a turn that isn't running") {
    // Nothing is streaming, so there is no "this turn" to end -- the queue
    // drains on the next submit instead. Saying otherwise would be a lie,
    // and a hint that is sometimes wrong is worse than no hint.
    Model m = with_queued("later");
    REQUIRE(!m.s.active());

    const std::string s = render(m);
    INFO(s);
    CHECK(s.find("queued #1") != std::string::npos);
    CHECK(s.find("sends when this turn ends") == std::string::npos);
}

TEST_CASE("queued preview: the promise is made once, not per row") {
    // Three queued items state it once. Repeating it on every row is
    // nagging, and the rows are adjacent so one line covers them all.
    Model m = with_queued("first");
    for (const char* t : {"second", "third"}) {
        ComposerState::QueuedMessage q;
        q.text = t;
        m.ui.composer.queued.push_back(std::move(q));
    }
    m.s.phase = phase::Streaming{};

    const std::string s = render(m);
    INFO(s);
    CHECK(s.find("queued #1") != std::string::npos);
    CHECK(s.find("queued #3") != std::string::npos);
    const auto first = s.find("sends when this turn ends");
    REQUIRE(first != std::string::npos);
    CHECK(s.find("sends when this turn ends", first + 1) == std::string::npos);
}
