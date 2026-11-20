#include "agentty/runtime/view/status_bar/cache_chip.hpp"

#include <chrono>

namespace agentty::ui {

maya::CacheChip::Config cache_chip_config(const Model& m) {
    using V = StreamState::CacheVerdict;
    using S = maya::CacheChip::State;
    maya::CacheChip::Config cfg;
    const auto& s = m.s;
    switch (s.cache_verdict) {
    case V::None:
        break;
    case V::Hit:
        cfg.state   = S::Hit;
        cfg.hit_pct = s.cache_reuse_pct;
        cfg.tokens  = s.cache_read_tokens;
        break;
    case V::Warming:
        cfg.state  = S::Warming;
        cfg.tokens = s.cache_creation_tokens;
        break;
    case V::Expired:
        cfg.state     = S::Expired;
        cfg.idle_secs = static_cast<int>(s.cache_gap.count());
        break;
    case V::Miss:
        cfg.state  = S::Miss;
        // What was re-priced cold: the prompt minus whatever still hit.
        cfg.tokens = std::max(0, s.tokens_in - s.cache_read_tokens);
        break;
    case V::Unreported:
        cfg.state = S::Unreported;
        break;
    }
    return cfg;
}

} // namespace agentty::ui
