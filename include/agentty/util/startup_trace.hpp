// agentty/util/startup_trace.hpp — where launch time goes, by hand.
//
// AGENTTY_STARTUP_TRACE=<path> appends "+12.3 ms  name" lines to <path> at
// each mark, measured from process start (the first mark). stderr would be
// lost once the TUI owns the terminal, hence a file. Unset: one cached
// getenv, nothing else.
#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace agentty::startup {

inline std::FILE* sink() noexcept {
    static std::FILE* f = [] () -> std::FILE* {
        const char* p = std::getenv("AGENTTY_STARTUP_TRACE");
        return (p && *p) ? std::fopen(p, "a") : nullptr;
    }();
    return f;
}

inline void mark(const char* name) noexcept {
    std::FILE* f = sink();
    if (!f) return;
    using clock = std::chrono::steady_clock;
    static const clock::time_point t0 = clock::now();
    const double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    std::fprintf(f, "+%7.2f ms  %s\n", ms, name);
    std::fflush(f);
}

}  // namespace agentty::startup
