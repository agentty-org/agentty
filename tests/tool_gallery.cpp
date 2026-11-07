// tool_gallery — every tool card, rendered through the REAL view, from REAL
// recorded tool calls.
//
// WHAT THIS IS
// ============
// A fixture, not a test. It replays a corpus of actual tool calls lifted from
// recorded threads, builds a real Model per case, and renders each through
// conversation_config -> maya::Conversation. What it prints is what a user
// sees, in colour, at a real width.
//
// WHY A CORPUS AND NOT HAND-WRITTEN CASES
// =======================================
// The first version of this file invented its fixtures, and invented
// fixtures are POLITE: a 6-line read, a 2-hit grep, a tidy diff. Real tool
// output is not. In 60 recorded threads the actual shape is:
//
//     bash      20077 calls   191 failed   up to  30 KB of output
//     edit      10045 calls   408 failed   up to  23 KB
//     read       7775 calls   144 failed   up to  80 KB
//     grep       3035 calls    40 failed   up to  29 KB
//
// A card design that looks good on a 6-line read and falls apart on an 80 KB
// one is not a design. So the corpus keeps, per tool, the TYPICAL call (the
// median by output size -- what a user stares at all day), a LARGE one (93rd
// percentile), and a FAILURE where the tool has any.
//
// The corpus is scrubbed: home paths, emails and unrelated project names are
// rewritten, because it came from real sessions.
//
// Run:  ./build/tool_gallery                 # every case
//       ./build/tool_gallery read grep       # tools matching these
//       ./build/tool_gallery --kind failed   # only the failures
//       ./build/tool_gallery --running bash  # as in-flight cards
//       ./build/tool_gallery --list          # what is in the corpus
//       COLS=120 ./build/tool_gallery        # at another width

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/view/thread/conversation.hpp"
#include "agentty/domain/ui_theme.hpp"

#include <maya/runtime.hpp>
#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace agentty;
using json = nlohmann::json;

// --running: render every card as an in-flight tool (shimmer, spinner).
static bool g_running = false;

namespace {

struct Case {
    std::string tool;     // "read", "bash", …
    std::string kind;     // "typical" | "large" | "failed"
    json        args;
    std::string output;
    bool        failed = false;
};

std::vector<Case> load_corpus(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr,
            "tool_gallery: cannot open %s\n"
            "  The corpus ships at tests/fixtures/tool_corpus.json; pass a\n"
            "  different one as $TOOL_CORPUS.\n", path.c_str());
        return {};
    }
    json j;
    try { f >> j; } catch (const std::exception& e) {
        std::fprintf(stderr, "tool_gallery: %s is not valid JSON: %s\n",
                     path.c_str(), e.what());
        return {};
    }
    std::vector<Case> out;
    for (const auto& r : j) {
        Case c;
        c.tool   = r.value("tool", "?");
        c.kind   = r.value("kind", "typical");
        c.args   = r.contains("args") ? r["args"] : json::object();
        c.output = r.value("output", "");
        c.failed = r.value("status", "") == "failed";
        out.push_back(std::move(c));
    }
    return out;
}

std::chrono::steady_clock::time_point now_tp() {
    return std::chrono::steady_clock::now();
}

ToolUse make_tool(const Case& c) {
    ToolUse t;
    t.id   = ToolCallId{"call_" + c.tool + "_" + c.kind};
    t.name = ToolName{c.tool};
    t.args = c.args;
    const auto started  = now_tp();
    const auto finished = started + std::chrono::milliseconds{
        // Plausible durations: a read is fast, a task is not. Only affects
        // the footer's timing chip, but a 2 ms `task` would read as fake.
        c.tool == "task" ? 42000 : c.tool == "test" ? 9100 :
        c.tool == "bash" ? 1400  : 180};
    if (g_running) {
        ToolUse::Running r;
        r.started_at = started;
        t.status = std::move(r);
    }
    else if (c.failed) t.status = ToolUse::Failed{started, finished, c.output};
    else               t.status = ToolUse::Done{started, finished, c.output};
    return t;
}

// Serialize a rendered Canvas to real ANSI.
//
// A tool card's information lives in its COLOUR -- the status glyph's hue,
// the dim chrome, the red of a failure, syntax spans. An earlier version of
// this fixture folded every cell to a bare char and printed a monochrome
// wall while believing it had colour, which hid the exact thing being
// judged.
//
// Each cell carries a style id into the pool; Style::to_sgr() resolves it
// against the live theme. A new SGR is emitted only when the id CHANGES, so
// the output has the same shape maya's own writer produces.
void emit_canvas(const maya::Canvas& canvas, const maya::StylePool& pool,
                 int cols) {
    std::string out;
    for (int y = 0; y <= canvas.max_content_row(); ++y) {
        int last = -1;
        for (int x = 0; x < cols; ++x) {
            const auto c = canvas.get(x, y);
            if (c.character != 0 && c.character != U' ') last = x;
        }
        std::uint16_t cur = 0xFFFF;
        for (int x = 0; x <= last; ++x) {
            const auto cell = canvas.get(x, y);
            if (cell.width == 2) continue;          // wide char's second half
            if (cell.style_id != cur) {
                cur = cell.style_id;
                out += "\x1b[0m";
                const auto sgr = pool.get(cur).to_sgr();
                if (!sgr.empty()) out += sgr;
            }
            const char32_t ch = cell.character ? cell.character : U' ';
            if (ch < 0x80) out += static_cast<char>(ch);
            else if (ch < 0x800) {
                out += static_cast<char>(0xC0 | (ch >> 6));
                out += static_cast<char>(0x80 | (ch & 0x3F));
            } else if (ch < 0x10000) {
                out += static_cast<char>(0xE0 | (ch >> 12));
                out += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (ch & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (ch >> 18));
                out += static_cast<char>(0x80 | ((ch >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (ch & 0x3F));
            }
        }
        out += "\x1b[0m\n";
    }
    std::fputs(out.c_str(), stdout);
}

// One short line naming what the agent was doing, so a card has the context
// a real turn gives it. Derived from the args rather than invented.
std::string prompt_for(const Case& c) {
    const auto s = [&](const char* k) -> std::string {
        if (c.args.is_object()) {
            auto it = c.args.find(k);
            if (it != c.args.end() && it->is_string()) return it->get<std::string>();
        }
        return {};
    };
    if (auto d = s("display_description"); !d.empty()) return d;
    if (auto p = s("command"); !p.empty())             return "run: " + p;
    if (auto p = s("pattern"); !p.empty())             return "find " + p;
    if (auto p = s("path"); !p.empty())                return "look at " + p;
    if (auto p = s("file_path"); !p.empty())           return "look at " + p;
    return c.tool + " (" + c.kind + ")";
}

void render_case(const Case& c, int cols) {
    Model m;
    Message user;
    user.role = Role::User;
    user.text = prompt_for(c);
    m.d.current.messages.push_back(std::move(user));

    Message asst;
    asst.role = Role::Assistant;
    asst.tool_calls.push_back(make_tool(c));
    m.d.current.messages.push_back(std::move(asst));

    auto root = maya::Conversation{ui::conversation_config(m)}.build();
    maya::StylePool pool;
    // Tall enough that a large body is never clipped by the fixture itself;
    // the card's own budget is what should bound it, and if it does not,
    // that is the finding.
    maya::Canvas canvas(cols, 400, &pool);
    canvas.clear();
    maya::render_tree(root, canvas, pool, maya::theme::live(), true);

    const std::string tag = c.tool + " \xc2\xb7 " + c.kind
                          + " \xc2\xb7 " + std::to_string(c.output.size()) + "b";
    std::printf("\n\x1b[2m\xe2\x94\x80\xe2\x94\x80 %s ", tag.c_str());
    for (int i = 0; i < cols - 6 - static_cast<int>(tag.size()); ++i)
        std::printf("\xe2\x94\x80");
    std::printf("\x1b[0m\n");

    emit_canvas(canvas, pool, cols);
}

}  // namespace

int main(int argc, char** argv) {
    // The view reads loop_bound<T> caches (the tool-body and settled-panel
    // memos in agent_timeline.cpp), which only the kernel's loop thread may
    // touch. A fixture has no kernel, so borrow the identity jaal exposes
    // for exactly this: it arms THIS thread for the scope below.
    const auto loop = maya::arm_loop_for_tests();

    // Install the theme as the LIVE one, not just pass it to render_tree.
    // Style::to_sgr() resolves a slot through theme::live(); with none set
    // every cell comes out with an empty SGR -- correct behaviour, and a
    // monochrome wall here.
    // THEME=<scheme name> (or "native"); defaults to Srcery, what Ayush runs.
    const char* theme_name = std::getenv("THEME");
    if (!theme_name) theme_name = "Srcery";
    const maya::Theme* th = ui_prefs::find_scheme(theme_name);
    maya::theme::set_live(th ? *th : maya::theme::native);

    int cols = 92;
    if (const char* c = std::getenv("COLS")) {
        const int v = std::atoi(c);
        if (v >= 40 && v <= 400) cols = v;
    }
    std::string corpus = "tests/fixtures/tool_corpus.json";
    if (const char* p = std::getenv("TOOL_CORPUS")) corpus = p;

    std::vector<std::string> want;
    std::string kind_filter;
    bool list_only = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--list") list_only = true;
        else if (a == "--running") g_running = true;
        else if (a == "--kind" && i + 1 < argc) kind_filter = argv[++i];
        else want.push_back(a);
    }

    const auto cases = load_corpus(corpus);
    if (cases.empty()) return 1;

    if (list_only) {
        std::printf("%-18s %-8s %9s\n", "tool", "kind", "bytes");
        for (const auto& c : cases)
            std::printf("%-18s %-8s %9zu\n", c.tool.c_str(), c.kind.c_str(),
                        c.output.size());
        std::printf("\n%zu cases\n", cases.size());
        return 0;
    }

    int shown = 0;
    for (const auto& c : cases) {
        if (!kind_filter.empty() && c.kind != kind_filter) continue;
        if (!want.empty()) {
            bool hit = false;
            for (const auto& w : want)
                if (c.tool.find(w) != std::string::npos) hit = true;
            if (!hit) continue;
        }
        render_case(c, cols);
        ++shown;
    }
    std::printf("\n\x1b[2m%d case%s\x1b[0m\n", shown, shown == 1 ? "" : "s");
    return 0;
}
