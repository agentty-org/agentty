#pragma once
#include <maya/widget/changes_strip.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

[[nodiscard]] maya::ChangesStrip::Config changes_strip_config(const Model& m);
// Rows the full strip's file list shows at once (its scroll page).
[[nodiscard]] int changes_strip_window(const Model& m);

} // namespace agentty::ui
