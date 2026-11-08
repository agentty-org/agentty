#include "agentty/runtime/view/diff_review.hpp"
#include "agentty/util/digits.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <maya/widget/markdown/highlight.hpp>
#include <maya/widget/tab_strip.hpp>
#include <maya/widget/tool_body_preview.hpp>   // diff bands, terminal_color_level
#include <maya/style/theme.hpp>

#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/hints.hpp"

namespace pn = agentty::ui::panel;

namespace agentty::ui {

using namespace maya;
using namespace maya::dsl;
namespace syntax = maya::syntax;

namespace {

// Red/green row bands, like the edit cards. Off below 256 colours, where
// the band colours can't be shown and plain signs read better.
[[nodiscard]] bool diff_bands_on() noexcept {
    return maya::terminal_color_level() >= 2;
}

// ── Language from a file path's extension ────────────────────────────────
[[nodiscard]] syntax::Lang lang_of(std::string_view path) {
    auto dot = path.rfind('.');
    if (dot == std::string_view::npos) return syntax::Lang::Generic;
    return syntax::lang_from_tag(path.substr(dot + 1));
}

// ── Status glyphs ────────────────────────────────────────────────────────
[[nodiscard]] const char* status_dot(Hunk::Status s) {
    switch (s) {
        case Hunk::Status::Accepted: return "\xe2\x97\x8f";  // ● filled
        case Hunk::Status::Rejected: return "\xe2\x9c\x97";  // ✗
        case Hunk::Status::Pending:  return "\xe2\x97\x8b";  // ○ hollow
    }
    return " ";
}
[[nodiscard]] Color status_color(Hunk::Status s) {
    switch (s) {
        case Hunk::Status::Accepted: return success;
        case Hunk::Status::Rejected: return danger;
        case Hunk::Status::Pending:  return warn;
    }
    return muted;
}

// A parsed diff line: its sign, the code text (sign stripped), and its old-
// and new-side line numbers (0 = none on that side).
struct DiffLine { char sign; std::string code; int oldno; int newno; };

// Lines in a file's text, the way an editor counts them.
[[nodiscard]] int line_count(const std::string& s) {
    int n = static_cast<int>(std::count(s.begin(), s.end(), '\n'));
    return (!s.empty() && s.back() != '\n') ? n + 1 : n;
}

// `old_lines`/`new_lines` drop the empty "line" diff::compute emits past a
// trailing newline; it isn't in the file.
[[nodiscard]] std::vector<DiffLine> parse_hunk(std::string_view patch,
                                               int old_lines = 1 << 30,
                                               int new_lines = 1 << 30,
                                               int old_start = 1, int new_start = 1) {
    std::vector<DiffLine> out;
    // diff::compute's patches carry no @@ header, so start from the hunk
    int oldno = old_start, newno = new_start;
    std::size_t i = 0;
    while (i < patch.size()) {
        std::size_t nl = patch.find('\n', i);
        std::string_view line = patch.substr(i, (nl == std::string_view::npos ? patch.size() : nl) - i);
        i = (nl == std::string_view::npos) ? patch.size() : nl + 1;
        if (line.empty()) continue;
        if (line.starts_with("@@")) {
            // Seed both counters from "-start" and "+start".
            auto seed = [&](char c) {
                int v = 0;
                if (auto p = line.find(c, 2); p != std::string_view::npos)
                    for (std::size_t k = p + 1; k < line.size() && line[k] >= '0' && line[k] <= '9'; ++k)
                        v = ::agentty::util::push_digit(v, line[k]);
                return v;
            };
            oldno = seed('-');
            newno = seed('+');
            continue;   // the @@ header itself isn't rendered as a code line
        }
        char sign = line[0];
        std::string code{line.substr(1)};
        int o = 0, n = 0;
        if (sign == '-' || sign == ' ') o = oldno++;
        if (sign == '+' || sign == ' ') n = newno++;
        const bool past_end = code.empty() && (o == 0 || o > old_lines) && (n == 0 || n > new_lines);
        if (past_end) continue;
        out.push_back({sign, std::move(code), o, n});
    }
    return out;
}

// Syntax-highlight one code line into styled runs, tinted for its diff sign.
// Added/removed lines keep syntax colours but carry a faint add/remove wash via
// the gutter + sign; context lines are dimmed so the eye tracks the changes.
[[nodiscard]] Element diff_code_line(const DiffLine& dl, syntax::Lang lang,
                                     int gutter_w) {
    const bool add = dl.sign == '+', del = dl.sign == '-';
    Color sign_c = add ? success : del ? danger : muted;

    // Build the WHOLE line as one string + per-byte-range StyledRuns, so it
    // renders as a SINGLE text element that clips its tail cleanly at the pane
    // width — an hstack of per-span text cells would instead squeeze every span
    // when the line overflows (mangled). This is how maya's DiffView stays
    // clean, applied with our syntax highlighting.
    std::string content;
    std::vector<StyledRun> runs;

    // Gutter: old and new line numbers, right-aligned, then the sign.
    auto num = [gutter_w](int n) {
        std::string s = n > 0 ? std::to_string(n) : "";
        s.insert(s.begin(), static_cast<std::size_t>(std::max(0, gutter_w - (int)s.size())), ' ');
        return s;
    };
    std::string gut = num(dl.oldno) + " " + num(dl.newno) + " ";
    const bool on_band = diff_bands_on() && (add || del);
    const maya::Color rail = add ? maya::diff_palette::add_rail : maya::diff_palette::rem_rail;
    const maya::Color rail_fg = add ? maya::diff_palette::add_fg_hi : maya::diff_palette::rem_fg_hi;
    runs.push_back({content.size(), gut.size(),
                    on_band ? Style{}.with_fg(rail_fg).with_bg(rail).with_bold()
                            : Style{}.with_fg(muted).with_dim()});
    content += gut;
    std::string sign_s = add ? "+ " : del ? "- " : "  ";
    runs.push_back({content.size(), sign_s.size(),
                    on_band ? Style{}.with_fg(rail_fg).with_bg(rail).with_bold()
                            : Style{}.with_fg(sign_c)});
    content += sign_s;

    // Syntax-highlighted code. On a band, each token colour is lifted until
    // it reads there; context lines render dimmed so the eye tracks changes.
    const std::size_t code_off = content.size();
    content += dl.code;
    auto spans = syntax::highlight(dl.code, lang);
    const auto& theme = syntax::themes::terminal;
    const bool banded = on_band;
    const auto& lt = maya::theme::live();
    const maya::LitColor band = lt.resolve(add ? maya::diff_palette::add_bg
                                               : maya::diff_palette::rem_bg);
    const maya::LitColor plain = lt.resolve(add ? maya::diff_palette::add_fg
                                                : maya::diff_palette::rem_fg);
    auto push = [&](std::size_t from, std::size_t to, syntax::Capture cap) {
        if (to <= from) return;
        Style st = theme.style_for(cap);
        if (banded) {
            maya::LitColor fg = st.fg ? maya::theme::measure(lt.resolve(*st.fg)) : plain;
            if (!fg.has_channels()) fg = plain;
            st = st.with_fg(maya::readable_on(fg, band, 6.0)).with_bg(band);
        } else if (dl.sign == ' ') {
            st = st.with_dim();
        }
        runs.push_back({code_off + from, to - from, st});
    };
    std::size_t pos = 0;
    for (const auto& sp : spans) {
        if (sp.start > pos) push(pos, sp.start, syntax::Capture::None);
        push(sp.start, std::min<std::size_t>(sp.start + sp.len, dl.code.size()),
             sp.cap);
        pos = sp.start + sp.len;
    }
    if (pos < dl.code.size()) push(pos, dl.code.size(), syntax::Capture::None);

    return Element{TextElement{.content = std::move(content),
                               .wrap = TextWrap::TruncateEnd,
                               .runs = std::move(runs)}};
}

} // namespace

Element diff_review(const Model& m) {
    auto rule = [](Color c) {
        return component([c](int w, int) -> Element {
            std::string s; s.reserve(static_cast<std::size_t>(std::max(0, w)) * 3);
            for (int i = 0; i < w; ++i) s += "\xe2\x94\x80";   // ─
            return text(std::move(s), fg_dim(c));
        });
    };
    auto* cursor = m.ui.panel.get<pn::DiffReview>();
    if (!cursor || m.d.pending_changes.empty()) return nothing();
    const int fidx = std::min<int>(cursor->file_index,
                                   static_cast<int>(m.d.pending_changes.size()) - 1);
    const auto& fc = m.d.pending_changes[static_cast<std::size_t>(fidx)];

    // ── Overall progress across every file's hunks ──
    int total_hunks = 0, decided = 0, accepted = 0, rejected = 0;
    for (const auto& f : m.d.pending_changes)
        for (const auto& hk : f.hunks) {
            ++total_hunks;
            if (hk.status != Hunk::Status::Pending) ++decided;
            if (hk.status == Hunk::Status::Accepted) ++accepted;
            if (hk.status == Hunk::Status::Rejected) ++rejected;
        }
    // NOTE: the pane can never sit at decided == total with work still
    // pending — the reducer commits and closes itself the moment the last
    // pending hunk is decided (auto-commit on final y/n), and the *All
    // handlers clear the queue wholesale. all_done is therefore transient at
    // worst, and the progress bar / border colour below read it only for the
    // one frame before the close lands.
    const bool all_done = (decided == total_hunks && total_hunks > 0);

    std::vector<Element> rows;

    // ── File rail: every file, its net status + diffstat, current one lit ──
    // Shows the WHOLE changeset at a glance (SOTA: you never lose the forest).
    //
    // maya::TabStrip owns the hard part: when the changeset is wider than the
    // pane the strip slides so the CURRENT file stays visible, and what it
    // scrolled past collapses into a leading "…". That used to be ~60 lines
    // of chunk-measuring and window-sliding here — the same logic the stats
    // viewer's tabs need, so it moved into the widget rather than being
    // written a second time. The per-file status dot and the +/- diffstat are
    // first-class fields on a tab precisely BECAUSE they must participate in
    // that width arithmetic; a host appending them itself would scroll by the
    // wrong amount and push the current file off the edge.
    {
        TabStrip rail;
        // Editor, not Dot: every tab already carries a status dot of its own
        // (pending / rejected / accepted), so Dot's leading ◆ selection
        // glyph put two dots on the same tab and the eye could not tell
        // which one meant "current". Editor separates the files with │ and
        // marks the live one with a FILLED CHIP in the accent, leaving the
        // per-file dot to mean status alone. A fill rather than weight
        // because the neighbours here are real filenames the user still
        // has to read — dimming them to make one look bright makes the
        // rest look disabled, when they are simply not the current file.
        // Both marks are single-row, so the pane's line budget is unchanged.
        rail.marker(TabMark::Editor);
        // Fill on every theme. TabStrip takes colours rather than a Style,
        // so it cannot use reverse video — keep inverse_text where the theme
        // owns a canvas, and fall back to the terminal's own background as
        // ink where it does not (native's background IS the honest inverse
        // of its foreground).
        rail.theme.active_bg = accent;
        rail.theme.active    = ui::theme_owns_canvas()
                                   ? maya::Color{ui::text_inverse}
                                   : maya::Color::slot(maya::ThemeSlot::Background);
        rail.theme.idle      = fg;
        rail.theme.accent    = accent;
        for (const auto& f : m.d.pending_changes) {
            bool anyrej = false, anypend = false;
            for (const auto& hk : f.hunks) {
                if (hk.status == Hunk::Status::Rejected) anyrej = true;
                if (hk.status == Hunk::Status::Pending)  anypend = true;
            }
            const Hunk::Status roll = anypend ? Hunk::Status::Pending
                                    : anyrej  ? Hunk::Status::Rejected
                                              : Hunk::Status::Accepted;
            std::string name = f.path;
            if (auto sl = name.rfind('/'); sl != std::string::npos)
                name = name.substr(sl + 1);
            rail.tab(std::move(name))
                .dot(status_color(roll), std::string(status_dot(roll)))
                .detail(std::format("+{} -{}", f.added, f.removed), success);
        }
        rail.active(fidx);
        rows.push_back(rail.build());
    }

    // ── Progress bar + counts ──
    {
        constexpr int kBarW = 18;
        int filled = total_hunks == 0 ? 0
                   : (decided * kBarW + total_hunks / 2) / total_hunks;
        std::string bar;
        for (int i = 0; i < kBarW; ++i) bar += (i < filled) ? "\xe2\x96\x88" : "\xe2\x96\x91";
        rows.push_back(h(
            text("  "),
            text(bar, all_done ? fg_of(success) : fg_of(accent)),
            text("  "),
            text(std::format("{}/{} reviewed", decided, total_hunks), fg_dim(muted)),
            spacer(),
            text(std::format("{} accepted", accepted), accepted ? fg_of(success) : fg_dim(muted)),
            text("  \xc2\xb7  ", fg_dim(muted)),
            text(std::format("{} rejected", rejected), rejected ? fg_of(danger) : fg_dim(muted)),
            text("  ")
        ).build());
    }
    rows.push_back(rule(muted));

    // ── Current file: the full path the rail shortened, and its diffstat ──
    {
        std::string dir, base = fc.path;
        if (auto sl = base.rfind('/'); sl != std::string::npos) {
            dir = base.substr(0, sl + 1);
            base = base.substr(sl + 1);
        }
        rows.push_back(h(
            text("  "),
            text(dir, fg_dim(muted)),
            text(base, fg_bold(fg)),
            text("  "),
            text(std::format("+{}", fc.added), fg_of(success)),
            text(" "),
            text(std::format("-{}", fc.removed), fg_of(danger)),
            spacer(),
            text(std::format("{} hunk{}", fc.hunks.size(), fc.hunks.size() == 1 ? "" : "s"),
                 fg_dim(muted)),
            text("  ")
        ).build());
        rows.push_back(text(""));
    }

    // ── The current file's hunks, syntax-highlighted ──
    const syntax::Lang lang = lang_of(fc.path);
    // Gutter width from the largest line number on either side.
    int max_ln = 1;
    for (const auto& hk : fc.hunks)
        max_ln = std::max({max_ln, hk.new_start + hk.new_len, hk.old_start + hk.old_len});
    const int gut_w = std::max(2, static_cast<int>(std::to_string(max_ln).size()));
    const int old_n = line_count(fc.original_contents);
    const int new_n = line_count(fc.new_contents);

    // Set inside the hunk loop when the FOCUSED hunk overflows its window —
    // gates the ^D/^U footer hint so it only appears when scrolling exists.
    bool focused_overflows = false;

    for (int hi = 0; hi < static_cast<int>(fc.hunks.size()); ++hi) {
        const auto& hk = fc.hunks[static_cast<std::size_t>(hi)];
        const bool sel = hi == cursor->hunk_index;

        // Per-hunk diffstat for the collapsed summary.
        int hadd = 0, hrem = 0;
        auto lines = parse_hunk(hk.patch, old_n, new_n, hk.old_start, hk.new_start);
        for (const auto& dl : lines) {
            if (dl.sign == '+') ++hadd; else if (dl.sign == '-') ++hrem;
        }

        // Hunk header: "▸ hunk N/M  ● status   +A -R    line L". The caret
        // marks the focused hunk; collapsed hunks show only this one line so
        // a huge changeset stays navigable instead of a 200-row wall.
        const int at = hk.new_start > 0 ? hk.new_start : hk.old_start;
        rows.push_back(h(
            text("  "),
            text(sel ? "\xe2\x96\xb8 " : "  ", sel ? fg_bold(accent) : fg_of(muted)),
            text(std::format("hunk {}/{}  ", hi + 1, fc.hunks.size()),
                 sel ? fg_bold(fg) : fg_dim(muted)),
            text(status_dot(hk.status), fg_of(status_color(hk.status))),
            text(" "),
            text(hk.status == Hunk::Status::Accepted ? "accepted"
               : hk.status == Hunk::Status::Rejected ? "rejected" : "pending ",
                 fg_of(status_color(hk.status))),
            text(std::format("   +{} ", hadd), fg_dim(success)),
            text(std::format("-{}", hrem), fg_dim(danger)),
            spacer(),
            text(std::format("line {}", at), fg_dim(muted)),
            text("  ")
        ).build());

        // Only the FOCUSED hunk expands. Others stay collapsed to the header
        // above — the SOTA large-review model (GitHub/Zed): one hunk in view,
        // j/k walks the list.
        if (!sel) continue;

        // Cap a single huge hunk's height so it can't blow past the viewport.
        // The window slides by the cursor's body_scroll (^D/^U, PgDn/PgUp);
        // "↑ N lines above" / "… N more" bracket it so nothing is silently
        // hidden and the user knows both directions exist.
        constexpr int kMaxHunkRows = 24;
        const int nlines = static_cast<int>(lines.size());
        // Clamp the offset precisely against the parsed rows (the reducer
        // clamps loosely against the raw patch's newline count).
        const int max_off = std::max(0, nlines - kMaxHunkRows);
        const int off = std::clamp(cursor->body_scroll, 0, max_off);
        focused_overflows = nlines > kMaxHunkRows;
        if (off > 0)
            rows.push_back(text(std::format(
                "      \xe2\x86\x91 {} lines above (^U)", off), fg_dim(muted)));
        int shown = 0;
        for (int li = off; li < nlines && shown < kMaxHunkRows; ++li, ++shown) {
            const auto& dl = lines[static_cast<std::size_t>(li)];
            // Same red/green bands as the edit cards, so a change reads the
            // same in review as it did in the transcript.
            const bool diff_bands = diff_bands_on();
            auto row = h(diff_code_line(dl, lang, gut_w)) | grow(1.0f);
            if (diff_bands && dl.sign == '+')
                row = std::move(row) | bgc(maya::diff_palette::add_bg);
            else if (diff_bands && dl.sign == '-')
                row = std::move(row) | bgc(maya::diff_palette::rem_bg);
            rows.push_back((h(std::move(row)) | padding(0, 0, 0, 2)).build());
        }
        if (off + shown < nlines)
            rows.push_back(text(std::format(
                "      \xe2\x80\xa6 {} more lines in this hunk (^D)",
                nlines - off - shown), fg_dim(muted)));
        rows.push_back(text(""));
    }
    rows.push_back(rule(muted));

    // ── Footer: phone-friendly keys, destructive actions tinted ──
    // ARMED ^X: replace the whole hint strip with an unmissable warning — the
    // next ^X reverts everything; any other key cancels.
    if (cursor->confirm_reject_all) {
        rows.push_back(h(
            text("  \xe2\x9a\xa0 ", fg_bold(danger)),
            text("^X again reverts ALL changes on disk ", fg_bold(danger)),
            text("\xc2\xb7 any other key cancels", fg_dim(muted))
        ).build());
    } else {
        std::vector<Hint> hints = {
            {"↑↓", "hunk", 6},
            {"←→", "file", 5, m.d.pending_changes.size() > 1 ? fg : muted},
            {"y", "accept", 7, success},
            {"n", "reject", 7, danger},
            {"^A", "all",  4, success},
            {"^X", "none", 3, danger},
        };
        if (focused_overflows)
            hints.push_back({"^D/^U", "scroll", 2});
        // Esc COMMITS: undecided hunks are kept (already live on disk), not
        // discarded. "keep rest" says that; a bare "close" reads like cancel.
        // There is deliberately no "all reviewed" case: the pane closes ITSELF
        // when the last pending hunk is decided, so Esc never has to mean two
        // opposed things (apply-at-end vs keep-the-rest mid-review) — the old
        // final-Esc affordance trained exactly that contradiction.
        hints.push_back({"Esc",
                         decided == 0 ? "keep all" : "keep rest",
                         8, fg});
        rows.push_back(key_hints(std::move(hints)));
    }

    auto content = (v(std::move(rows)) | padding(1, 2));
    return (v(content.build())
            | border(BorderStyle::Round)
            | bcolor(all_done ? success : muted)
            | btext(" Review Changes ", BorderTextPos::Top, BorderTextAlign::Center)
            ).build();
}

} // namespace agentty::ui
