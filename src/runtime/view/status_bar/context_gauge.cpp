#include "agentty/runtime/view/status_bar/context_gauge.hpp"

#include <algorithm>

namespace agentty::ui {

maya::ContextGauge::Config context_gauge_config(const Model& m) {
    maya::ContextGauge::Config cfg;
    cfg.used     = m.s.tokens_in;
    cfg.max      = m.s.context_max;
    // The share of the prompt the provider read from its cache on the last
    // request — the gauge draws it textured (▓) so cached vs fresh context is
    // visible at a glance. Reset with tokens_in on a thread switch.
    cfg.cached   = std::min(m.s.cache_read_tokens, m.s.tokens_in);
    cfg.cells    = 10;
    cfg.show_bar = true;     // StatusBar overrides per-frame based on width
    return cfg;
}

} // namespace agentty::ui
