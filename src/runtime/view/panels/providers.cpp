// providers.cpp — the provider picker (^P) + accounts drill-down.
//
// Pure adapter: builds maya::Panel::Config from Model state; the widget
// owns every chrome decision. Shared scaffolding: panels_prologue.hpp.

#include "panels_prologue.hpp"

namespace agentty::ui {

Element providers_panel(const Model& m) {
    auto* picker = m.ui.panel.get<pn::Providers>();
    if (!picker) return nothing();

    Panel::Config cfg;
    cfg.title      = " Providers ";
    cfg.accent     = highlight;
    cfg.min_width  = kPanelStandard;
    cfg.viewport_h = panel_viewport_h();
    cfg.scroll     = &m.ui.provider_picker_scroll;
    cfg.selected   = picker->index;

    const std::string active_id = m.d.selection.catalog_id();

    // The one ordered, query-filtered row list — the SAME list the reducer
    // resolves a selection against (see build_provider_rows). The cursor is a
    // plain index into it; there is no offset math on either side.
    //
    // Off the RECORD, not the seam. view() is a pure function of the Model:
    // reaching through deps() here meant the same Model could paint two
    // different frames depending on what the store happened to hold, which
    // is also what makes visual_hash's cache-skip unsound. `m.d.persisted`
    // is the same settings the reducers read.
    const auto& settings = m.d.persisted;
    const std::vector<std::string> saved_custom_hosts =
        provider::saved_custom_hosts(settings.provider_keys);
    const auto sectioned = ui::build_sectioned_provider_rows(
        saved_custom_hosts, picker->query, settings, m.d.auth, active_id,
        picker->show_all);
    const auto& srows = sectioned.rows;

    // Live search header (mirrors the model picker). Backspace trims; typing
    // narrows. Hidden ACP/custom rows return the moment the query is cleared.
    cfg.header.push_back(filter_header(picker->query, "providers"));
    cfg.header.push_back(sep);

    // Trailing auth-status column for a built-in preset. One place, so every
    // provider's badge stays consistent.
    auto preset_note = [&](const provider::ProviderPreset& p, bool active)
        -> std::pair<std::string, maya::Color> {
        auto signed_badge = [&](std::string signed_label) {
            return std::pair<std::string, maya::Color>{
                active ? std::string{"\xe2\x9c\x93 signed in \xc2\xb7 accounts"}
                       : std::move(signed_label),
                active ? success : muted};
        };
        // OAuth providers whose token lives in their own transport: one
        // uniform status line, answered by the auth vault. Was three
        // near-identical name-keyed blocks calling three different
        // signed_in() probes; vault::signed_in dispatches over the same
        // table, so a new OAuth provider gets its status row for free.
        if (p.token_in_transport) {
            if (provider::signed_in(p.id, m.d.persisted, m.d.auth))
                return signed_badge(std::string{p.label} + " (signed in)");
            return {"\xe2\x9a\xa0 sign in with " + std::string{p.label}, warn};
        }
        if (p.is_local || p.auth == provider::AuthStyle::None)
            return {"\xe2\x97\x8f local", info};
        if (p.kind() == provider::Kind::Anthropic) {
            // The credential store, not the live header (do NOT read
            // deps().auth here), as loaded into the Model's AuthView.
            if (m.d.auth.stored(p.id))
                return {active ? "\xe2\x9c\x93 signed in \xc2\xb7 accounts" : "\xe2\x9c\x93 signed in", success};
            return {"\xe2\x9a\xa0 sign in", warn};
        }
        // Hosted API-key provider: distinguish a SAVED (pasted) key — which
        // ^D signs out — from an ENV key, which can't be removed in-app, so
        // the badge must not imply ^D will work on it.
        switch (provider::auth_source(p, settings, m.d.auth)) {
            case provider::AuthSource::Saved:
                return {active ? "\xe2\x9c\x93 signed in \xc2\xb7 key"
                               : "\xe2\x9c\x93 key saved", success};
            case provider::AuthSource::Env: {
                return {"\xe2\x97\x8f key from " + m.d.auth.env_var(p.id), info};
            }
            case provider::AuthSource::Local:
                return {"\xe2\x97\x8f local", info};
            case provider::AuthSource::None:
            default: break;
        }
        std::string_view want = p.auth_env.front();
        return {want.empty() ? "\xe2\x9a\xa0 no key" : "\xe2\x9a\xa0 " + std::string{want}, warn};
    };

    cfg.items.reserve(srows.size() + 3);
    std::optional<ui::ProviderSection> cur_section;
    int visual_selected = 0;
    // Per-section counts for the right-pinned stat on each divider.
    int yours_count = 0, more_count = 0;
    for (const auto& sr : srows) {
        if (sr.section == ui::ProviderSection::Yours) ++yours_count;
        else if (sr.section == ui::ProviderSection::More) ++more_count;
    }
    // Does any row actually carry the amber "no key" badge? Drives the footer
    // legend, which must only explain what's on screen. The signpost isn't
    // one: it's a dim action row, not a warning.
    const bool any_unconfigured = more_count > 0;

    // Name column width: the longest provider label actually on screen, so
    // every description starts at the SAME column and the list scans
    // vertically. Capped so one long custom-host spec can't push every
    // blurb off the right edge.
    int name_w = 0;
    for (const auto& sr : srows) {
        if (sr.more_signpost) continue;
        std::string nm;
        if (const auto* p = sr.row.preset())            nm = std::string{p->label};
        else if (const auto* a = sr.row.acp())          nm = a->id;
        else if (const auto* s = sr.row.custom_host())  nm = *s;
        name_w = std::max(name_w, static_cast<int>(maya::string_width(nm)));
    }
    name_w = std::min(name_w, 22);
    // Pad `nm` out to the name column. A name longer than the cap keeps its
    // full text and simply pushes its own description right -- truncating a
    // provider name to save alignment is the wrong trade.
    const auto pad_name = [&](const std::string& nm) {
        const int w = static_cast<int>(maya::string_width(nm));
        return nm + std::string(static_cast<std::size_t>(std::max(1, name_w - w + 2)), ' ');
    };

    for (std::size_t ri = 0; ri < srows.size(); ++ri) {
        const auto& sr = srows[ri];
        const auto& r  = sr.row;
        const int   i  = static_cast<int>(ri);

        // One divider per band. Only drawn when the band is non-empty, so a
        // fresh install (nothing configured) never shows an empty "YOURS".
        // The signpost stands alone -- no divider above a single row that is
        // itself a label.
        if ((!cur_section || *cur_section != sr.section)
            && sr.section != ui::ProviderSection::Signpost) {
            SectionHeader sh;
            switch (sr.section) {
                case ui::ProviderSection::Yours:
                    sh.label = sectioned.searching ? "configured" : "yours";
                    sh.hue   = accent;
                    sh.count = yours_count;
                    break;
                case ui::ProviderSection::More:
                    sh.label = "not configured";
                    sh.hue   = muted;
                    sh.count = more_count;
                    break;
                case ui::ProviderSection::Signpost:
                    break;                       // unreachable, guarded above
                case ui::ProviderSection::Add:
                    sh.label = "add";
                    sh.hue   = muted;
                    break;
            }
            cfg.items.push_back(section_header(std::move(sh)));
        }
        cur_section = sr.section;
        if (i == picker->index)
            visual_selected = static_cast<int>(cfg.items.size());

        Panel::Item row;

        // The "N more providers" signpost, rendered as a real row so Enter
        // reaches it. Dim and stated as an action, not a status.
        if (sr.more_signpost) {
            // An action, not a destination: a dim › marker in the badge
            // column distinguishes it from the provider rows above without
            // borrowing the ● that means "active".
            row.badge       = "\xe2\x80\xba";          // ›
            row.badge_style = fg_dim(muted);
            row.leading = "Show " + std::to_string(sectioned.n_hidden)
                        + " more providers";
            row.leading_style  = fg_dim(muted);
            row.trailing       = "Enter  \xc2\xb7  ^E";
            row.trailing_style = fg_dim(muted);
            row.trailing_secondary = true;
            cfg.items.push_back(std::move(row));
            continue;
        }

        if (const auto* p = r.preset()) {
            const bool active = (p->id == active_id);
            const bool confirming = (picker->confirm_remove == std::string{p->id});
            auto [note, note_color] = preset_note(*p, active);
            // ● in the badge column, like the model picker: the marker lives
            // in chrome the widget already reserves, so every provider name
            // starts at the same column instead of the active row being
            // indented one glyph out of line with its neighbours.
            //
            // A SPACE when inactive, not an empty string: maya only reserves
            // the badge cell for rows that set one, so leaving it empty
            // pulled every other row a glyph left and undid the alignment.
            row.badge       = active ? "\xe2\x97\x8f" : " ";   // ●
            row.badge_style = fg_of(accent);
            row.leading = pad_name(std::string{p->label})
                        + std::string{p->blurb};
            // Primary content renders at full foreground (same rule as the
            // model picker): dimming EVERY non-active row to make one stand
            // out inverts the hierarchy — it makes the whole list read as
            // unavailable while the trailing status chip out-shouts the name.
            //
            // An OFFER row is the one exception: it's a provider you haven't
            // set up, shown because you searched for it, so it reads dim and
            // states its single action instead of warning about a key you
            // never asked to have.
            row.leading_style  = sr.offer ? fg_dim(muted)
                                          : (active ? fg_bold(fg) : fg_of(fg));
            if (confirming) {
                // Del/d armed on a preset that has a saved key — second press
                // signs out (clears the key), the preset itself stays.
                row.trailing       = "\xe2\x9c\x97 press again to sign out";
                row.trailing_style = fg_of(warn);
            } else if (sr.offer) {
                row.trailing       = "Enter to set up";
                row.trailing_style = fg_dim(muted);
            } else {
                row.trailing       = note;
                row.trailing_style = fg_of(note_color);
            }
            row.active         = active;
        } else if (const auto* agent = r.acp()) {
            const bool active = (agent->id == active_id);
            row.badge       = active ? "\xe2\x97\x8f" : " ";   // ●
            row.badge_style = fg_of(accent);
            row.leading        = pad_name(agent->id)
                               + "external ACP agent (" + agent->command + ")";
            row.leading_style  = active ? fg_bold(fg) : fg_of(fg);
            row.trailing       = "agent";
            row.trailing_style = fg_dim(info);
            row.active         = active;
        } else if (const auto* spec = r.custom_host()) {
            const bool active = (*spec == active_id);
            const bool confirming = (picker->confirm_remove == *spec);
            row.badge       = active ? "\xe2\x97\x8f" : " ";   // ●
            row.badge_style = fg_of(accent);
            row.leading        = pad_name(*spec) + "custom OpenAI-compatible host";
            row.leading_style  = active ? fg_bold(fg) : fg_of(fg);
            if (confirming) {
                row.trailing       = "\xe2\x9c\x97 press again to remove";
                row.trailing_style = fg_of(warn);
            } else {
                // What the host last told us, not a blanket "ready": a key
                // it rejected is the thing the user came here to fix.
                std::string err;
                for (const auto& c : m.d.provider_catalogs)
                    if (c.provider_id == *spec) err = c.error;
                if (active && err.empty()) err = m.s.models_error;
                const bool auth_err = err.find("HTTP 401") != std::string::npos
                                   || err.find("HTTP 403") != std::string::npos;
                if (auth_err) {
                    row.trailing       = "\xe2\x9a\xa0 key rejected \xc2\xb7 Enter to fix";
                    row.trailing_style = fg_of(warn);
                } else if (!err.empty()) {
                    row.trailing       = "\xe2\x9a\xa0 couldn't list models";
                    row.trailing_style = fg_of(warn);
                } else {
                    row.trailing       = "\xe2\x9c\x93 ready";
                    row.trailing_style = fg_of(success);
                }
            }
            row.active         = active;
        } else if (const std::string* prefill = r.new_host_prefill()) {
            // PROMOTED sentinel: the query itself is endpoint-shaped, so this
            // row stops being a generic escape hatch and becomes a concrete
            // offer for the thing the user just typed. Full foreground, not
            // muted — muted is this TUI's vocabulary for "unavailable", and
            // reading the one row that solves your problem as disabled is
            // exactly how a user concludes agentty cannot reach their server.
            row.badge          = "+";
            row.badge_style    = fg_of(success);
            row.leading        = "Use " + *prefill + "  as a custom OpenAI-compatible host";
            row.leading_style  = fg_of(fg);
            row.trailing       = "\xe2\x8f\x8e connect";
            row.trailing_style = fg_of(info);
        } else {   // NewCustomHost sentinel, unpromoted
            // `+` marks the one row that CREATES a provider rather than
            // selecting one, so "add" is legible without reading the label.
            row.badge          = "+";
            row.badge_style    = fg_of(success);
            row.leading        = pad_name("Custom host\xe2\x80\xa6")
                               + "any OpenAI-compatible server (host:port)";
            row.leading_style  = fg_of(fg);
            row.trailing       = "\xe2\x9c\x8e edit";
            row.trailing_style = fg_dim(info);
        }
        // Under width pressure the STATUS chip gives way, not the provider
        // name — the name is what you select; "✓ signed in · accounts" is
        // reference data. Same policy as the command palette and the model
        // picker; maya's default (leading yields first) is for rows where the
        // trailing cell matters more, like the file picker's diffstat.
        row.trailing_secondary = true;
        cfg.items.push_back(std::move(row));
    }

    cfg.selected = visual_selected;

    // Enter opens the accounts drill-down on an account-capable OAuth
    // provider, otherwise it switches. Read straight off the highlighted row.
    const bool row_has_accounts = [&] {
        if (picker->index < 0 || picker->index >= static_cast<int>(srows.size()))
            return false;
        const auto& sr = srows[static_cast<std::size_t>(picker->index)];
        // An offer row's Enter starts sign-in, it doesn't drill into accounts.
        if (sr.offer) return false;
        const auto& row = sr.row;
        // Custom hosts hold multiple keys (accounts) — Enter drills in.
        if (const auto* spec = row.custom_host())
            return provider::credentials::add_method(*spec)
                   != provider::credentials::AddMethod::None;
        const auto* p = row.preset();
        if (!p) return false;
        // Every account-capable provider (OAuth + hosted API key) shows the
        // account drill-down on Enter; only keyless local servers don't.
        return provider::credentials::add_method(p->id)
               != provider::credentials::AddMethod::None;
    }();

    cfg.footer.push_back(text(""));

    // The footer's top line is CONTEXTUAL, because a static legend teaches
    // nothing at the moment a user is stuck. Three states:
    //
    //   typed an endpoint  → confirm what Enter will do with it
    //   typed a non-match  → say the generic path exists, in words, AT the
    //                        moment the list looks empty (this is where a
    //                        user otherwise concludes agentty can't reach
    //                        their server and goes to patch the registry)
    //   otherwise          → the badge legend
    const bool host_shaped = ui::query_looks_like_host(picker->query);
    const bool no_presets  = !picker->query.empty()
                           && provider::filter_provider_indices(picker->query).empty();
    if (host_shaped) {
        cfg.footer.push_back(h(
            text("\xe2\x8f\x8e", fg_of(info)),
            text(" connects to ", fg_dim(muted)),
            text(picker->query, fg_of(fg)),
            text(" \xc2\xb7 agentty probes it and adopts the dialect it finds",
                 fg_dim(muted))
        ).build());
    } else if (no_presets) {
        cfg.footer.push_back(h(
            text("no built-in provider matches ", fg_dim(muted)),
            text("\xe2\x80\x9c" + picker->query + "\xe2\x80\x9d", fg_of(fg)),
            text(" \xc2\xb7 type a ", fg_dim(muted)),
            text("host:port", fg_of(info)),
            text(" to use any OpenAI-compatible server", fg_dim(muted))
        ).build());
    } else {
        // The legend explains the badges that are ACTUALLY on screen. In the
        // browse view every row is usable, so there is nothing to explain;
        // expanded, the unconfigured rows say "Enter to set up" rather than
        // naming an env var, so the legend must say the same thing.
        if (any_unconfigured)
            cfg.footer.push_back(h(
                text("\xe2\x9c\x93", fg_of(success)), text(" ready  ", fg_dim(muted)),
                text("\xc2\xb7", fg_dim(muted)),
                text("  Enter on a dim row adds it", fg_dim(muted))
            ).build());
        else
            cfg.footer.push_back(h(
                text("\xe2\x9c\x93", fg_of(success)), text(" ready  ", fg_dim(muted))
            ).build());
    }
    std::vector<Hint> hints{
        {"\xe2\x86\x91\xe2\x86\x93", "move", 5},        // ↑↓
        {"type", host_shaped ? "endpoint" : "filter", 4},
        {"Enter", host_shaped ? "connect"
                              : (row_has_accounts ? "accounts" : "switch"), 5},
    };
    // Only advertise ^E when it would do something: while searching, every
    // row is already shown.
    if (!sectioned.searching && (sectioned.n_hidden > 0 || picker->show_all))
        hints.push_back({"^E", picker->show_all ? "fewer" : "all", 3});
    hints.push_back({"^D", picker->confirm_remove.empty() ? "remove" : "confirm", 2});
    hints.push_back({"^/", "models", 3});           // cross-hint: model picker
    hints.push_back({"Esc", "close", 4});
    cfg.footer.push_back(key_hints(std::move(hints)));

    return Panel{std::move(cfg)}.build();
}


} // namespace agentty::ui
