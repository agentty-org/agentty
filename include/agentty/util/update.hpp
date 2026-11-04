#pragma once
// agentty::update — self-update against GitHub releases.
//
// Design goals, in order:
//   1. NEVER slow anything down. The TUI check runs on a background thread,
//      deferred past first paint, rate-limited by a 24 h on-disk cache, and
//      its only effect is one Msg dispatch. No network on the render path.
//   2. Make updating trivial. `agentty update` from any shell; `/update` in
//      the palette (surfaced ONLY when an update is actually available);
//      an unobtrusive "⬆ v0.3.1" chip in the status bar as the signal.
//   3. Fail safe. Download to a sibling temp file, verify, atomic rename
//      over the current binary (the running process keeps its old image —
//      classic Unix; on Windows the old file is moved aside first). Any
//      failure leaves the installed binary untouched.
//
// Channel: the GitHub releases of agentty-org/agentty, single-file binaries named
// agentty-<os>-<arch>[.exe]. Package-manager installs (rpm/pacman/ebuild)
// are detected by install-path heuristics and get a "use your package
// manager" message instead of a self-replace.

#include <functional>
#include <optional>
#include <string>

namespace agentty::update {

// Result of a version check.
//
// TWO versions, on purpose. `latest` is the newest release GitHub calls
// latest; `latest` may not be INSTALLABLE yet, because the release workflow
// streams: the release page and source tarball go up first, then each
// platform's binary lands over the next ~35-80 minutes (#119). During that
// window "v0.9.46 is out" is true and "you can update to v0.9.46" is not.
//
// So `update_available` and `target` describe what THIS platform can actually
// install -- the newest release, among the last few, that ships our asset --
// and `pending` says the newest one exists but its binary is still building.
// A client never offers an update it cannot perform.
struct CheckResult {
    bool        update_available = false;  // `target` is newer than `current` AND installable
    std::string current;        // e.g. "0.3.0"
    std::string latest;         // newest release, installable or not (empty on error)
    std::string target;         // newest INSTALLABLE release; empty when none is newer
    std::string url;            // browser URL of the `target` release page (or latest's)
    std::string asset_sha256;   // GitHub's digest for target's asset, hex; may be empty
    bool        pending = false;  // `latest` is newer but its binary for us isn't up yet
    std::string error;          // non-empty ⇒ the check itself failed
};

// Compare two dotted versions ("0.3.0" vs "0.3.1"); true if b is newer.
// Tolerates a leading 'v' and trailing junk. Missing components are 0.
[[nodiscard]] bool version_less(const std::string& a, const std::string& b);

// The compiled-in version ("0.3.0").
[[nodiscard]] std::string current_version();

// Query GitHub for the latest release tag. `force` bypasses the 24 h cache.
// Network round-trip — call from a worker thread, never the UI thread.
[[nodiscard]] CheckResult check_latest(bool force = false);

// Non-blocking: the last CACHED check result, if it exists and is fresh.
// Reads one small JSON file; safe for startup.
[[nodiscard]] std::optional<CheckResult> cached_check();

// The release-asset name for this build's platform ("agentty-linux-x86_64"),
// or empty when the platform has no prebuilt asset.
[[nodiscard]] std::string platform_asset();

// Is the running binary self-replaceable? False for package-manager installs
// (/usr/bin, /usr/local from rpm layouts) where the right path is the
// package manager. `reason` explains when false.
[[nodiscard]] bool self_update_possible(std::string& reason);

// Perform the update: download the platform asset for `tag` and atomically
// replace the running binary. `progress` (optional) is called with
// (downloaded_bytes, total_bytes) — total 0 when unknown. When
// `expected_sha256` is non-empty the download must match it, or nothing is
// installed.
// Returns an empty string on success, otherwise a human error message.
[[nodiscard]] std::string perform_update(
    const std::string& tag,
    const std::function<void(std::size_t, std::size_t)>& progress = {},
    const std::string& expected_sha256 = {});

// Pure, for tests: given the GitHub `/releases` JSON (newest first), the
// running version and this platform's asset name, decide what to offer.
// No network, no cache.
[[nodiscard]] CheckResult choose_release(const std::string& releases_json,
                                         const std::string& current,
                                         const std::string& asset);

} // namespace agentty::update
