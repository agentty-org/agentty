// Shared helpers for the per-tool body-preview renderers. See
// tool_body_common.hpp for the contract.

#include "tool_body_common.hpp"

#include <algorithm>
#include <cctype>

#include <maya/platform/io.hpp>

#include "agentty/runtime/view/thread/turn/agent_timeline/tool_args.hpp"

namespace agentty::ui::detail {

// ── Streaming-card body budget ────────────────────────────────────────
//
// THE INVARIANT (scrollback oracle, write/edit turn): while a write/edit
// card streams, its event HEADER row must stay inside the viewport. If
// the body grows tall enough to push the header into native scrollback,
// the settle's Running→Done restyle of that row (● bright → ✓ + dim on
// the tree/name cells — a STYLE-ONLY rewrite, invisible in char dumps)
// is a committed-row rewrite; maya's gate can only recover with a
// destructive HardReset on the grow frame.
//
// The budget therefore scales with the REAL terminal height instead of
// being pinned to the worst case. Fixed chrome below the header, counted
// from the oracle's 60x18 viewport dump:
//   header(1) + blank(1) + footer(1) + card bottom border(1) + gap(1)
//   + composer(6) + status bar(3) = 14 rows, +1 slack = 15.
// body_budget = term_rows − 15, floored at 3 (the proven-safe minimum at
// 18-row terminals — exactly the config the oracle passes with).
//
// Tool bodies are built at VIEW-BUILD time, under the RenderContext view()
// installs from the Model's terminal size, so available_height() is the real
// height there (24 outside any view, the old no-tty fallback).
int stream_body_budget() {
    const int rows = maya::available_height() > 0 ? maya::available_height() : 24;
    constexpr int kChromeBelowHeader = 15;
    return std::max(3, rows - kChromeBelowHeader);
}

int stream_card_rows_bound() {
    // Mirror edit_body.cpp's per_side derivation EXACTLY — if that clamp
    // changes, this must change with it, which is why they sit one call
    // apart rather than as two independent constants.
    const int per_side = std::clamp((stream_body_budget() - 1) / 2, 1, 6);
    const int body_rows = 1 + 2 * per_side;   // stat chip + tail rows
    // Per-card chrome: event header, hunk/stat line, top+bottom border,
    // footer status, and the blank gutter rows the card sits in.
    constexpr int kCardChromeRows = 7;
    return body_rows + kCardChromeRows;
}

std::string tail_window(std::string_view s, std::size_t keep_lines) {
    if (s.empty()) return {};
    // Walk backwards counting newlines; stop after keep_lines+1 of them
    // (the +1 anchors the start of the first kept line).
    std::size_t nl_seen = 0;
    std::size_t start = s.size();
    for (std::size_t i = s.size(); i-- > 0;) {
        if (s[i] == '\n') {
            if (++nl_seen > keep_lines) { start = i + 1; break; }
        }
        if (i == 0) start = 0;
    }
    return std::string{s.substr(start)};
}

std::string head_window(std::string_view s, std::size_t keep_lines) {
    if (s.empty()) return {};
    std::size_t nl_seen = 0;
    std::size_t end = s.size();
    std::size_t total_lines = 1;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            ++total_lines;
            if (nl_seen + 1 == keep_lines && end == s.size()) end = i;
            ++nl_seen;
        }
    }
    if (total_lines <= keep_lines) return std::string{s};
    std::string out{s.substr(0, end)};
    std::size_t more = total_lines - keep_lines;
    out += "\n\xe2\x8b\xaf " + std::to_string(more)
         + (more == 1 ? " more line" : " more lines");  // ⋯
    return out;
}

std::string read_path_arg(const nlohmann::json& args) {
    if (!args.is_object()) return {};
    for (auto k : {"path", "file_path", "filepath", "filename"}) {
        if (auto it = args.find(k); it != args.end() && it->is_string())
            return it->get<std::string>();
    }
    return {};
}

// Parse `## Matches in <path>` and `### L<start>-<end>` markers out of
// agentty's grep tool output and accumulate (path → {start lines}) into
// `out`. We use the BLOCK START line as a representative match anchor
// — agentty's output groups matches with surrounding context, so the
// individual match offsets aren't recoverable from the rendered body.
// Block-start is good enough for the highlight_lines anchor; the user's
// eye lands on the right region of the file.
void accumulate_grep_hits(std::string_view output, GrepHits& out) {
    constexpr std::string_view kPathTag  = "## Matches in ";
    constexpr std::string_view kBlockTag = "### L";

    std::string current_path;
    std::size_t pos = 0;
    while (pos < output.size()) {
        const auto nl = output.find('\n', pos);
        const auto end = (nl == std::string::npos) ? output.size() : nl;
        const std::string_view line(output.data() + pos, end - pos);

        if (line.starts_with(kPathTag)) {
            current_path = std::string{line.substr(kPathTag.size())};
        } else if (!current_path.empty() && line.starts_with(kBlockTag)) {
            // Parse `### L<start>-<end>` — read digits up to '-' or end.
            std::size_t i = kBlockTag.size();
            int start = 0;
            bool got = false;
            while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
                start = start * 10 + (line[i] - '0');
                ++i;
                got = true;
            }
            if (got && start > 0)
                out[current_path].insert(start);
        }

        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
}

std::string args_match_needle(const nlohmann::json& args) {
    if (!args.is_object()) return {};
    // An explicit regex:true means the arg is a pattern, not a literal.
    const auto r = args.find("regex");
    const bool regex_mode =
        r != args.end() && r->is_boolean() && r->get<bool>();
    if (regex_mode) return {};

    // `find` is literal by contract when regex is off, so take it as-is --
    // metacharacter screening would throw away perfectly good code needles
    // like `foo(bar);`. The pattern-style args have no such contract, so
    // those still get screened below.
    if (auto it = args.find("find"); it != args.end() && it->is_string()) {
        auto needle = it->get<std::string>();
        if (needle.size() < 2 || needle.size() > 200) return {};
        if (needle.find('\n') != std::string::npos) return {};
        return needle;
    }

    std::string needle;
    for (const char* key : {"pattern", "needle", "query"}) {
        auto it = args.find(key);
        if (it != args.end() && it->is_string()) {
            needle = it->get<std::string>();
            break;
        }
    }
    if (needle.size() < 2 || needle.size() > 200) return {};
    if (needle.find('\n') != std::string::npos) return {};

    // A pattern that is purely an alternation of literals (`foo|bar|128000`)
    // is still highlightable: split on '|' and hand maya every alternative.
    // Any other regex metacharacter means we can't substring-match it.
    if (needle.find_first_of("\\^$.?*+()[]{}") != std::string::npos)
        return {};
    if (needle.find('|') == std::string::npos) return needle;

    std::string out;
    for (std::size_t s = 0; s <= needle.size();) {
        const auto e = needle.find('|', s);
        const auto alt = needle.substr(
            s, e == std::string::npos ? std::string::npos : e - s);
        if (alt.size() >= 2) {
            if (!out.empty()) out += '\n';
            out += alt;
        }
        if (e == std::string::npos) break;
        s = e + 1;
    }
    return out;
}

std::string_view strip_error_tags(std::string_view s) {
    // Drop every leading `[kind] ` tag. Exit trailers like `[exit code 1]`
    // never lead the text, so they are left alone.
    for (;;) {
        if (s.empty() || s.front() != '[') return s;
        const auto close = s.find(']');
        if (close == std::string_view::npos || close < 2 || close > 32)
            return s;
        const auto inner = s.substr(1, close - 1);
        if (inner.find_first_of("[\n") != std::string_view::npos) return s;
        if (inner.starts_with("exit code") || inner.starts_with("terminated"))
            return s;
        s.remove_prefix(close + 1);
        while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    }
}

std::string failure_body_text(const ToolUse& tc) {
    auto s = strip_error_tags(card_output(tc));
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.remove_suffix(1);
    // The header already says these; a body row would only repeat it.
    if (s == "cancelled" || s == "interrupted" || s == "aborted") return {};
    return std::string{s};
}

std::string lang_for_path(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    std::string base{slash == std::string_view::npos ? path
                                                     : path.substr(slash + 1)};
    for (auto& c : base)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (base == "cmakelists.txt") return "cmake";
    if (base == "makefile" || base == "gnumakefile") return "makefile";
    if (base == "dockerfile") return "dockerfile";
    if (base == "pkgbuild") return "sh";
    const auto dot = base.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= base.size()) return {};
    return base.substr(dot + 1);
}

std::string lang_for_tool(const ToolUse& tc) {
    for (const char* k : {"path", "file_path"}) {
        auto p = safe_arg(tc.args, k);
        if (auto l = lang_for_path(p); !l.empty()) return l;
    }
    // replace / rewrite_structural: `glob` like "*.cpp" or "src/**/*.ts".
    if (auto g = safe_arg(tc.args, "glob"); !g.empty()) {
        if (auto l = lang_for_path(g); !l.empty() && l.find_first_of("{}*,") == std::string::npos)
            return l;
    }
    // A diff body names its files: `+++ b/path` or `▏ path`.
    const std::string_view out = card_output(tc);
    for (std::string_view tag : {"+++ b/", "+++ ", "\xe2\x96\x8f "}) {
        if (auto p = out.find(tag); p != std::string_view::npos) {
            auto e = out.find('\n', p);
            auto path = out.substr(p + tag.size(),
                                   e == std::string_view::npos ? std::string_view::npos
                                                               : e - p - tag.size());
            if (auto l = lang_for_path(path); !l.empty()) return l;
        }
    }
    return {};
}

// Last resort for extensionless files: a shebang or an obvious first line.
static std::string lang_from_content(std::string_view s) {
    while (!s.empty() && (s.front() == '\n' || s.front() == ' ')) s.remove_prefix(1);
    const auto line = s.substr(0, s.find('\n'));
    if (line.starts_with("#!")) {
        if (line.find("python") != std::string_view::npos) return "py";
        if (line.find("node") != std::string_view::npos)   return "js";
        return "sh";
    }
    if (s.find("#include") != std::string_view::npos
     || s.find("#pragma once") != std::string_view::npos
     || s.find("namespace ") != std::string_view::npos)
        return "cpp";
    if (s.starts_with("use ") || s.find("\nfn ") != std::string_view::npos)
        return "rs";
    if (s.starts_with("package ")) return "go";
    if (s.starts_with("import ") || s.starts_with("def ")
     || s.find("\ndef ") != std::string_view::npos)
        return "py";
    // Lines ending in `;`, `{` or `}` read as C-family; the C++ tokeniser
    // handles C, Java, JS and friends well enough to colour them.
    int c_like = 0;
    for (std::size_t p = 0; p < s.size() && c_like < 2;) {
        auto e = s.find('\n', p);
        if (e == std::string_view::npos) e = s.size();
        auto l = s.substr(p, e - p);
        while (!l.empty() && (l.back() == ' ' || l.back() == '\r')) l.remove_suffix(1);
        if (!l.empty() && (l.back() == ';' || l.back() == '{' || l.back() == '}'))
            ++c_like;
        p = e + 1;
    }
    if (c_like >= 2) return "cpp";
    return {};
}

std::string lang_for_tool_or_content(const ToolUse& tc) {
    if (auto l = lang_for_tool(tc); !l.empty()) return l;
    for (const char* k : {"content", "new_text", "old_text"}) {
        if (auto l = lang_from_content(safe_arg(tc.args, k)); !l.empty())
            return l;
    }
    if (tc.args.is_object()) {
        if (auto it = tc.args.find("edits"); it != tc.args.end() && it->is_array())
            for (const auto& e : *it)
                if (e.is_object())
                    for (const char* k : {"new_text", "old_text"})
                        if (auto f = e.find(k); f != e.end() && f->is_string())
                            if (auto l = lang_from_content(f->get<std::string>()); !l.empty())
                                return l;
    }
    return lang_from_content(card_output(tc));
}

} // namespace agentty::ui::detail
