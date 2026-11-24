#include "agentty/runtime/view/status_bar/stream_note.hpp"
#include <maya/core/anim_clock.hpp>   // anim_now: the frame clock

#include <algorithm>
#include <chrono>
#include <string>

#include "agentty/provider/selection.hpp"

namespace agentty::ui {

// The phase chip names the state in one verb; this says the rest — WHY, and
// what happens next — in the activity row's middle, so it never covers the
// gauges the way a toast did. Priority, highest first:
//
//   retry waiting   "HTTP 503: service unavailable · 2/6  ━━━━━╸────"
//                   The bar DRAINS over the backoff (full at the failure,
//                   empty when the next attempt fires); the chip beside it
//                   carries the number ("retry 7s"), so the note doesn't
//                   repeat it.
//   retry in flight "HTTP 503: service unavailable · 2/6 · trying again"
//   stalled         "no data for 47s · reconnecting"
//   buffered        "a proxy is holding the reply · it lands in one burst"
//   first token     "waiting for the first token" / "reading the prompt"
//                   (only once the wait is long enough to wonder about)
//
// A working stream gets no note: the sparkline is the narration then.
maya::StreamNote::Config stream_note_config(const Model& m) {
    using namespace std::chrono;
    maya::StreamNote::Config cfg;
    const auto* a = active_ctx(m.s.phase);
    if (!a || m.s.compacting) return cfg;
    const auto now = maya::anim_now();
    const auto warn = maya::Color::slot(maya::ThemeSlot::Warning);

    // The attempt number rides in the TAIL: the note truncates its text
    // from the end, and "2/6" is the part that must survive a narrow row.
    auto with_count = [&](std::string s) {
        cfg.tail = a->retry_count;
        return s;
    };

    if (m.s.in_scheduled() && !a->retry_why.empty()) {
        cfg.text = with_count(a->retry_why);
        cfg.tone = warn;
        if (a->retry_at.time_since_epoch().count() != 0
            && a->last_failure_at.time_since_epoch().count() != 0) {
            const double span = duration<double>(a->retry_at - a->last_failure_at).count();
            const double left = duration<double>(a->retry_at - now).count();
            if (span > 0.0)
                cfg.progress = static_cast<float>(std::clamp(left / span, 0.0, 1.0));
        }
        return cfg;
    }
    if (m.s.in_stall_fired()) {
        const auto quiet = duration_cast<seconds>(now - a->last_event_at).count();
        cfg.text = "no data for " + std::to_string(std::max<long long>(quiet, 0))
                 + "s \xc2\xb7 reconnecting";
        cfg.tone = maya::Color::slot(maya::ThemeSlot::Error);
        return cfg;
    }
    if (!a->retry_why.empty()) {
        cfg.text = with_count(a->retry_why + " \xc2\xb7 trying again");
        cfg.tone = warn;
        return cfg;
    }
    if (!m.s.is_streaming() || a->first_delta_at.time_since_epoch().count() != 0)
        return cfg;
    if (a->gateway_buffered) {
        cfg.text = "a proxy is holding the reply \xc2\xb7 it lands in one burst";
        cfg.tone = maya::Color::slot(maya::ThemeSlot::Info);
        return cfg;
    }
    // Under ~3 s nobody wonders; a note that flickers on every turn is noise.
    if (a->started.time_since_epoch().count() == 0 || now - a->started < seconds{3})
        return cfg;
    const auto& sel = m.d.selection;
    const bool local = sel.kind == provider::Kind::OpenAI && !sel.openai_endpoint.use_tls;
    cfg.text = local ? "the model is reading the prompt"
                     : "waiting for the first token";
    cfg.tone = maya::Color::slot(maya::ThemeSlot::Info);
    return cfg;
}

} // namespace agentty::ui
