#include "agentty/runtime/view/changes_strip.hpp"

#include <algorithm>

#include "agentty/runtime/view/palette.hpp"

namespace agentty::ui {

int changes_strip_window(const Model& m) {
    // Eight rows on a normal terminal; never more than a quarter of the
    // screen, so even the full strip leaves the transcript the room.
    const int rows = m.ui.term_rows > 0 ? m.ui.term_rows : 40;
    return std::clamp(rows / 4, 3, 8);
}

maya::ChangesStrip::Config changes_strip_config(const Model& m) {
    maya::ChangesStrip::Config cfg;
    cfg.border_color = warn;
    cfg.text_color   = fg;
    cfg.accept_color = success;
    cfg.reject_color = danger;
    // Hidden by setting (Ctrl+K → "Changes strip") — edits still queue for
    // Ctrl+R review, just without the always-on banner.
    if (!m.d.show_changes_strip) return cfg;
    if (m.d.pending_changes.empty()) return cfg;
    cfg.compact  = !m.d.changes_strip_full;
    cfg.max_rows = changes_strip_window(m);
    cfg.scroll   = m.d.changes_strip_scroll;
    // Paths relative to where agentty runs: the workspace prefix on every
    // row is the same 30 columns of nothing (the screenshot was a column of
    // "…/Projects/BenchManager/").
    cfg.root     = m.env.cwd;

    cfg.changes.reserve(m.d.pending_changes.size());
    for (const auto& c : m.d.pending_changes) {
        cfg.changes.push_back({
            .path          = c.path,
            .kind          = c.original_contents.empty()
                               ? maya::FileChangeKind::Created
                               : maya::FileChangeKind::Modified,
            .lines_added   = c.added,
            .lines_removed = c.removed,
        });
    }
    return cfg;
}

} // namespace agentty::ui
