// tool_body_preview — `task` (subagent) tool body.
//
// RUNNING: the activity feed under the header. One line per tool call
// ("○ grep foo" while it runs, resolved in place to "✓ grep foo  1.2s" or
// "✗ ... — why"), the model's current narration as the last row. The status
// line (turn N/M, tools, time) lives in the card HEADER, not here.
//
// SETTLED: the report, cleaned for a 5-row glance. The runner's bracketed
// banner ("[subagent hit its turn budget ...]") already shows as the header's
// failure reason, so it is dropped here; markdown punctuation that would print
// literally (`**`, `##`, table pipes) is stripped; and the model's OUTCOME
// line leads. A partial report (out of turns / time) is shown like a success,
// in the warning colour: it carries real findings, and hiding it behind a
// bare "failed" body was the worst part of the old card.

#include "tool_body_common.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

#include "agentty/runtime/view/palette.hpp"

namespace agentty::ui::detail {

namespace {

// Remove `**x**` emphasis pairs (same line, no space just inside the
// delimiters) and code ticks. A lone `**` -- `docs/**`, `a ** b` -- is
// content and stays.
std::string strip_emphasis(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '`') { ++i; continue; }
        if (s.compare(i, 2, "**") == 0) {
            const auto close = s.find("**", i + 2);
            if (close != std::string_view::npos && close > i + 2
                && s[i + 2] != ' ' && s[close - 1] != ' ') {
                for (std::size_t k = i + 2; k < close; ++k)
                    if (s[k] != '`') out += s[k];
                i = close + 2;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

// Marks a line clean_report should drop outright (not render as blank).
constexpr std::string_view kDropLine = "\x01";

// One report line, readable as plain text in a narrow card row.
std::string clean_report_line(std::string_view ln) {
    while (!ln.empty() && (ln.back() == ' ' || ln.back() == '\r')) ln.remove_suffix(1);
    std::size_t indent = 0;
    while (indent < ln.size() && ln[indent] == ' ') ++indent;
    std::string_view body = ln.substr(indent);
    // "## Heading" -> "Heading"; "- item" / "* item" -> "• item".
    while (body.starts_with('#')) body.remove_prefix(1);
    while (body.starts_with(' ') && indent == 0 && !ln.empty() && ln[0] == '#') body.remove_prefix(1);
    std::string out(indent, ' ');
    if (body.starts_with("- ") || body.starts_with("* ")) {
        out += "\xe2\x80\xa2 ";   // •
        body.remove_prefix(2);
    }
    // Table row "| a | b |" -> "a · b"; separator rows vanish.
    if (body.starts_with('|')) {
        if (body.find_first_not_of("|-: ") == std::string_view::npos) return std::string{kDropLine};
        std::string row;
        std::size_t i = 1;
        while (i < body.size()) {
            const auto j = body.find('|', i);
            std::string_view cell = body.substr(i, (j == std::string_view::npos ? body.size() : j) - i);
            while (!cell.empty() && cell.front() == ' ') cell.remove_prefix(1);
            while (!cell.empty() && cell.back() == ' ') cell.remove_suffix(1);
            if (!cell.empty()) { if (!row.empty()) row += " \xc2\xb7 "; row += cell; }
            if (j == std::string_view::npos) break;
            i = j + 1;
        }
        out += strip_emphasis(row);
        return out;
    }
    out += strip_emphasis(body);
    return out;
}

// The report minus the runner's header/banner lines, cleaned, blank runs
// collapsed, the incomplete-note label dropped.
std::string clean_report(std::string_view body) {
    std::string out;
    bool last_blank = true;
    std::size_t i = 0;
    while (i <= body.size()) {
        const auto j = body.find('\n', i);
        std::string_view ln = body.substr(i, (j == std::string_view::npos ? body.size() : j) - i);
        i = j == std::string_view::npos ? body.size() + 1 : j + 1;
        if (ln.starts_with("Subagent report (") || ln.starts_with("tools: ")) continue;
        if (ln.starts_with("[subagent ")) continue;                       // banner: in the header
        if (ln.starts_with("Last text the subagent produced")) continue;  // label noise
        std::string c = clean_report_line(ln);
        if (c == kDropLine) continue;
        const bool blank = c.find_first_not_of(' ') == std::string::npos;
        if (blank) { if (!last_blank) out += '\n'; last_blank = true; continue; }
        out += c;
        out += '\n';
        last_blank = false;
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

// "◆ coder · turn 7/80 · 9 tools · 1m 12s" -> "▰▱▱▱▱▱▱▱ turn 7/80 · 9 tools · 1m 12s".
// The bar shows how much of the turn budget is spent, so a run nearing its
// cap is visible at a glance. Empty when the line isn't a status line.
std::string status_row(std::string_view first) {
    const auto at = first.find("turn ");
    if (at == std::string_view::npos) return {};
    const std::string_view rest = first.substr(at);
    int turn = 0, max = 0;
    std::size_t i = 5;
    while (i < rest.size() && rest[i] >= '0' && rest[i] <= '9') turn = turn * 10 + (rest[i++] - '0');
    if (i < rest.size() && rest[i] == '/') ++i;
    while (i < rest.size() && rest[i] >= '0' && rest[i] <= '9') max = max * 10 + (rest[i++] - '0');
    std::string bar;
    if (max > 0) {
        constexpr int kCells = 10;
        const int filled = std::clamp((turn * kCells + max - 1) / max, 1, kCells);
        for (int k = 0; k < kCells; ++k) bar += k < filled ? "\xe2\x96\xb0" : "\xe2\x96\xb1";
        bar += "  ";
    }
    return bar + std::string{rest};
}

}  // namespace

bool task_body(const ToolUse& tc, maya::ToolBodyPreview::Config& out) {
    using Kind = maya::ToolBodyPreview::Kind;

    // The live feed gets more room (it's the only window into what the
    // subagent is doing, and it's transient); the settled report stays a
    // tight glance -- the full report is in the transcript and Ctrl+O.
    constexpr std::size_t kTaskLiveRows   = 7;
    constexpr std::size_t kTaskReportRows = 5;
    if (tc.is_running()) {
        std::string_view feed = tc.progress_text();
        // The runner's status line leads every snapshot. It is shown as the
        // body's FIRST row, pinned above the scrolling feed -- not in the
        // card header, which must stay byte-stable once it has scrolled
        // into native scrollback.
        std::string status;
        if (feed.starts_with("\xe2\x97\x86 ")) {
            const auto nl = feed.find('\n');
            status = status_row(feed.substr(0, nl));
            feed = nl == std::string_view::npos ? std::string_view{} : feed.substr(nl + 1);
        }
        while (!feed.empty() && feed.front() == '\n') feed.remove_prefix(1);
        // Nothing has happened yet: zero rows. The header spinner says "in
        // flight", and inserting a placeholder row here can shift content
        // that is already in native scrollback (tool_timeline_adapter_test).
        if (feed.empty()) return true;
        // With nested rows (the runner sent structured children), they ARE
        // the activity: the body keeps only the one-line status row. Nothing
        // that comes and goes -- a row that appears and vanishes between
        // updates makes the whole live card, and the composer under it,
        // bounce up and down.
        if (!tc.children.empty()) {
            if (status.empty()) return true;
            out.kind = Kind::CodeBlock;
            out.text = status;
            out.text_color = text_primary;
            out.colorize = true;
            out.show_all = true;
            out.is_streaming = true;
            return true;
        }
        // Pre-sliced: status row + the newest feed lines, shown verbatim
        // (CodeBlock + show_all) so the widget's own tail can't scroll the
        // status row away. colorize paints ✓ / ✗, paths and numbers.
        out.kind = Kind::CodeBlock;
        const std::size_t rows = status.empty() ? kTaskLiveRows : kTaskLiveRows - 1;
        out.text = (status.empty() ? std::string{} : status + "\n") + tail_window(feed, rows);
        out.text_color = text_primary;
        out.colorize = true;
        out.show_all = true;
        out.is_streaming = true;
        return true;
    }
    if (!tc.is_terminal() || card_output(tc).empty()) return true;

    const std::string_view raw = card_output(tc);
    const bool partial = tc.is_failed()
        && (raw.find("turn budget") != std::string_view::npos
            || raw.find("wall-clock") != std::string_view::npos
            || raw.find("Last text the subagent produced") != std::string_view::npos);
    // A hard failure (provider error, cancel, stuck loop): say what happened
    // in words and what to try, then the last few steps. The raw banner is
    // already the header's reason, so it isn't repeated here.
    if (tc.is_failed() && !partial) {
        std::string hint;
        if (raw.find("overloaded") != std::string_view::npos || raw.find("529") != std::string_view::npos
            || raw.find("rate limit") != std::string_view::npos || raw.find("429") != std::string_view::npos)
            hint = "The provider was overloaded and retries ran out. Ask again in a moment.";
        else if (raw.find("cancelled") != std::string_view::npos)
            hint = "Cancelled before it finished.";
        else if (raw.find("same tool call failed") != std::string_view::npos)
            hint = "Stopped: it kept repeating a failing call. Give it a narrower task.";
        else if (raw.find("context window full") != std::string_view::npos)
            hint = "It ran out of context. Split the task into smaller ones.";
        else if (raw.find("[subagent failed: ") != std::string_view::npos)
            hint = "The model call failed; the agent's work up to that point is below.";
        if (hint.empty()) return true;   // the generic failure body owns it
        std::string steps;
        if (auto p = raw.find("Last steps:\n"); p != std::string_view::npos) {
            std::string_view s = raw.substr(p + 12);
            // keep the last 3 step lines
            std::size_t cut = s.size();
            for (int n = 0; n < 3 && cut > 0; ++n) {
                const auto nl = s.rfind('\n', cut - 1);
                cut = nl == std::string_view::npos ? 0 : nl;
            }
            steps = std::string{s.substr(cut == 0 ? 0 : cut + 1)};
            while (!steps.empty() && steps.back() == '\n') steps.pop_back();
        }
        out.kind = Kind::CodeBlock;
        out.text = hint + (steps.empty() ? "" : "\n" + steps);
        out.text_color = text_primary;
        out.colorize = true;
        out.show_all = true;
        return true;
    }

    std::string body = clean_report(raw);
    if (body.empty()) return true;
    // Partial reports drop their "Last steps" log from the glance: the
    // findings above it are what the user wants first.
    if (partial)
        if (auto p = body.find("Last steps:"); p != std::string::npos) {
            body.resize(p);
            while (!body.empty() && body.back() == '\n') body.pop_back();
        }
    out.kind = Kind::CodeBlock;
    // content lines + 1 "⋯ N more" marker = the row ceiling.
    out.text = head_window(body, kTaskReportRows - 1);
    out.text_color = partial ? status_warn : text_primary;
    if (partial) out.chrome_color = status_warn;
    out.colorize = !partial;
    // Pre-sliced to the ceiling; render verbatim (no head+tail elision that
    // would fight the marker).
    out.show_all = true;
    return true;
}

} // namespace agentty::ui::detail
