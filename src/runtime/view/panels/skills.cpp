// skills.cpp — the read-only skills viewer.
//
// Pure adapter: builds maya::Panel::Config from Model state; the widget
// owns every chrome decision. Shared scaffolding: panels_prologue.hpp.
//
// ── What this panel is for ───────────────────────────────────────────────
//
// It answers four questions without the user leaving the TUI:
//
//   what is installed?          the list
//   what did it declare?        the effects chips
//   is it approved?             the trust marker
//   what did agentty notice?    the findings, in the detail footer
//
// It does NOT approve anything. That is a deliberate omission, not a gap:
// Anthropic measured 93% approval on Claude Code permission prompts, and
// an approve key one keystroke from a list you are already scrolling is
// the fastest possible route to a habituated yes. Approval lives behind
// `agentty skill approve NAME`, where the consent screen can show the
// findings AND the body.
//
// ── Row layout ───────────────────────────────────────────────────────────
//
//   leading    ! name            — the ! only when something needs a human
//   trailing   exec net · PENDING
//
// Colour carries severity, but never alone: every state also has a word or
// a glyph, because a colourblind user and a `theme::native` terminal both
// have to be able to read this.

#include "panels_prologue.hpp"

#include "agentty/runtime/panel/skills.hpp"
#include "agentty/tool/skills.hpp"

namespace agentty::ui {

Element skills_panel(const Model& m) {
    auto* o = m.ui.panel.get<pn::Skills>();
    if (!o) return nothing();

    Panel::Config cfg;
    cfg.title      = " Skills ";
    cfg.min_width  = kPanelStandard;
    cfg.scroll     = &o->scroll;
    cfg.selected   = o->rows.empty() ? -1 : o->index;

    // What this list is NOT showing, before the first row, one line per
    // cause with its own fix. A capped catalog and a small library look
    // identical from the rows alone, and the person scrolling is usually
    // scrolling because a skill went missing. Silence when everything on disk
    // is loaded — the healthy case needs no sentence.
    if (o->incomplete()) {
        for (const auto& p : tools::skills::catalog_problems(o->catalog))
            cfg.header.push_back(h(
                text("  "),
                text(p.what, p.warn ? fg_of(warn) : fg_of(fg)),
                text("  " + p.fix, fg_dim(muted))).build());
        cfg.header.push_back(text(""));
    }

    // The list budget is set at the END, once the header AND the detail
    // footer exist: both are rows this panel spends outside the list, and
    // the shared estimate only knows the two-row hint chrome.

    // Accent follows the worst state present, so the border itself carries
    // the headline before a single row is read.
    const auto flagged = o->flagged_count();
    const auto pending = o->pending_count();
    cfg.accent = flagged ? danger : (pending ? warn : highlight);

    // ── Detail for the selected row ──────────────────────────────────────
    // The list answers "what is installed"; the footer answers "what is
    // THIS one", so the user never has to leave the panel to find out why a
    // row is marked.
    cfg.footer.push_back(text(""));

    // A footer line with a hanging margin.
    //
    // `text("  " + sentence)` looks identical until the sentence is longer
    // than the panel — then the wrap drops the continuation at column 0, and
    // the reader sees a new entry where the rest of this one should be. The
    // longest strings here are exactly the ones that matter most (a finding's
    // detail, the path a skill came from), so the margin lives in its OWN
    // cell: the second cell wraps inside its column and the remainder stays
    // under the text it belongs to.
    const auto note = [&cfg](std::string content, Style sty) {
        cfg.footer.push_back(h(text("  "), text(std::move(content), sty)).build());
    };

    // Blank spacer rows, remembered so they can yield to content when the
    // terminal is too short (see the budget below). Breathing room is the
    // first thing a cramped panel should lose — never the detail itself.
    std::vector<std::size_t> spacers;
    const auto blank = [&cfg, &spacers] {
        spacers.push_back(cfg.footer.size());
        cfg.footer.push_back(text(""));
    };

    if (const auto* sel = o->selected()) {
        if (!sel->description.empty()) {
            // Already sanitised at scan time — safe to render inside our
            // own chrome. (A description that can open a new line can
            // impersonate the headings around it.)
            note(sel->description, fg_dim(muted));
            blank();
        }

        // Provenance. "you wrote this" vs "this arrived from elsewhere" is
        // the single most decision-relevant fact about a skill, so it gets
        // a plain sentence rather than a field label.
        if (!sel->origin.empty())
            note("from " + sel->origin, fg_dim(muted));
        else if (sel->source == "project")
            note("came with this repo", fg_dim(muted));
        else
            note("you wrote this", fg_dim(muted));

        note(sel->dir, fg_dim(muted));

        // A name collision hides a whole skill, so it is said plainly and
        // before the findings — the user cannot discover it any other way.
        if (sel->shadow_conflict) {
            blank();
            note("another skill in this scope declares the same name.", fg_of(danger));
            note("one of them is not loaded \xe2\x80\x94 agentty skill list",
                 fg_dim(muted));
        }

        // What screening saw. Capped at four lines: a footer nobody can
        // read is a footer nobody reads, and the CLI shows the full set.
        if (!sel->findings.empty()) {
            blank();
            cfg.footer.push_back(text("  agentty noticed:", fg_of(fg)));

            int shown = 0;
            for (const auto& f : sel->findings) {
                if (shown++ >= 4) break;
                const bool crit =
                    f.severity == tools::skills::Finding::Severity::Critical;
                std::string prefix = "   ";
                prefix += crit ? "!!" : " !";
                if (f.line > 0) prefix += " line " + std::to_string(f.line);
                // Marker and detail as two cells, the gap between them as
                // PADDING — a cell trims its own trailing spaces, so a gap
                // written into the prefix disappears the moment the row
                // wraps ("line 1downloads"). Padding survives, and a detail
                // long enough to wrap continues UNDER itself instead of
                // restarting at the left edge, where it reads as another
                // finding.
                //
                // The marker is NoWrap: it is a column, not prose, and a
                // narrow panel that wrapped it would split "line" from its
                // number. Known cosmetic drift: on a row whose detail holds
                // a wide glyph (an em dash) the gap can come out one column
                // short — that is maya's width table, not this layout.
                const Style sty = crit ? fg_of(danger) : fg_of(warn);
                cfg.footer.push_back(
                    h(text(prefix, sty, TextWrap::NoWrap),
                      (text(f.detail, sty) | padding(0, 0, 0, 2))).build());
            }
            if (sel->findings.size() > 4)
                note("\xe2\x80\xa6 " + std::to_string(sel->findings.size() - 4) +
                         " more \xe2\x80\x94 agentty skill list",
                     fg_dim(muted));

            // The honest caveat, every time findings are shown. Without it
            // an empty findings list reads as a clean bill of health.
            blank();
            note("pattern matching, not proof \xe2\x80\x94 and no findings"
                 " is not a clean bill.",
                 fg_dim(muted));
        }

        // The action, named but not bound. Saying WHERE the decision lives
        // is more useful than a key that makes it too easy.
        if (sel->gated && !sel->trusted) {
            blank();
            note("not approved \xe2\x80\x94 the agent cannot load it.", fg_of(warn));
            note("review and approve:  agentty skill approve " + sel->name,
                 fg_dim(muted));
        }
    } else {
        note("No skills installed.", fg_dim(muted));
        note("Drop a SKILL.md under ~/.agentty/skills/, or:", fg_dim(muted));
        note("agentty skill add ./path", fg_dim(muted));
    }

    // Summary line: the headline count, only when there is something to
    // count. Silence is the right output for a healthy set.
    if (flagged || pending) {
        blank();
        std::string sum = "  ";
        if (flagged)
            sum += std::to_string(flagged)
                 + (flagged == 1 ? " skill flagged" : " skills flagged");
        if (flagged && pending) sum += ", ";
        if (pending)
            sum += std::to_string(pending) + " awaiting approval";
        cfg.footer.push_back(text(std::move(sum),
                                  flagged ? fg_of(danger) : fg_of(warn)));
    }

    cfg.footer.push_back(key_hints({
        {"\xe2\x86\x91\xe2\x86\x93", "move", 5},   // ↑↓
        {"Esc", "close", 4},
    }));

    // ── The list gets what is LEFT ───────────────────────────────────────
    // Header and footer are rows this panel spends above and below the list,
    // and the shared budget only knows the two-row hint chrome. Counting
    // them here is what keeps the WHOLE panel inside the terminal: a panel
    // taller than the screen loses its bottom edge, and the bottom edge is
    // this footer — the description, the findings, the caveat. That was the
    // bug that made the detail look interleaved with the rows.
    //
    // The count is ELEMENTS, not wrapped rows: an estimate that errs toward a
    // shorter list rather than a spilled panel. And when even the 4-row list
    // floor does not fit, the blank spacers go first — a cramped panel should
    // lose its breathing room, not its detail.
    const auto budget = [&] {
        return panel_viewport_h(static_cast<int>(cfg.header.size() +
                                                cfg.footer.size()));
    };
    for (auto it = spacers.rbegin();
         it != spacers.rend() && budget() <= 4; ++it) {
        cfg.footer.erase(cfg.footer.begin() + static_cast<std::ptrdiff_t>(*it));
    }
    cfg.viewport_h = budget();

    cfg.items.reserve(o->rows.size());
    for (const auto& r : o->rows) {
        Panel::Item row;

        // A glyph, not just a colour: "!!" for something screening flagged,
        // "!" for unapproved-but-clean, two spaces for the ordinary case so
        // the names stay aligned in a monospace column.
        const char* mark = r.has_critical          ? "!! "
                         : r.shadow_conflict       ? "!! "
                         : (r.gated && !r.trusted) ? " ! "
                                                   : "   ";
        row.leading = std::string{mark} + r.name;
        row.leading_style = (r.has_critical || r.shadow_conflict) ? fg_of(danger)
                          : (r.gated && !r.trusted) ? fg_of(warn)
                                                    : fg_of(fg);

        // Trailing: what it declared, then its trust state. Prose skills
        // say "prose" rather than nothing — an empty column reads as
        // missing data, and "declares nothing" is a real answer.
        std::string decl = tools::skills::effects_to_frontmatter(r.effects);
        if (decl.empty()) decl = "prose";

        std::string state;
        if (r.gated) state = r.trusted ? "approved" : "PENDING";

        std::string trailing = decl;
        if (!state.empty()) trailing += " \xc2\xb7 " + state;   // ·
        row.trailing = std::move(trailing);
        row.trailing_style = (r.gated && !r.trusted) ? fg_of(warn)
                                                     : fg_dim(muted);

        cfg.items.push_back(std::move(row));
    }

    return Panel{std::move(cfg)}.build();
}

} // namespace agentty::ui
