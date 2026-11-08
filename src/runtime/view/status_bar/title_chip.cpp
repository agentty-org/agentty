#include "agentty/runtime/view/status_bar/title_chip.hpp"

#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/palette.hpp"

namespace agentty::ui {

maya::TitleChip::Config title_chip_config(const Model& m) {
    maya::TitleChip::Config cfg;
    cfg.title      = m.d.current.title;
    cfg.edge_color = phase_color(m.s.phase);
    cfg.text_color = fg;
    // a cap, not a width: the status bar still fits it to the columns left
    // over, so a wide terminal shows the whole title instead of 28 chars
    cfg.max_chars  = 60;
    return cfg;
}

} // namespace agentty::ui
