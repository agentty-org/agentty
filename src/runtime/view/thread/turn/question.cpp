// The `question` card. See the header.
//
//   ╭─ ? Database · 1 of 2 ────────────────────────────────╮
//   │ Which database should the cache use?                 │
//   │                                                      │
//   │ › 1 ◉ SQLite     one file, no server                 │
//   │   2 ○ Postgres   the one the app already runs        │
//   │   ✎ type your own answer                             │
//   │                                                      │
//   │ ↑↓ move · enter choose · 1-2 pick · esc skip          │
//   ╰──────────────────────────────────────────────────────╯

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

Style fg(ThemeSlot s) { return Style{}.with_fg(Color::slot(s)); }

// One line built from styled pieces.
struct Line {
    std::string text;
    std::vector<StyledRun> runs;
    void put(std::string_view t, Style st) {
        if (t.empty()) return;
        runs.push_back(StyledRun{text.size(), t.size(), st});
        text += t;
    }
    Element build(TextWrap wrap = TextWrap::NoWrap) && {
        return Element{TextElement{.content = std::move(text), .style = {},
                                   .wrap = wrap, .runs = std::move(runs)}};
    }
};

Element plain(std::string s, Style st, TextWrap wrap = TextWrap::Wrap) {
    return Element{TextElement{.content = std::move(s), .style = st, .wrap = wrap}};
}

}  // namespace

Element question_card(const question::State& s) {
    std::vector<Element> rows;
    if (s.questions.empty()) return maya::dsl::v(std::move(rows)).build();
    const auto& q = s.q();
    const auto& a = s.a();
    const Color accent = Color::slot(ThemeSlot::Info);

    rows.push_back(plain(q.question, fg(ThemeSlot::Text).with_bold()));
    if (q.multiple)
        rows.push_back(plain("pick any that apply", fg(ThemeSlot::Muted)));
    rows.push_back(plain("", {}));

    // Options. The cursor row gets a caret and the accent; a chosen option a
    // filled marker. Numbers are the shortcut keys, shown only where they work.
    // Descriptions line up in one column after the longest label.
    int label_w = 0;
    for (const auto& o : q.options) label_w = std::max(label_w, maya::string_width(o.label));
    for (std::size_t i = 0; i < q.options.size(); ++i) {
        const bool here = !s.typing && s.cursor == i;
        const bool chosen = i < a.chosen.size() && a.chosen[i];
        Line l;
        l.put(here ? "\xe2\x80\xba " : "  ", Style{}.with_fg(accent).with_bold());   // ›
        l.put(i < 9 ? std::to_string(i + 1) + " " : "  ", fg(ThemeSlot::Muted));
        const char* mark = q.multiple ? (chosen ? "\xe2\x96\xa0 " : "\xe2\x96\xa1 ")   // ■ □
                                      : (chosen ? "\xe2\x97\x89 " : "\xe2\x97\x8b ");  // ◉ ○
        l.put(mark, chosen ? Style{}.with_fg(accent) : fg(ThemeSlot::Muted));
        Style label = here ? Style{}.with_fg(accent).with_bold() : fg(ThemeSlot::Text);
        if (chosen) label = label.with_bold();
        l.put(q.options[i].label, label);
        if (!q.options[i].description.empty()) {
            const int w = maya::string_width(q.options[i].label);
            l.put(std::string(static_cast<std::size_t>(label_w - w + 3), ' '), {});
            l.put(q.options[i].description, fg(ThemeSlot::Muted));
        }
        rows.push_back(std::move(l).build(TextWrap::Wrap));
    }

    // The free-text row, always last.
    {
        const bool here = s.on_text_row();
        Line l;
        l.put(here ? "\xe2\x80\xba " : "  ", Style{}.with_fg(accent).with_bold());
        l.put("\xe2\x9c\x8e ", here ? Style{}.with_fg(accent) : fg(ThemeSlot::Muted));   // ✎
        if (s.typing || !a.typed.empty()) {
            l.put(a.typed, fg(ThemeSlot::Text));
            if (s.typing) l.put("\xe2\x96\x8f", Style{}.with_fg(accent));               // ▏ caret
        } else {
            l.put("type your own answer", here ? Style{}.with_fg(accent) : fg(ThemeSlot::Muted));
        }
        rows.push_back(std::move(l).build(TextWrap::Wrap));
    }

    rows.push_back(plain("", {}));
    std::string keys;
    if (s.typing) {
        keys = "enter send \xc2\xb7 \xe2\x86\x91\xe2\x86\x93 back to the options \xc2\xb7 esc stop typing";
    } else {
        keys = "\xe2\x86\x91\xe2\x86\x93 move \xc2\xb7 ";
        keys += q.multiple ? "space tick \xc2\xb7 enter send" : "enter choose";
        if (!q.options.empty())
            keys += " \xc2\xb7 1-" + std::to_string(std::min<std::size_t>(q.options.size(), 9)) + " pick";
        if (s.current > 0) keys += " \xc2\xb7 shift+tab back";
        keys += " \xc2\xb7 esc skip";
    }
    rows.push_back(plain(std::move(keys), fg(ThemeSlot::Muted), TextWrap::NoWrap));

    // Title: the header (or "question"), and where we are when there are more.
    std::string title = " ? ";
    title += q.header.empty() ? std::string{"question"} : q.header;
    if (s.questions.size() > 1)
        title += " \xc2\xb7 " + std::to_string(s.current + 1) + " of " +
                 std::to_string(s.questions.size());
    title += " ";

    using namespace maya::dsl;
    return (v(std::move(rows))
            | border(maya::BorderStyle::Round)
            | bcolor(accent)
            | btext(std::move(title), maya::BorderTextPos::Top, maya::BorderTextAlign::Start)
            | padding(0, 1, 0, 1)).build();
}

} // namespace agentty::ui
