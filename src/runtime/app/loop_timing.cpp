// loop_timing.cpp — name the loop step that ate a frame.
//
// Lives outside the reducers on purpose: it reads the real clock, which the
// fold must not (elm_purity). Folds are timed by jaal's kernel (run_options
// below hands it agentty's budget and logger); program.hpp brackets view()
// and subscribe(), which are not folds, with note_slow().

#include "agentty/runtime/app/program.hpp"
#include "agentty/util/logx.hpp"

#include <string_view>

namespace agentty::app {

namespace {
// One frame at 60 Hz. Anything slower is visible as input lag.
constexpr auto kFrame = std::chrono::milliseconds(16);
}  // namespace

void note_slow(const char* what, std::chrono::steady_clock::time_point t0) noexcept {
    const auto dt = std::chrono::steady_clock::now() - t0;
    if (dt < kFrame) return;
    const double ms = std::chrono::duration<double, std::milli>(dt).count();
    AGT_LOG(Ui, Warn, "loop.slow", "{} {:.1f} ms", what, ms);
}

maya::run_options run_options() {
    maya::run_options opt;
    opt.kernel.fold_budget_time = kFrame;
    // jaal names the leaf the update() overload took; strip our namespace so
    // the line reads like the rest of the log ("msg::OpenPalette").
    opt.kernel.slow = [](std::string_view leaf, std::size_t, std::chrono::nanoseconds took) {
        constexpr std::string_view ns = "agentty::";
        if (leaf.starts_with(ns)) leaf.remove_prefix(ns.size());
        const double ms = std::chrono::duration<double, std::milli>(took).count();
        AGT_LOG(Ui, Warn, "loop.slow", "update {} {:.1f} ms", leaf, ms);
    };
    return opt;
}

}  // namespace agentty::app
