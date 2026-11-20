#include "agentty/runtime/view/status_bar/cache_gauge.hpp"

namespace agentty::ui {

// One percentage for every verdict; the state only picks its colour.
maya::CacheGauge::Config cache_gauge_config(const Model& m) {
    using V = StreamState::CacheVerdict;
    using S = maya::CacheGauge::State;
    maya::CacheGauge::Config cfg;
    cfg.pct = m.s.cache_reuse_pct;
    switch (m.s.cache_verdict) {
    case V::None:       cfg.state = S::Hidden;     break;
    case V::Hit:        cfg.state = S::Hit;        break;
    case V::Warming:    cfg.state = S::Warming;    break;
    case V::Expired:    cfg.state = S::Expired;    break;
    case V::Miss:       cfg.state = S::Miss;       break;
    case V::Unreported: cfg.state = S::Unreported; break;
    }
    return cfg;
}

} // namespace agentty::ui
