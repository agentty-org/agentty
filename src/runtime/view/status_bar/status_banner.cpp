#include "agentty/runtime/view/status_bar/status_banner.hpp"
#include <maya/core/anim_clock.hpp>   // anim_now: the frame clock

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

namespace agentty::ui {

namespace {

// Severity for a transient status message — the band's colour:
//   Error → red    (the turn or the action FAILED: "error: …", "giving up
//                   after …", "could not save …", "rewind failed: …")
//   Warn  → amber  (recoverable trouble: any retry, "awaiting …", "auth
//                   expired — refreshing…", "cannot X while …")
//   Info  → blue   (everything else: "context compacted", "copied", …)
//
// Retries key off status_is_retry (domain/session.hpp), the definition the
// reducer clears them by. The view's own prefix list drifted from it: a
// rate-limit retry read "rate_limit — retrying …", missed the "rate limit"
// prefix here, and painted the calm Info blue.
maya::StatusBanner::Kind classify(std::string_view s) noexcept {
    using K = maya::StatusBanner::Kind;
    constexpr std::string_view error_prefixes[] = {
        "error:", "giving up", "could not", "couldn't",
    };
    for (auto p : error_prefixes)
        if (s.starts_with(p)) return K::Error;
    if (s.find(" failed") != std::string_view::npos) return K::Error;

    if (status_is_retry(s)) return K::Warn;
    constexpr std::string_view warn_prefixes[] = {
        "awaiting", "auth expired", "cannot ",
    };
    for (auto p : warn_prefixes)
        if (s.starts_with(p)) return K::Warn;
    return K::Info;
}

// "… retrying in 21s …" → the live number, or "retrying now" once the wait
// is over. The reducer writes the delay ONCE, when it schedules the retry;
// left as written, the banner sat on "retrying in 21s" for 21 seconds and
// then on a number that was already in the past while the new attempt
// connected. Counting from the turn's own deadline (Active::retry_at) makes
// the line describe the present every frame.
//
// `secs_left` < 0 means "no wait left". Text without the phrase is
// returned unchanged — the countdown is a rendering of a deadline, never
// a reason to rewrite a message it can't find.
std::string with_countdown(std::string s, long secs_left) {
    constexpr std::string_view kIn = "retrying in ";
    const auto at = s.find(kIn);
    if (at == std::string::npos) return s;
    std::size_t d = at + kIn.size();
    std::size_t e = d;
    while (e < s.size() && s[e] >= '0' && s[e] <= '9') ++e;
    if (e == d || e >= s.size() || s[e] != 's') return s;
    if (secs_left > 0)
        s.replace(d, e - d, std::to_string(secs_left));
    else
        s.replace(at, e + 1 - at, "retrying now");
    return s;
}

} // namespace

// The toast band: the left of the status row while a status stands (maya
// keeps the context and cache gauges on its right).
//
// What it shows is decided against the turn's REAL state, not just the
// text's TTL — which is what let a banner outlive the thing it described:
//
//   * A retry line whose turn has ENDED is hidden. A retry describes an
//     in-flight turn by definition; once the phase is Idle it is history,
//     whatever its TTL says. The reducer also clears it on the first sign
//     of progress — this is the floor under every path that doesn't.
//   * While a retry waits on its backoff, the delay counts down live.
//
// "ready" is the calm idle pseudo-status and never strobes the band.
maya::StatusBanner::Config status_banner_config(const Model& m) {
    maya::StatusBanner::Config cfg;
    if (m.s.status.empty() || m.s.status == "ready") return cfg;
    const auto now = maya::anim_now();
    if (!m.s.status_active(now)) return cfg;

    const bool retry = status_is_retry(m.s.status);
    if (retry && m.s.is_idle()) return cfg;

    cfg.text = m.s.status;
    if (retry) {
        if (const auto* a = active_ctx(m.s.phase)) {
            if (m.s.in_scheduled() && a->retry_at.time_since_epoch().count() != 0) {
                const auto left = std::chrono::ceil<std::chrono::seconds>(
                    a->retry_at - now).count();
                cfg.text = with_countdown(std::move(cfg.text), static_cast<long>(left));
            } else if (!m.s.in_scheduled()) {
                // The wait is over and the next attempt is connecting.
                cfg.text = with_countdown(std::move(cfg.text), 0);
            }
        }
    }
    cfg.kind     = classify(m.s.status);
    cfg.is_error = (cfg.kind == maya::StatusBanner::Kind::Error);
    return cfg;
}

} // namespace agentty::ui
