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
//       ./build/tool_gallery --ask bash      # with the permission prompt
//       ./build/tool_gallery --chrome        # status bar + composer
//       ./build/tool_gallery --list          # what is in the corpus
//       COLS=120 ./build/tool_gallery        # at another width

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/view/thread/conversation.hpp"
#include "agentty/runtime/view/status_bar/status_bar.hpp"
#include "agentty/runtime/view/composer.hpp"
#include "agentty/runtime/view/thread/welcome_screen.hpp"
#include "agentty/runtime/view/panels.hpp"
#include "agentty/runtime/view/view.hpp"
#include "agentty/runtime/app/update/internal.hpp"   // collect_viewer_entries
#include "agentty/runtime/panel/tool_output.hpp"
#include "agentty/runtime/view/diff_review.hpp"
#include "agentty/diff/diff.hpp"
#include "agentty/runtime/fused_models.hpp"
#include "agentty/domain/ui_theme.hpp"

#include <format>
#include <maya/runtime.hpp>
#include <maya/core/anim_clock.hpp>
#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace agentty;
using json = nlohmann::json;

// --running: render every card as an in-flight tool (shimmer, spinner).
static bool g_running = false;
// --ask: render each case waiting on a permission prompt.
static bool g_ask = false;

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
        // The recorded output stands in for live progress, so a running card
        // has a body to show (as bash and task do mid-run).
        r.progress_text = c.output;
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
    ToolUse tool = make_tool(c);
    if (g_ask) {
        tool.status = ToolUse::Pending{std::chrono::steady_clock::now()};
        m.d.pending_permission = PendingPermission{tool.id, tool.name,
                                                   "needs permission", 0};
    }
    asst.tool_calls.push_back(std::move(tool));
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

void emit_element(const std::string& tag, const maya::Element& root, int cols) {
    maya::StylePool pool;
    maya::Canvas canvas(cols, 120, &pool);
    canvas.clear();
    maya::render_tree(root, canvas, pool, maya::theme::live(), true);
    std::printf("\n\x1b[2m\xe2\x94\x80\xe2\x94\x80 %s ", tag.c_str());
    for (int i = 0; i < cols - 6 - static_cast<int>(tag.size()); ++i)
        std::printf("\xe2\x94\x80");
    std::printf("\x1b[0m\n");
    emit_canvas(canvas, pool, cols);
}

// --agents: the subagent (`task`) card in the states a user actually sees,
// with progress text in the exact shape the runner emits. The corpus has
// only settled task calls, and feeding a settled report in as "progress"
// shows nothing like a real in-flight run.
void render_agents(int cols) {
    const auto now = now_tp();
    using std::chrono::seconds;
    auto task = [&](std::string id, std::string type, std::string desc) {
        ToolUse t;
        t.id = ToolCallId{std::move(id)};
        t.name = ToolName{"task"};
        t.args = json{{"agent_type", type}, {"display_description", desc},
                      {"prompt", desc + " (full prompt elided)"}};
        return t;
    };
    auto running = [&](ToolUse t, int ago_s, std::string progress) {
        ToolUse::Running r;
        r.started_at = now - seconds{ago_s};
        r.progress_text = std::move(progress);
        r.last_progress_at = now;
        t.status = std::move(r);
        return t;
    };
    auto scene = [&](const std::string& tag, const std::string& user,
                     std::vector<ToolUse> calls) {
        Model m;
        Message u; u.role = Role::User; u.text = user;
        m.d.current.messages.push_back(std::move(u));
        Message a; a.role = Role::Assistant;
        for (auto& c : calls) a.tool_calls.push_back(std::move(c));
        m.d.current.messages.push_back(std::move(a));
        emit_element(tag, maya::Conversation{ui::conversation_config(m)}.build(), cols);
    };

    const std::string coder_feed =
        "\xe2\x97\x86 coder \xc2\xb7 turn 7/80 \xc2\xb7 9 tools \xc2\xb7 1m 12s\n"
        "  \xe2\x9c\x93 grep  minimal_splice\n"
        "  \xe2\x9c\x93 read  third_party/mcp-cpp/src/tools/fs_edit.cpp\n"
        "  \xe2\x9c\x93 edit  third_party/mcp-cpp/src/tools/fs_edit.cpp\n"
        "  \xe2\x9c\x97 shell  cmake --build /tmp/mcp-build  \xe2\x80\x94 exit 1: fs_edit.cpp:88 'repl' undeclared\n"
        "  \xe2\x9c\x93 edit  third_party/mcp-cpp/src/tools/fs_edit.cpp\n"
        "  \xe2\x9c\x93 shell  cmake --build /tmp/mcp-build  14.2s\n"
        "  \xe2\x97\x8b shell  ctest --test-dir /tmp/mcp-build -R edit\n"
        "  \xe2\x96\xb8 Build is green; running the edit tests next.";
    scene("agent \xc2\xb7 running (coder, mid-run)", "make edit splice in place",
          {running(task("t1", "coder", "Make edit splice in place"), 72, coder_feed)});

    scene("agent \xc2\xb7 running (explorer, first seconds)", "map the sandbox",
          {running(task("t2", "explorer", "Map the sandbox spawn path"), 3,
                   "\xe2\x97\x86 explorer \xc2\xb7 turn 1/32 \xc2\xb7 0 tools \xc2\xb7 3s\n")});

    scene("agent \xc2\xb7 running (retrying)", "review the PR",
          {running(task("t3", "reviewer", "Review PR #132 gate changes"), 95,
                   "\xe2\x97\x86 reviewer \xc2\xb7 turn 4/32 \xc2\xb7 6 tools \xc2\xb7 1m 35s\n"
                   "  \xe2\x9c\x93 git_diff  master...pr-132\n"
                   "  \xe2\x9c\x93 read  src/tool/util/handoff_gate.cpp\n"
                   "  \xe2\x86\xbb retry 1/3 in 2000ms (HTTP 529: overloaded)")});

    // Parallel fan-out: three agents in one turn, at different stages.
    {
        ToolUse done = task("f1", "explorer", "Map the gate");
        done.status = ToolUse::Done{now - seconds{80}, now - seconds{17},
            "Subagent report (explorer, 14 turns):\n\n**Outcome:** the gate checks shapes first, then resolved paths.\n- `handoff_gate.cpp:127` resolves symlinks\n- `handoff_gate.cpp:741` git-config targets"};
        scene("agent \xc2\xb7 fan-out (3 parallel)", "review PR #132",
              {std::move(done),
               running(task("f2", "reviewer", "Review env scope + spawn"), 80,
                       "\xe2\x97\x86 reviewer \xc2\xb7 turn 9/32 \xc2\xb7 14 tools \xc2\xb7 1m 20s\n"
                       "  \xe2\x9c\x93 read  src/domain/sandbox_provenance.cpp\n"
                       "  \xe2\x97\x8b grep  env_allowed"),
               running(task("f3", "reviewer", "Review pane + docs"), 80,
                       "\xe2\x97\x86 reviewer \xc2\xb7 turn 12/32 \xc2\xb7 19 tools \xc2\xb7 1m 20s\n"
                       "  \xe2\x96\xb8 Writing the findings now.")});
    }

    // Settled: clean success, budget-exhausted partial, provider failure.
    // ── Nested: the subagent's own calls as real tool cards ────────────
    {
        using C = ToolUse::Child;
        auto kid = [](std::string id, std::string name, json args, C::State st,
                      std::string out, std::int64_t ms) {
            return C{std::move(id), std::move(name), std::move(args), st, std::move(out), ms};
        };
        const std::string grep_out =
            "src/tools/fs_edit.cpp:48:void minimal_splice(std::string& buf,\n"
            "src/tools/fs_edit.cpp:661:            minimal_splice(buf, fm.pos, fm.len, repl);\n";
        const std::string edit_out =
            "Edited src/tools/fs_edit.cpp (3+ 2- at line 88):\n\n```diff\n--- a/src/tools/fs_edit.cpp\n+++ b/src/tools/fs_edit.cpp\n"
            "@@ -86,4 +86,5 @@\n     std::string_view matched{buf.data() + region_pos, region_len};\n"
            "-    std::size_t pre = 0;\n+    std::size_t pre = 0;   // common prefix\n+    const auto cap = repl.size();\n     const std::size_t maxpre = std::min(matched.size(), repl.size());\n```";
        ToolUse t = running(task("n1", "coder", "Make edit splice in place"), 72,
            "\xe2\x97\x86 coder \xc2\xb7 turn 7/80 \xc2\xb7 5 tools \xc2\xb7 1m 12s\n"
            "  \xe2\x9c\x93 grep  minimal_splice\n  \xe2\x96\xb8 Build is green; running the edit tests next.");
        t.children = {
            kid("c1", "grep", json{{"pattern", "minimal_splice"}}, C::State::Done, grep_out, 140),
            kid("c2", "read", json{{"path", "src/tools/fs_edit.cpp"}, {"offset", 40}, {"limit", 30}}, C::State::Done,
                "    48\tvoid minimal_splice(std::string& buf,\n    49\t                    std::size_t region_pos,\n    50\t                    std::size_t region_len,\n", 2),
            kid("c3", "edit", json{{"path", "src/tools/fs_edit.cpp"},
                                   {"edits", json::array({{{"old_text", "std::size_t pre = 0;"},
                                                           {"new_text", "std::size_t pre = 0;   // common prefix\n    const auto cap = repl.size();"}}})}},
                C::State::Done, edit_out, 4),
            kid("c4", "shell", json{{"command", "cmake --build /tmp/mcp-build"}}, C::State::Failed,
                "[subprocess failed] fs_edit.cpp:88:16: error: 'repl' was not declared in this scope\n[exit code 1]", 9100),
            kid("c5", "shell", json{{"command", "ctest --test-dir /tmp/mcp-build -R edit"}}, C::State::Running, "", 0),
        };
        scene("agent \xc2\xb7 nested (running)", "make edit splice in place", {std::move(t)});

        ToolUse d = task("n2", "explorer", "Map the gate");
        d.status = ToolUse::Done{now - seconds{40}, now,
            "Subagent report (explorer, 4 turns):\n\n**Outcome:** the gate checks shapes first, then resolved paths.\n"
            "- `handoff_gate.cpp:127` resolves symlinks\n- `handoff_gate.cpp:741` git-config targets"};
        d.children = {
            kid("d1", "grep", json{{"pattern", "is_git_config_target"}}, C::State::Done,
                "src/tool/util/handoff_gate.cpp:741:bool is_git_config_target(const fs::path& path) {\n", 90),
            kid("d2", "read", json{{"path", "src/tool/util/handoff_gate.cpp"}, {"symbol", "check_with"}}, C::State::Done,
                "   101\tVerdict check_with(std::string_view path, std::string_view tool,\n   102\t                   sandbox_cfg::HandoffPolicy policy) {\n", 3),
        };
        scene("agent \xc2\xb7 nested (done)", "map the gate", {std::move(d)});
    }

    {
        ToolUse ok = task("s1", "coder", "Add a glob test");
        ok.status = ToolUse::Done{now - seconds{8}, now,
            "Subagent report (coder, 3 turns):\n\nI added `check(m(\"docs/**\", \"docs/a/b.md\"))` at "
            "`tests/glob_path_test.cpp:35`.\n- Built `mcp_glob_path_test`: passed.\n- No other files changed."};
        scene("agent \xc2\xb7 done", "add a glob test", {std::move(ok)});

        ToolUse partial = task("s2", "explorer", "Map src/tool/util");
        partial.status = ToolUse::Failed{now - seconds{6}, now,
            "Subagent report (explorer, 2 turns):\n\n[subagent hit its turn budget without producing a final report \xe2\x80\x94 the summary below is incomplete]\n\n"
            "OUTCOME: partial. I listed 12 files but read none, so purposes are inferred from names.\n"
            "| File | Purpose |\n|---|---|\n| exec.cpp | command execution |\n| sandbox.cpp | sandbox setup |"};
        scene("agent \xc2\xb7 partial (out of turns)", "map the util dir", {std::move(partial)});

        ToolUse bad = task("s3", "reviewer", "Review the neowall diff");
        bad.status = ToolUse::Failed{now - seconds{187}, now,
            "Subagent report (reviewer, 9 turns):\n\n[subagent failed: HTTP 529: overloaded]\n\n"
            "Last steps:\n  \xe2\x9c\x93 git_diff  v0.9.0..HEAD\n  \xe2\x9c\x93 read  src/render.c\n"
            "  \xe2\x9a\xa0 stream failed 4\xc3\x97 \xe2\x80\x94 giving up: HTTP 529: overloaded"};
        scene("agent \xc2\xb7 failed (provider)", "review neowall", {std::move(bad)});
    }
}
// the states a user actually sees.
// A real reasoning block (taken from a recorded thread), settled and live,
// with the answer that followed it.
void render_reasoning(int cols) {
    static const char* kThought =
        "Since `truncate_middle` already exists, I'm planning extension-preserving "
        "truncation for FileTree filenames \xe2\x80\x94 keeping the stem shortened while "
        "preserving the extension by default, falling back to middle truncation "
        "when there's no extension or it's too long, and letting the active row "
        "reveal the full filename.\n\n"
        "I'm sketching out the config: a `FileTreeTrunc` enum (KeepExtension, "
        "Middle, End), plus fluent setters `truncate(mode)` and `reveal_active(bool)` "
        "on the FileTree class. Writing truncation dispatch logic...\n\n"
        "For the KeepExtension case, I need to preserve the extension while "
        "shortening the stem with an ellipsis, falling back to middle-truncation if "
        "the extension itself is too wide.";
    for (int live = 0; live < 2; ++live) {
        Model m;
        Message user;
        user.role = Role::User;
        user.text = "make long filenames in the file tree readable";
        m.d.current.messages.push_back(std::move(user));
        Message asst;
        asst.role = Role::Assistant;
        asst.thinking = kThought;
        asst.reasoning_ms = 6400;
        if (live) {
            // Thinking started 6.4 s ago on the same clock the view reads.
            const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                maya::anim_now().time_since_epoch()).count();
            asst.reasoning_ms = 0;
            asst.reasoning_started_ms = now_ms - 6400;
            m.s.phase = phase::Streaming{};
        } else {
            asst.text = "Added `FileTreeTrunc` with three modes. KeepExtension is the "
                        "default, so `really_long_component_name.cpp` shows as "
                        "`really_lo\xe2\x80\xa6name.cpp`, and the selected row shows the whole name.";
        }
        m.d.current.messages.push_back(std::move(asst));
        auto root = maya::Conversation{ui::conversation_config(m)}.build();
        emit_element(live ? "reasoning \xc2\xb7 live" : "reasoning \xc2\xb7 settled",
                     root, cols);
    }
}

void render_chrome(int cols) {
    struct State { const char* tag; int tokens; bool streaming; const char* text; };
    const State states[] = {
        {"idle, empty composer",           12000,  false, ""},
        {"streaming, 42% context",         84000,  true,  ""},
        {"typing, mentions and a command", 150000, false,
         "/review @src/runtime/view/composer.cpp check the footer"},
        {"near the limit",                 188000, false, "summarise and compact"},
    };
    for (const auto& s : states) {
        Model m;
        m.s.tokens_in   = s.tokens;
        m.s.context_max = 200000;
        if (s.streaming) m.s.phase = phase::Streaming{};
        m.ui.composer.text   = s.text;
        m.ui.composer.cursor = static_cast<int>(std::string_view{s.text}.size());
        auto bar  = maya::StatusBar{ui::status_bar_config(m)}.build();
        auto comp = maya::Composer{ui::composer_config(m)}.build();
        emit_element(std::string{s.tag} + " \xc2\xb7 status", bar, cols);
        emit_element(std::string{s.tag} + " \xc2\xb7 composer", comp, cols);
    }
}

// A markdown-heavy assistant answer (lifted from a recorded thread) with
// every element the prose renderer handles.
void render_prose(int cols) {
    static const char* kAnswer =
        "## Emphasis mix\n\n"
        "You can combine `code` with **bold**, or put a [link with **bold** text](https://example.com) inline.\n\n"
        "### Ordered + nested\n\n"
        "1. Setup\n   1. Install deps\n   2. Configure env\n2. Build\n3. Ship\n\n"
        "| Command | Description                        | Shorthand |\n"
        "|---------|------------------------------------|:---------:|\n"
        "| `status`| Show working tree state            | `st`      |\n"
        "| `commit`| Record changes to the repository   | `ci`      |\n\n"
        "```bash\nset -euo pipefail\nfor f in *.txt; do\n  echo \"processing $f\"\ndone\n```\n\n"
        "> Level 1\n> > Level 2\n\n"
        "- [x] Write parser\n- [ ] Ship release\n\n"
        "---\n\nWant me to focus on any one element?";
    Model m;
    Message user;
    user.role = Role::User;
    user.text = "show me every markdown element";
    m.d.current.messages.push_back(std::move(user));
    Message asst;
    asst.role = Role::Assistant;
    asst.text = kAnswer;
    m.d.current.messages.push_back(std::move(asst));
    auto root = maya::Conversation{ui::conversation_config(m)}.build();
    emit_element("prose", root, cols);
}

// The first screen: a fresh Model with no conversation.
void render_welcome(int cols) {
    Model m;
    auto root = maya::WelcomeScreen{ui::welcome_screen_config(m)}.build();
    emit_element("welcome", root, cols);
}

// The overlays a user opens most: ^K palette, the model picker, ^J threads.
void render_panels(int cols) {
    namespace pn = agentty::ui::panel;
    {
        Model m;
        m.ui.panel.descend(pn::Palette{{}});
        emit_element("palette", ui::palette_panel(m), cols);
    }
    {
        auto mk = [](std::string id, std::string name, std::string prov) {
            ModelInfo mi;
            mi.id = ModelId{std::move(id)};
            mi.display_name = std::move(name);
            mi.provider = std::move(prov);
            mi.context_window = 200000;
            return mi;
        };
        auto cat = [](std::string id, std::string label, std::vector<ModelInfo> ms) {
            ProviderCatalog c;
            c.provider_id = std::move(id);
            c.label = std::move(label);
            c.state = ProviderCatalog::State::Ready;
            c.models = std::move(ms);
            return c;
        };
        const std::vector<ProviderCatalog> cats{
            cat("anthropic", "Anthropic",
                {mk("claude-opus-4-5", "Claude Opus 4.5", "anthropic"),
                 mk("claude-sonnet-4-5", "Claude Sonnet 4.5", "anthropic"),
                 mk("claude-haiku-4-5", "Claude Haiku 4.5", "anthropic")}),
            cat("openai", "OpenAI",
                {mk("gpt-5", "GPT-5", "openai"), mk("gpt-5-mini", "GPT-5 mini", "openai")}),
        };
        const std::vector<ModelRef> recents{{"anthropic", "claude-opus-4-5"},
                                            {"openai", "gpt-5"}};
        Model m;
        ui::FusedInputs in;
        in.catalogs = &cats;
        in.recents  = &recents;
        in.active   = ModelRef{"anthropic", "claude-opus-4-5"};
        m.d.fused_rows = ui::build_fused_rows(in);
        m.ui.panel.descend(pn::Models{});
        emit_element("model picker", ui::models_panel(m), cols);
    }
    {
        Model m;
        const char* titles[] = {"tool cards: chips and syntax colour",
                                "fix MSYS2 launch under mintty",
                                "provider switching ux overhaul",
                                "reasoning: neutral ink and beats"};
        int i = 0;
        for (const char* t : titles) {
            Thread th;
            th.id = ThreadId{"t-" + std::to_string(i)};
            th.title = t;
            th.message_count = 12 + 7 * i++;
            m.d.threads.push_back(std::move(th));
        }
        m.ui.panel.descend(pn::ThreadList{});
        emit_element("thread list", ui::thread_list_panel(m), cols);
    }
    {
        Model m;
        // two separate changes in one file, the first already kept
        std::string before, after;
        for (int i = 1; i <= 30; ++i) {
            before += std::format("int f{}() {{ return {}; }}\n", i, i);
            after  += i == 4  ? std::string{"int f4() { return -1; }\nvoid log();\n"}
                    : i == 22 ? std::string{""}
                              : std::format("int f{}() {{ return {}; }}\n", i, i);
        }
        auto a = diff::compute("src/auth/login.cpp", before, after);
        if (!a.hunks.empty()) a.hunks[0].status = Hunk::Status::Accepted;
        m.d.pending_changes.push_back(a);
        // A real edit: lines changed in place, inside a function.
        m.d.pending_changes.push_back(diff::compute("src/net/http_client.cpp",
            "#include <string>\n\nint connect(const std::string& host, int port) {\n"
            "    auto timeout = 30;\n    if (port == 0) port = 80;\n    return open_socket(host, port, timeout);\n}\n",
            "#include <string>\n\nint connect(const std::string& host, int port) {\n"
            "    auto timeout = config().connect_timeout_s;\n    if (port <= 0) port = 443;\n    return open_socket(host, port, timeout);\n}\n"));
        m.d.pending_changes.push_back(diff::compute("README.md", "", "# notes\nnew file\n"));
        m.ui.panel.descend(pn::DiffReview{{1, 0}});
        emit_element("diff review", ui::diff_review(m), cols);
    }
    {
        Model m;
        m.ui.panel.descend(pn::Providers{});
        emit_element("providers", ui::providers_panel(m), cols);
    }
    {
        Model m;
        m.ui.panel.descend(pn::SettingsList{});
        emit_element("settings", ui::settings_list_panel(m), cols);
    }
}

}  // namespace

// The ^O tool-output pane: the list, then each body, from the real corpus.
// VIEWER_ROWS sets the terminal height the pane lays out for (default 40).
static void render_viewer(int cols, const std::vector<std::string>& want,
                          const std::string& kind_filter) {
    namespace pn = agentty::ui::panel;
    const auto cases = load_corpus("tests/fixtures/tool_corpus.json");
    Model m;
    Message user;
    user.role = Role::User;
    user.text = "look around";
    m.d.current.messages.push_back(std::move(user));
    Message asst;
    asst.role = Role::Assistant;
    for (const auto& c : cases) {
        if (!kind_filter.empty() && c.kind != kind_filter) continue;
        bool hit = want.empty();
        for (const auto& w : want) if (c.tool.find(w) != std::string::npos) hit = true;
        if (hit) asst.tool_calls.push_back(make_tool(c));
    }
    m.d.current.messages.push_back(std::move(asst));
    const int rows = [] {
        const char* r = std::getenv("VIEWER_ROWS");
        return r ? std::max(10, std::atoi(r)) : 40;
    }();
    setenv("LINES", std::to_string(rows).c_str(), 1);
    setenv("COLUMNS", std::to_string(cols).c_str(), 1);
    auto entries = app::detail::collect_viewer_entries(m);
    if (entries.empty()) { std::puts("no entries"); return; }
    const int n = static_cast<int>(entries.size());
    m.ui.panel.descend(pn::ToolOutput{{std::move(entries), 0, false}});
    emit_element("tool output \xc2\xb7 list", ui::tool_output_panel(m), cols);
    auto* o = m.ui.panel.get<pn::ToolOutput>();
    o->viewing = true;
    if (const char* q = std::getenv("VIEWER_FIND")) { o->query = q; ++o->hit_seq; }
    if (std::getenv("VIEWER_FULL")) {
        // The real screen: base + overlay, laid out for a cols x rows terminal.
        // The corpus turn is cleared so the base is just the chrome.
        m.d.current.messages.clear();
        m.ui.term_cols = cols;
        m.ui.term_rows = rows;
        {
            Model bare;
            bare.ui.term_cols = cols;
            bare.ui.term_rows = rows;
            std::printf("== base alone %dx%d: %d rows\n", cols, rows,
                        maya::measure_element(ui::view(bare), cols).height.value);
            bare.ui.panel.descend(pn::Palette{});
            std::printf("== palette open %dx%d: %d rows\n", cols, rows,
                        maya::measure_element(ui::view(bare), cols).height.value);
        }
        for (int i = 0; i < std::min(n, 3); ++i) {
            o->index = i;
            m.ui.tool_viewer_scroll.y = 0;
            auto root = ui::view(m);
            const auto sz = maya::measure_element(root, cols);
            std::printf("\n== full screen %dx%d, entry %d: frame is %d rows%s\n", cols, rows, i + 1,
                        sz.height.value, sz.height.value > rows ? "  OVERFLOW" : "");
            emit_element("screen", root, cols);
        }
        return;
    }
    const int bodies = std::getenv("VIEWER_ALL") ? n : std::min(n, 4);
    for (int i = 0; i < bodies; ++i) {
        o->index = i;
        m.ui.tool_viewer_scroll.y = 0;
        emit_element("tool output \xc2\xb7 body " + std::to_string(i + 1),
                     ui::tool_output_panel(m), cols);
    }
}

// ^O on a subagent: the calls it made, then its report as prose.
static void render_viewer_agent(int cols) {
    namespace pn = agentty::ui::panel;
    using namespace std::chrono;
    using C = ToolUse::Child;
    const auto now = steady_clock::now();
    ToolUse t;
    t.id   = ToolCallId{"va1"};
    t.name = ToolName{"task"};
    t.args = json{{"agent_type", "explorer"},
                  {"display_description", "Demo: how the agent card stays a fixed height"},
                  {"prompt", "x"}};
    t.status = ToolUse::Done{now - seconds{17}, now,
        "Subagent report (explorer, 4 turns):\n\n"
        "**Outcome:** a subagent's Agent card shows at most the last 5 nested calls, one row each.\n\n"
        "## Details\n"
        "- **Calls shown:** the cap is 5 (`agent_timeline.cpp:501`). A dim `⋯ N earlier` row sits above them, and this "
        "line is long enough that it has to wrap onto a second row in the reader.\n"
        "- **Persistence:** `children` is saved and restored in `persistence.cpp:398-415`.\n"
        "  - nested bullet keeps its indent\n"
        "1. First numbered step\n2. Second numbered step\n\n"
        "| File | Purpose |\n|---|---|\n| exec.cpp | command execution |\n\n"
        "```\nconstexpr std::size_t cap = 5;\n```\n"};
    t.children = {
        C{"k1", "grep", json{{"pattern", "children_hidden"}}, C::State::Done, "a.hpp:1: x\n", 80},
        C{"k2", "glob", json{{"pattern", "*agent_timeline*"}}, C::State::Done, "a\nb\n", 12},
        C{"k3", "read", json{{"path", "src/io/persistence.cpp"}, {"start_line", 390}, {"end_line", 420}},
          C::State::Done, "x", 3},
        C{"k4", "shell", json{{"command", "git log --oneline -3"}}, C::State::Failed,
          "tool 'shell' is not available in this run", 0},
    };
    Model m;
    Message a; a.role = Role::Assistant;
    a.tool_calls.push_back(std::move(t));
    m.d.current.messages.push_back(std::move(a));
    setenv("LINES", "40", 1);
    setenv("COLUMNS", std::to_string(cols).c_str(), 1);
    auto entries = app::detail::collect_viewer_entries(m);
    if (entries.empty()) { std::puts("no entries"); return; }
    m.ui.panel.descend(pn::ToolOutput{{std::move(entries), 0, false}});
    auto* o = m.ui.panel.get<pn::ToolOutput>();
    o->viewing = true;
    m.ui.tool_viewer_scroll.y = 0;
    emit_element("tool output \xc2\xb7 agent", ui::tool_output_panel(m), cols);
}

int main(int argc, char** argv) {
    // The view reads loop_bound<T> caches (the tool-body and settled-panel
    // memos in agent_timeline.cpp), which only the kernel's loop thread may
    // touch. A fixture has no kernel, so borrow the identity jaal exposes
    // for exactly this: it arms THIS thread for the scope below.
    const auto loop = maya::arm_loop_for_tests();
    // FREEZE_CLOCK=1: pin the animation clock so spinners and shimmer render
    // the same frame every run (the UI snapshot test sets it).
    if (const char* f = std::getenv("FREEZE_CLOCK"); f && *f == '1')
        maya::testing::freeze_anim_clock(1000);

    // Install the theme as the LIVE one, not just pass it to render_tree.
    // Style::to_sgr() resolves a slot through theme::live(); with none set
    // every cell comes out with an empty SGR -- correct behaviour, and a
    // monochrome wall here.
    // THEME=<scheme name> (or "native"); defaults to Srcery, what Ayush runs.
    const char* theme_name = std::getenv("THEME");
    if (!theme_name) theme_name = "Srcery";
    // TERM_BG / TERM_FG=RRGGBB stand in for what a real terminal answers to
    // the startup colour probe, so native can be checked without one.
    auto hex = [](const char* s, maya::LitColor& out) {
        if (!s || std::strlen(s) != 6) return false;
        const unsigned v = static_cast<unsigned>(std::strtoul(s, nullptr, 16));
        out = maya::LitColor::rgb(static_cast<uint8_t>(v >> 16),
                                  static_cast<uint8_t>(v >> 8),
                                  static_cast<uint8_t>(v));
        return true;
    };
    if (maya::LitColor fg = maya::LitColor::rgb(0, 0, 0), bg = fg;
        hex(std::getenv("TERM_FG"), fg) && hex(std::getenv("TERM_BG"), bg))
        maya::theme::set_terminal_colors(fg, bg);
    // TERM_PALETTE="RRGGBB,RRGGBB,..." (16 entries): the OSC 4 answers.
    if (const char* pal = std::getenv("TERM_PALETTE")) {
        std::string s{pal};
        int k = 0;
        for (std::size_t p = 0; p <= s.size() && k < 16;) {
            const auto e = s.find(',', p);
            const std::string one = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
            if (maya::LitColor c = maya::LitColor::rgb(0, 0, 0); hex(one.c_str(), c))
                maya::theme::set_terminal_palette(k, c);
            ++k;
            if (e == std::string::npos) break;
            p = e + 1;
        }
    }
    const maya::Theme* th = ui_prefs::find_scheme(theme_name);
    ui_prefs::publish_theme(th ? *th : maya::theme::native);

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
        else if (a == "--ask") g_ask = true;
        else if (a == "--chrome") { render_chrome(cols); return 0; }
        else if (a == "--reasoning") { render_reasoning(cols); return 0; }
        else if (a == "--prose") { render_prose(cols); return 0; }
        else if (a == "--welcome") { render_welcome(cols); return 0; }
        else if (a == "--panels") { render_panels(cols); return 0; }
        else if (a == "--agents") { render_agents(cols); return 0; }
        else if (a == "--viewer-agent") { render_viewer_agent(cols); return 0; }
        else if (a == "--viewer") {
            std::vector<std::string> w; std::string k;
            for (int j = i + 1; j < argc; ++j) {
                const std::string b = argv[j];
                if (b == "--kind" && j + 1 < argc) k = argv[++j]; else w.push_back(b);
            }
            render_viewer(cols, w, k);
            return 0;
        }
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
