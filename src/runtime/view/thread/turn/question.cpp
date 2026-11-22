// The `question` card. See the header.
//
//   ╭─ ? Asking you ────────────────────────────── 1 of 2 ─╮
//   │  ● Database   ○ Scope   ○ Review                     │
//   │                                                      │
//   │  Which database should the cache use?                │
//   │                                                      │
//   │ ▌ 1  ◉ SQLite                          ★ suggested   │  ← cursor row, shaded
//   │        one file, nothing to run                      │
//   │   2  ○ Postgres                                      │
//   │        the server the app already uses               │
//   │   ✎  or type your own answer below                   │
//   │                                                      │
//   │  enter select · ↑↓ move · 1-2 pick · esc stop          │
//   ╰──────────────────────────────────────────────────────╯
//
// With several questions the tab row says where you are and which are
// answered (●); after the last one a Review page shows every answer and
// Enter sends them. A free answer is typed in the composer under the card,
// the box the user already types into; Enter with text there sends it.

#include "agentty/runtime/view/thread/turn/question.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <maya/dsl.hpp>
#include <maya/element/text.hpp>   // string_width
#include <maya/style/border.hpp>
#include <maya/style/style.hpp>
#include <maya/style/theme.hpp>

namespace agentty::ui {

namespace {

using maya::Color;
using maya::Element;
using maya::Style;
using maya::StyledRun;
using maya::TextElement;
using maya::TextWrap;
using maya::ThemeSlot;

constexpr const char* kDot    = " \xc2\xb7 ";           // ·
constexpr const char* kBar    = "\xe2\x96\x8c";         // ▌ cursor bar

Style fg(ThemeSlot s) { return Style{}.with_fg(Color::slot(s)); }
Style fgc(Color c) { return Style{}.with_fg(c); }

// One line built from styled pieces.
struct Line {
    std::string text;
    std::vector<StyledRun> runs;
    Line& put(std::string_view t, Style st) {
        if (t.empty()) return *this;
        runs.push_back(StyledRun{text.size(), t.size(), st});
        text += t;
        return *this;
    }
    [[nodiscard]] int width() const { return maya::string_width(text); }
    Element build(TextWrap wrap = TextWrap::Wrap) && {
        return Element{TextElement{.content = std::move(text), .style = {},
                                   .wrap = wrap, .runs = std::move(runs)}};
    }
};

Element blank() { return Element{TextElement{.content = ""}}; }

// A row shaded across the full card width: the cursor row. Built at render
// time so the band reaches the border whatever the card's width.
Element shaded(Line l, Color shade) {
    for (auto& r : l.runs) r.style = r.style.with_bg(shade);
    return Element{maya::ComponentElement{
        .render = [l = std::move(l), shade](int w, int) -> Element {
            std::string s = l.text;
            auto runs = l.runs;
            if (const int used = maya::string_width(s); used < w) {
                runs.push_back({s.size(), static_cast<std::size_t>(w - used), Style{}.with_bg(shade)});
                s.append(static_cast<std::size_t>(w - used), ' ');
            }
            return Element{TextElement{.content = std::move(s), .style = Style{}.with_bg(shade),
                                       .wrap = TextWrap::NoWrap, .runs = std::move(runs)}};
        },
        .measure = [](int mw) -> maya::Size { return maya::Size{maya::Columns(mw), maya::Rows(1)}; },
    }};
}

// What the user picked for question i, in a few words ("" = nothing yet).
std::string answer_text(const question::State& s, std::size_t i) {
    const auto& q = s.questions[i];
    const auto& a = s.answers[i];
    std::string out;
    for (std::size_t k = 0; k < q.options.size(); ++k)
        if (k < a.chosen.size() && a.chosen[k]) {
            if (!out.empty()) out += ", ";
            out += q.options[k].label;
        }
    if (!a.typed.empty()) {
        if (!out.empty()) out += ", ";
        out += "\xe2\x80\x9c" + a.typed + "\xe2\x80\x9d";   // “…”
    }
    return out;
}

std::string short_name(const question::Question& q, std::size_t i) {
    if (!q.header.empty()) return q.header;
    return "Q" + std::to_string(i + 1);
}

// ● Database   ○ Scope   ○ Review — where you are, and what is answered.
Element tab_row(const question::State& s, Color accent) {
    Line l;
    l.put(" ", {});
    auto tab = [&](std::string name, bool here, bool done) {
        l.put(done ? "\xe2\x97\x8f " : "\xe2\x97\x8b ",   // ● ○
              done ? fg(ThemeSlot::Success) : fg(ThemeSlot::Muted));
        l.put(name, here ? fgc(accent).with_bold().with_underline()
                         : (done ? fg(ThemeSlot::Text) : fg(ThemeSlot::Muted)));
        l.put("   ", {});
    };
    for (std::size_t i = 0; i < s.questions.size(); ++i)
        tab(short_name(s.questions[i], i), !s.reviewing && s.current == i, s.answers[i].answered);
    tab("Review", s.reviewing, false);
    return std::move(l).build(TextWrap::NoWrap);
}

// The hint row: the keys that do something HERE, primary first. Wraps
// between hints (never inside one) when the card is narrow, so "esc stop"
// is never cut off on a small terminal.
Element keys_row(std::vector<std::pair<std::string, std::string>> keys) {
    std::vector<std::pair<std::string, std::string>> owned = std::move(keys);
    return Element{maya::ComponentElement{
        .render = [keys = owned](int w, int) -> Element {
            std::vector<Element> lines;
            Line l;
            l.put(" ", {});
            for (std::size_t i = 0; i < keys.size(); ++i) {
                const int need = maya::string_width(keys[i].first) + 1 +
                                 maya::string_width(keys[i].second) + (i ? 3 : 0);
                if (i && w > 0 && l.width() + need > w) {
                    lines.push_back(std::move(l).build(TextWrap::NoWrap));
                    l = Line{};
                    l.put(" ", {});
                } else if (i) {
                    l.put(kDot, fg(ThemeSlot::Muted));
                }
                l.put(keys[i].first, fg(ThemeSlot::Text).with_bold());
                l.put(" " + keys[i].second, fg(ThemeSlot::Muted));
            }
            lines.push_back(std::move(l).build(TextWrap::NoWrap));
            return maya::dsl::v(std::move(lines)).build();
        },
        .measure = [keys = owned](int mw) -> maya::Size {
            int rows = 1, used = 1;
            for (std::size_t i = 0; i < keys.size(); ++i) {
                const int need = maya::string_width(keys[i].first) + 1 +
                                 maya::string_width(keys[i].second) + (i ? 3 : 0);
                if (i && mw > 0 && used + need > mw) { ++rows; used = 1 + need - 3; }
                else used += need;
            }
            return maya::Size{maya::Columns(mw), maya::Rows(rows)};
        },
    }};
}

void question_page(const question::State& s, bool drafting, std::vector<Element>& rows,
                   Color accent) {
    const auto& q = s.q();
    const auto& a = s.a();
    const Color shade = Color::slot(ThemeSlot::Selection);

    Line ql;
    ql.put(" ", {}).put(q.question, fg(ThemeSlot::Text).with_bold());
    rows.push_back(std::move(ql).build());
    if (q.multiple) {
        Line sub;
        sub.put(" ", {}).put("choose any that apply", fg(ThemeSlot::Muted).with_italic());
        rows.push_back(std::move(sub).build());
    }
    rows.push_back(blank());

    // Labels share one column so the ★ tags line up.
    int label_w = 0;
    for (const auto& o : q.options) label_w = std::max(label_w, maya::string_width(o.label));

    for (std::size_t i = 0; i < q.options.size(); ++i) {
        const auto& o = q.options[i];
        // While a typed answer is being drafted, Enter sends that instead,
        // so the cursor row stops claiming it.
        const bool here = !drafting && s.cursor == i;
        const bool chosen = i < a.chosen.size() && a.chosen[i];
        Line l;
        l.put(here ? kBar : " ", fgc(accent));
        l.put(i < 9 ? " " + std::to_string(i + 1) + "  " : "    ",
              here ? fgc(accent) : fg(ThemeSlot::Muted));
        const char* mark = q.multiple ? (chosen ? "\xe2\x96\xa3 " : "\xe2\x96\xa1 ")   // ▣ □
                                      : (chosen ? "\xe2\x97\x89 " : "\xe2\x97\x8b ");  // ◉ ○
        l.put(mark, chosen ? fg(ThemeSlot::Success).with_bold()
                           : (here ? fgc(accent) : fg(ThemeSlot::Muted)));
        Style label = here || chosen ? fg(ThemeSlot::Text).with_bold() : fg(ThemeSlot::Text);
        l.put(o.label, label);
        if (o.recommended) {
            l.put(std::string(static_cast<std::size_t>(label_w - maya::string_width(o.label) + 3), ' '), {});
            l.put("\xe2\x98\x85 suggested", fg(ThemeSlot::Warning));                     // ★
        }
        rows.push_back(here ? shaded(std::move(l), shade) : std::move(l).build(TextWrap::NoWrap));
        if (!o.description.empty()) {
            Line d;
            d.put("        ", {}).put(o.description, fg(ThemeSlot::Muted));
            rows.push_back(std::move(d).build());
        }
    }

    // Something else: typed in the composer below. Shown as a hint row, and as
    // the answer once one was given (coming back to this question).
    {
        Line l;
        l.put(" ", {});
        l.put(" \xe2\x9c\x8e  ", drafting ? fgc(accent) : fg(ThemeSlot::Muted));      // ✎
        if (!a.typed.empty() && !drafting) {
            l.put("\xe2\x80\x9c" + a.typed + "\xe2\x80\x9d", fg(ThemeSlot::Text).with_bold());
        } else if (drafting) {
            l.put("enter sends what you typed below as the answer", fgc(accent));
        } else {
            l.put(q.options.empty() ? "type your answer below"
                                    : "or type your own answer below",
                  fg(ThemeSlot::Muted).with_italic());
        }
        rows.push_back(std::move(l).build());
    }
    rows.push_back(blank());

    std::vector<std::pair<std::string, std::string>> keys;
    if (drafting) {
        keys = {{"enter", "send as answer"}};
        if (s.questions.size() > 1) keys.push_back({"tab", "next"});
        keys.push_back({"esc", "stop"});
    } else {
        if (q.options.empty())  keys = {};
        else if (q.multiple)    keys = {{"space", "toggle"}, {"enter", "done"}};
        else                    keys = {{"enter", "select"}};
        if (!q.options.empty()) {
            keys.push_back({"\xe2\x86\x91\xe2\x86\x93", "move"});
            keys.push_back({"1-" + std::to_string(std::min<std::size_t>(q.options.size(), 9)), "pick"});
        }
        if (s.questions.size() > 1) keys.push_back({"tab", "next"});
        keys.push_back({"esc", "stop"});
    }
    rows.push_back(keys_row(std::move(keys)));
}

void review_page(const question::State& s, std::vector<Element>& rows, Color accent) {
    Line t;
    t.put(" ", {}).put("Ready to send", fg(ThemeSlot::Text).with_bold());
    rows.push_back(std::move(t).build());
    rows.push_back(blank());
    int name_w = 0;
    for (std::size_t i = 0; i < s.questions.size(); ++i)
        name_w = std::max(name_w, maya::string_width(short_name(s.questions[i], i)));
    for (std::size_t i = 0; i < s.questions.size(); ++i) {
        const auto name = short_name(s.questions[i], i);
        const auto ans = answer_text(s, i);
        Line l;
        l.put(" " + std::to_string(i + 1) + "  ", fg(ThemeSlot::Muted));
        l.put(name, fg(ThemeSlot::Muted));
        l.put(std::string(static_cast<std::size_t>(name_w - maya::string_width(name) + 2), ' '), {});
        l.put("\xe2\x86\x92 ", fgc(accent));                                              // →
        l.put(ans.empty() ? std::string{"(no answer)"} : ans,
              ans.empty() ? fg(ThemeSlot::Muted).with_italic() : fg(ThemeSlot::Text).with_bold());
        rows.push_back(std::move(l).build());
    }
    rows.push_back(blank());
    rows.push_back(keys_row({{"enter", "send answers"},
                             {"1-" + std::to_string(s.questions.size()), "change one"},
                             {"esc", "back"}}));
}

}  // namespace

Element question_card(const question::State& s, bool drafting) {
    std::vector<Element> rows;
    if (s.questions.empty()) return maya::dsl::v(std::move(rows)).build();
    const Color accent = Color::slot(ThemeSlot::Info);
    const bool many = s.questions.size() > 1;

    if (many) {
        rows.push_back(tab_row(s, accent));
        rows.push_back(blank());
    }
    if (s.reviewing) review_page(s, rows, accent);
    else             question_page(s, drafting, rows, accent);

    // One height for every page. Moving to a shorter question (or the review)
    // used to shrink the card under a live frame, and the rows it gave up
    // were left blank between the card and the message box. The body is
    // padded to the tallest page instead, measured at the card's width, so
    // stepping through the questions never moves anything else on screen.
    {
        const int inner = std::max(1, maya::available_width() - 6);
        auto height_of = [&](auto&& build) {
            std::vector<Element> page;
            if (many) { page.push_back(tab_row(s, accent)); page.push_back(blank()); }
            build(page);
            int h = 0;
            for (const auto& e : page) h += maya::measure_element(e, inner).height.value;
            return h;
        };
        int tallest = 0;
        for (std::size_t i = 0; i < s.questions.size(); ++i) {
            question::State probe = s;
            probe.current = i;
            probe.reviewing = false;
            tallest = std::max(tallest, height_of([&](auto& p) {
                question_page(probe, drafting, p, accent); }));
        }
        if (many)
            tallest = std::max(tallest, height_of([&](auto& p) { review_page(s, p, accent); }));
        int have = 0;
        for (const auto& e : rows) have += maya::measure_element(e, inner).height.value;
        // Padding goes ABOVE the hint row, so the keys stay at the bottom.
        if (have < tallest && !rows.empty()) {
            auto hint = std::move(rows.back());
            rows.pop_back();
            for (int k = have; k < tallest; ++k) rows.push_back(blank());
            rows.push_back(std::move(hint));
        }
    }

    // Title: what this is. Progress is the tab row's job.
    std::string title = " ? ";
    title += many ? std::string{"A few questions"}
                  : (s.q().header.empty() ? std::string{"A question"} : s.q().header);
    title += " ";

    using namespace maya::dsl;
    return (v(std::move(rows))
            | border(maya::BorderStyle::Round)
            | bcolor(accent)
            | btext(std::move(title), maya::BorderTextPos::Top, maya::BorderTextAlign::Start)
            | padding(0, 1, 0, 0)).build();
}

} // namespace agentty::ui
