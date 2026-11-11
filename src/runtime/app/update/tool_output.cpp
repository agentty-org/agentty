// tool_output.cpp — the tool-output viewer panel's reducer (^O): the
// list/body two-stage navigation, clipboard copy, and tail-follow scroll.
//
// collect_viewer_entries / resync_live_tool_viewer stay in tool.cpp beside
// the execution state they read (declared in internal.hpp) — they are the
// shared seam between the panel and the execution plumbing.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <algorithm>
#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/io/clipboard.hpp"          // write_clipboard_text
#include "agentty/runtime/panel/tool_output.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_args.hpp"

namespace agentty::tool_output {

namespace {
// A note a tool leaves where it cut its output. Shown to the reader as a
// "⋯ …" line, since a silent cut makes the output look complete.
bool is_cut_note(std::string_view t) {
    return t.starts_with("[output capped") || t.starts_with("[output truncated")
        || t.starts_with("[truncated")
        || (t.starts_with("[... ") && t.find(" elided") != std::string_view::npos);
}
std::string as_cut_marker(std::string_view t) {
    t.remove_prefix(1);
    if (!t.empty() && t.back() == ']') t.remove_suffix(1);
    while (t.starts_with(".") || t.starts_with(" ")) t.remove_prefix(1);
    return "\xe2\x8b\xaf " + std::string{t};   // ⋯
}
} // namespace

std::string display_text(const Entry& e) {
    // Thinking is the model's own prose: nothing to clean.
    if (e.thinking) return e.output;
    // Tools echo their own description as line 1; the header already says it.
    std::string_view s{e.output};
    if (e.call.args.is_object())
        for (const char* k : {"display_description", "description"})
            if (auto it = e.call.args.find(k); it != e.call.args.end() && it->is_string()) {
                const auto& d = it->get_ref<const std::string&>();
                if (!d.empty() && s.starts_with(d)
                    && (s.size() == d.size() || s[d.size()] == '\n')) {
                    s.remove_prefix(std::min(s.size(), d.size() + 1));
                    break;
                }
            }
    // Turn cut notes into visible markers before the cleaner drops them.
    std::string marked;
    std::vector<std::string> tail_notes;
    marked.reserve(s.size());
    const auto last_fence = s.rfind("```");
    for (std::size_t p = 0; p <= s.size();) {
        auto nl = s.find('\n', p);
        if (nl == std::string_view::npos) nl = s.size();
        std::string_view ln = s.substr(p, nl - p);
        auto t = ln;
        while (!t.empty() && t.front() == ' ') t.remove_prefix(1);
        if (is_cut_note(t)) {
            // Notes after a closing fence would be dropped with the fence.
            if (last_fence != std::string_view::npos && p > last_fence)
                tail_notes.push_back(as_cut_marker(t));
            else
                marked += as_cut_marker(t);
        } else {
            marked.append(ln);
        }
        if (nl == s.size()) break;
        marked += '\n';
        p = nl + 1;
    }
    // Shell-style tools wrap their output in a ``` fence for the model.
    const bool shell = e.call.args.is_object() && e.call.args.contains("command");
    std::string out = ui::clean_output(shell ? ui::strip_bash_output_fence(marked)
                                             : marked);
    for (auto& n : tail_notes) {
        if (!out.empty()) out += '\n';
        out += n;
    }
    return out;
}

std::vector<std::pair<std::size_t, std::size_t>>
hit_spans(std::string_view line, std::string_view q) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    if (q.empty() || line.size() < q.size()) return out;
    auto low = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; };
    for (std::size_t i = 0; i + q.size() <= line.size();) {
        std::size_t k = 0;
        while (k < q.size() && low(line[i + k]) == low(q[k])) ++k;
        if (k == q.size()) { out.push_back({i, q.size()}); i += q.size(); }
        else ++i;
    }
    return out;
}

} // namespace agentty::tool_output

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

using maya::overload;

Cmd tool_output_update(Model& m, msg::ToolOutputMsg tm) {
    return std::visit(overload{
        [&](OpenToolOutput) -> Cmd {
            auto entries = collect_viewer_entries(m);
            if (entries.empty()) {
                auto cmd = set_status_toast(m, "nothing to inspect yet");
                return cmd;
            }
            m.ui.panel.descend(pn::ToolOutput{{std::move(entries), 0, false}});
            m.ui.tool_viewer_scroll.y = 0;
            m.ui.tool_viewer_tail = true;
            return Cmd::none();
        },
        [&](OpenThinking) -> Cmd {
            auto entries = collect_thinking_entries(m);
            if (entries.empty())
                return set_status_toast(m, "no thinking in this thread yet");
            // Straight onto the newest block: that's almost always the one
            // you want. Esc goes to the list of all of them.
            tool_output::Open open{std::move(entries), 0, true};
            open.thinking_only = true;
            m.ui.panel.descend(pn::ToolOutput{std::move(open)});
            m.ui.tool_viewer_scroll.y = 0;
            m.ui.tool_viewer_tail = true;
            return Cmd::none();
        },
        [&](CloseToolOutput& e) -> Cmd {
            // Esc semantics: an active search is cleared first; then body →
            // back to the list; list → unwind one level. The open chord
            // pressed again (`all`) skips the steps and closes.
            if (auto* o = m.ui.panel.get<pn::ToolOutput>(); o && o->viewing && !e.all) {
                if (!o->query.empty() || o->typing) {
                    o->query.clear();
                    o->typing = false;
                    return Cmd::none();
                }
                o->viewing = false;
                m.ui.tool_viewer_scroll.y = 0;
                return Cmd::none();
            }
            ascend(m);
            return Cmd::none();
        },
        [&](ToolOutputMove& e) -> Cmd {
            auto* o = m.ui.panel.get<pn::ToolOutput>();
            if (!o) return Cmd::none();
            if (o->viewing) {
                // Body stage: deltas scroll the viewport; a page is what the
                // view last showed, less a line of overlap.
                auto& sc = m.ui.tool_viewer_scroll;
                const int page = std::max(1, sc.viewport_bounds.h - 1);
                sc.y = std::clamp(sc.y + e.delta + e.pages * page, 0, std::max(0, sc.max_y));
                // Live tail-follow: scrolling UP off the bottom disengages
                // auto-tail so the user can read earlier output; scrolling
                // (or Ending) back to the bottom re-engages it.
                m.ui.tool_viewer_tail = (sc.y >= sc.max_y);
            } else {
                int sz = static_cast<int>(o->entries.size());
                if (sz > 0)
                    o->index = std::clamp(o->index + e.delta + e.pages * 10, 0, sz - 1);
                o->fresh_above = std::min(o->fresh_above, o->index);
            }
            return Cmd::none();
        },
        [&](ToolOutputSelect) -> Cmd {
            auto* o = m.ui.panel.get<pn::ToolOutput>();
            if (!o || o->viewing) return Cmd::none();
            if (o->index < 0
                || o->index >= static_cast<int>(o->entries.size()))
                return Cmd::none();
            o->viewing = true;
            m.ui.tool_viewer_scroll.y = 0;
            m.ui.tool_viewer_tail = true;
            return Cmd::none();
        },
        [&](ToolOutputStep& e) -> Cmd {
            // ←/→ while reading an output: hop to the neighbouring
            // entry's body directly. Clamped at the ends (no wrap — the
            // list is short and wrap-around disorients more than it
            // helps). List stage: no-op.
            auto* o = m.ui.panel.get<pn::ToolOutput>();
            if (!o || !o->viewing) return Cmd::none();
            int sz = static_cast<int>(o->entries.size());
            if (sz <= 0) return Cmd::none();
            int next = std::clamp(o->index + e.delta, 0, sz - 1);
            if (next != o->index) {
                o->index = next;
                o->fresh_above = std::min(o->fresh_above, o->index);
                m.ui.tool_viewer_scroll.y = 0;
                m.ui.tool_viewer_tail = true;
                // A search carries over: land on this output's first match.
                if (!o->query.empty()) { o->hit = 0; ++o->hit_seq; }
            }
            return Cmd::none();
        },
        [&](ToolOutputNewest) -> Cmd {
            // `t`: jump to the top entry (the newest), in whichever stage.
            auto* o = m.ui.panel.get<pn::ToolOutput>();
            if (!o || o->entries.empty() || o->index == 0) return Cmd::none();
            o->index = 0;
            o->fresh_above = 0;
            m.ui.tool_viewer_scroll.y = 0;
            m.ui.tool_viewer_tail = true;
            if (!o->query.empty()) { o->hit = 0; ++o->hit_seq; }
            return Cmd::none();
        },
        [&](ToolOutputFind& e) -> Cmd {
            auto* o = m.ui.panel.get<pn::ToolOutput>();
            if (!o || o->entries.empty()) return Cmd::none();
            using Op = ToolOutputFind::Op;
            auto jump = [&](int to) {
                o->hit = to;
                ++o->hit_seq;
                m.ui.tool_viewer_tail = false;   // reading, not following
            };
            switch (e.op) {
                case Op::Open:
                    if (!o->viewing) {   // / on the list searches the selected output
                        o->viewing = true;
                        m.ui.tool_viewer_scroll.y = 0;
                    }
                    o->typing = true;
                    o->query.clear();
                    break;
                case Op::Type: {
                    if (!o->typing || e.ch < 0x20) break;
                    char buf[4];
                    const char32_t c = e.ch;
                    std::size_t n = 0;
                    if (c < 0x80) { buf[n++] = char(c); }
                    else if (c < 0x800) { buf[n++] = char(0xC0 | (c >> 6)); buf[n++] = char(0x80 | (c & 0x3F)); }
                    else if (c < 0x10000) { buf[n++] = char(0xE0 | (c >> 12)); buf[n++] = char(0x80 | ((c >> 6) & 0x3F)); buf[n++] = char(0x80 | (c & 0x3F)); }
                    else { buf[n++] = char(0xF0 | (c >> 18)); buf[n++] = char(0x80 | ((c >> 12) & 0x3F)); buf[n++] = char(0x80 | ((c >> 6) & 0x3F)); buf[n++] = char(0x80 | (c & 0x3F)); }
                    o->query.append(buf, n);
                    jump(0);
                    break;
                }
                case Op::Erase:
                    if (!o->typing) break;
                    while (!o->query.empty()) {   // one UTF-8 character
                        const unsigned char b = static_cast<unsigned char>(o->query.back());
                        o->query.pop_back();
                        if ((b & 0xC0) != 0x80) break;
                    }
                    if (!o->query.empty()) jump(0);
                    break;
                case Op::Commit:
                    o->typing = false;
                    break;
                case Op::Cancel:
                    o->typing = false;
                    o->query.clear();
                    break;
                case Op::Next:
                    if (!o->query.empty()) jump(o->hit + 1);
                    break;
                case Op::Prev:
                    if (!o->query.empty()) jump(o->hit - 1);
                    break;
            }
            return Cmd::none();
        },
        [&](ToolOutputCopy) -> Cmd {
            auto* o = m.ui.panel.get<pn::ToolOutput>();
            if (!o) return Cmd::none();
            if (o->index < 0
                || o->index >= static_cast<int>(o->entries.size()))
                return Cmd::none();
            // What the reader sees, not the notes and fences meant for the model.
            std::string body = tool_output::display_text(
                o->entries[static_cast<std::size_t>(o->index)]);
            std::size_t lines = body.empty() ? 0 : 1;
            for (char c : body) if (c == '\n') ++lines;
            (void)write_clipboard_text(body);   // native pbcopy/wl-copy/xclip
            auto toast = set_status_toast(m, "copied " + std::to_string(lines)
                + (lines == 1 ? " line" : " lines"));
            return Cmd::batch(
                cmd::write_clipboard(std::move(body)),
                std::move(toast));
        },
    }, tm);
}

} // namespace agentty::app::detail
