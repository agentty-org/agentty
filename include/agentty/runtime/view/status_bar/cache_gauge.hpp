#pragma once
#include <maya/widget/cache_gauge.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

// The prompt-cache verdict for the status bar, from StreamState's per-request
// decision (note_cache_usage). Hidden until there is something to say.
[[nodiscard]] maya::CacheGauge::Config cache_gauge_config(const Model& m);

} // namespace agentty::ui
