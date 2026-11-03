#pragma once
// Memory-pressure release helper.
//
// Long sessions accumulate hundreds of MiB of small allocations: every
// tool result string, every Element-tree node in the view cache, every
// scratch JSON parsed off the wire.  When the user takes an action that
// frees a *large* contiguous chunk of that — compacting the conversation
// (drops every prior tool output), switching to / starting a new thread
// (releases the previous thread's view cache) — the bytes go back to the
// allocator's free list but not necessarily back to the kernel.  RSS stays
// high even though the program is logically much smaller.
//
// `release_to_kernel()` is the explicit nudge.  It is intentionally
// scoped: we don't call it on every Tick (would thrash the heap), only
// at the few coarse-grained boundaries where the user has just done
// something that frees megabytes of conversation state.
//
//   • mimalloc     → mi_collect(true): forces collection and asks the OS to
//                    reclaim unused pages. This is the default fetched path.
//   • glibc        → malloc_trim(0): fallback when mimalloc is disabled.
//   • other        → no-op on system allocators without a portable trim API.
//
// The function is declared `inline` and lives in this header so the
// platform branch is resolved at compile-time per TU; no link-time
// dispatch overhead.

#if defined(AGENTTY_USE_MIMALLOC)
#  include <mimalloc.h>
#elif defined(__GLIBC__)
#  include <malloc.h>
#endif

#include <cstddef>
#include <cstdlib>
#include <optional>

namespace agentty {

inline void release_to_kernel() noexcept {
#if defined(AGENTTY_USE_MIMALLOC)
    mi_collect(true);
#elif defined(__GLIBC__)
    ::malloc_trim(0);
#else
    // No-op on musl / macOS / Windows.
#endif
}

// Call once at startup. mimalloc keeps freed pages for 10 ms before giving
// them back, and pages freed on a worker thread wait for that thread to
// allocate again, so a burst of work (a long thread's scrollback replay)
// stayed resident. Giving them back at once measured no slower per frame.
// MIMALLOC_PURGE_DELAY still overrides it.
inline void tune_allocator() noexcept {
#if defined(AGENTTY_USE_MIMALLOC)
    if (!std::getenv("MIMALLOC_PURGE_DELAY"))
        mi_option_set(mi_option_purge_delay, 0);
#endif
}

// mimalloc exposes statistics through callback/printing APIs rather than a
// stable typed snapshot, so keep this portable helper explicitly unavailable.
struct memory_stats_unavailable_t {};
inline std::optional<memory_stats_unavailable_t> memory_stats() noexcept {
    return std::nullopt;
}

} // namespace agentty
