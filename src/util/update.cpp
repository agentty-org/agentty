// SPDX-License-Identifier: Apache-2.0
//
// update.cpp — self-update against the GitHub releases of agentty-org/agentty.
// See include/agentty/util/update.hpp for the design contract.

#include "agentty/util/update.hpp"
#include "agentty/version.hpp"
#include "agentty/util/digits.hpp"

#include "agentty/util/user_root.hpp"   // user_cache_dir()
#include "agentty/util/self_exe.hpp"
#include "agentty/io/http.hpp"
#include "agentty/io/shared_file.hpp"
#include "agentty/util/dbglog.hpp"
#include "agentty/auth/auth.hpp"     // sha256_hex: verify the asset digest

#include <nlohmann/json.hpp>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef _WIN32
#include <unistd.h>     // readlink
#include <sys/stat.h>   // chmod
#endif


namespace agentty::update {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr const char* kRepo = "agentty-org/agentty";
constexpr auto kCacheTtl = std::chrono::hours{24};
// While a release is still building its binaries, look again this soon.
constexpr auto kPendingTtl = std::chrono::minutes{20};

fs::path cache_path() { return util::user_cache_dir() / "update_check.json"; }

// Where is the running binary? (Windows self-replace isn't wired; MSI is the channel.)
fs::path self_path() {
#ifdef _WIN32
    return {};
#else
    return util::self_exe_path();
#endif
}

struct HttpResult { int status = 0; std::string body; std::string err; std::string location; };

HttpResult https_get(std::string_view host, std::string_view path,
                     std::size_t max_bytes,
                     const std::function<void(std::size_t, std::size_t)>& progress = {}) {
    HttpResult r;
    http::Request req;
    req.method = http::HttpMethod::Get;
    req.host   = std::string{host};
    req.port   = 443;
    req.path   = std::string{path};
    req.headers = {
        {"accept", "application/vnd.github+json"},
        {"user-agent", std::string{"agentty-updater/"} + ::agentty::version()},
    };
    req.max_body_bytes = max_bytes;
    http::Timeouts tos;
    tos.connect = std::chrono::milliseconds(8'000);
    tos.total   = std::chrono::milliseconds(120'000);
    auto resp = http::default_client(::agentty::IoAccess::grant()).send(req, tos);
    if (!resp) { r.err = resp.error().render(); return r; }
    r.status = resp->status;
    r.body   = std::move(resp->body);
    for (const auto& h : resp->headers) {
        std::string name = h.name;
        for (auto& c : name) c = static_cast<char>(std::tolower((unsigned char)c));
        if (name == "location") { r.location = h.value; break; }
    }
    if (progress && r.status == 200) progress(r.body.size(), r.body.size());
    return r;
}

// Split "https://host/path" → {host, path}. Empty host on parse failure.
std::pair<std::string, std::string> split_url(const std::string& url) {
    auto scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    auto host_start = scheme + 3;
    auto slash = url.find('/', host_start);
    if (slash == std::string::npos) return {url.substr(host_start), "/"};
    return {url.substr(host_start, slash - host_start), url.substr(slash)};
}

void write_cache(const CheckResult& c) {
    json j = {
        {"checked_at", std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count()},
        {"current", c.current},
        {"latest", c.latest},
        {"target", c.target},
        {"url", c.url},
        {"asset_sha256", c.asset_sha256},
        {"pending", c.pending},
        {"update_available", c.update_available},
    };
    // Was a truncating write: two instances checking at once could leave a
    // torn cache that parsed as "no update". Atomic now; a cache, so no fsync.
    (void)persistence::write_atomic(::agentty::IoAccess::grant(), cache_path(), j.dump(2),
                                    {.durability = persistence::Durability::Atomic});
}

} // namespace

bool version_less(const std::string& a, const std::string& b) {
    auto parse = [](const std::string& s, int out[3]) {
        out[0] = out[1] = out[2] = 0;
        std::size_t i = 0;
        if (i < s.size() && (s[i] == 'v' || s[i] == 'V')) ++i;
        for (int k = 0; k < 3; ++k) {
            int v = 0;
            bool any = false;
            while (i < s.size() && std::isdigit((unsigned char)s[i])) {
                v = ::agentty::util::push_digit(v, s[i]);
                ++i;
                any = true;
            }
            out[k] = any ? v : 0;
            if (i < s.size() && s[i] == '.') ++i;
            else break;
        }
    };
    int va[3], vb[3];
    parse(a, va);
    parse(b, vb);
    for (int k = 0; k < 3; ++k) {
        if (va[k] != vb[k]) return va[k] < vb[k];
    }
    return false;
}

std::string current_version() { return ::agentty::version(); }

std::string platform_asset() {
#if defined(_WIN32)
    return "agentty-windows-x86_64.exe";
#elif defined(__APPLE__)
#  if defined(__aarch64__) || defined(__arm64__)
    return "agentty-macos-arm64";
#  else
    return "agentty-macos-x86_64";
#  endif
#elif defined(__linux__)
#  if defined(__aarch64__)
    return "agentty-linux-aarch64";
#  elif defined(__i386__)
    return "agentty-linux-i686";
#  else
    return "agentty-linux-x86_64";
#  endif
#else
    return {};
#endif
}

std::optional<CheckResult> cached_check() {
    std::ifstream f(cache_path());
    if (!f) return std::nullopt;
    try {
        json j = json::parse(f);
        auto age = std::chrono::system_clock::now().time_since_epoch()
                 - std::chrono::seconds{j.value("checked_at", std::int64_t{0})};
        if (age > kCacheTtl) return std::nullopt;
        CheckResult c;
        c.current          = j.value("current", std::string{});
        c.latest           = j.value("latest", std::string{});
        c.target           = j.value("target", std::string{});
        c.url              = j.value("url", std::string{});
        c.asset_sha256     = j.value("asset_sha256", std::string{});
        c.pending          = j.value("pending", false);
        c.update_available = j.value("update_available", false);
        // A cache written by an OLDER binary is stale the moment the binary
        // changed (post-update relaunch must not re-announce the old delta).
        if (c.current != current_version()) return std::nullopt;
        // Written before `target` existed: its update_available meant "a
        // newer tag exists", which is exactly the claim #119 broke. Re-check.
        if (!j.contains("target")) return std::nullopt;
        // A pending release is about to become installable; a 24h-old
        // "still building" would hide the update for a day. Re-check sooner.
        if (c.pending && age > kPendingTtl) return std::nullopt;
        return c;
    } catch (...) {
        return std::nullopt;
    }
}

CheckResult choose_release(const std::string& releases_json, const std::string& current,
                           const std::string& asset) {
    CheckResult c;
    c.current = current;
    json arr;
    try {
        arr = json::parse(releases_json);
    } catch (const std::exception& e) {
        c.error = std::string{"release JSON parse failed: "} + e.what();
        return c;
    }
    if (!arr.is_array()) { c.error = "release JSON: expected a list"; return c; }

    auto strip_v = [](std::string t) { return (!t.empty() && t.front() == 'v') ? t.substr(1) : t; };
    // An asset counts only once GitHub has the whole file: `state` is
    // "uploaded" (a half-sent --clobber is "starter"), and it is big enough
    // to be a binary, not a placeholder.
    auto ships = [&](const json& rel, std::string& sha) {
        if (asset.empty() || !rel.contains("assets") || !rel["assets"].is_array()) return false;
        for (const auto& a : rel["assets"]) {
            if (a.value("name", std::string{}) != asset) continue;
            if (a.value("state", std::string{"uploaded"}) != "uploaded") return false;
            if (a.value("size", std::int64_t{0}) < 1024 * 1024) return false;
            const std::string d = a.value("digest", std::string{});
            sha = d.starts_with("sha256:") ? d.substr(7) : std::string{};
            return true;
        }
        return false;
    };

    for (const auto& rel : arr) {
        if (!rel.is_object()) continue;
        if (rel.value("draft", false) || rel.value("prerelease", false)) continue;
        const std::string ver = strip_v(rel.value("tag_name", std::string{}));
        if (ver.empty()) continue;
        if (c.latest.empty() || version_less(c.latest, ver)) {
            c.latest = ver;
            if (c.url.empty()) c.url = rel.value("html_url", std::string{});
        }
        // Not newer than us: nothing older can be an update either, but keep
        // scanning only for `latest` -- the list is newest-first in practice
        // and not guaranteed, so do not break on order.
        if (!version_less(current, ver)) continue;
        std::string sha;
        if (!ships(rel, sha)) continue;
        if (c.target.empty() || version_less(c.target, ver)) {
            c.target = ver;
            c.asset_sha256 = std::move(sha);
            c.url = rel.value("html_url", c.url);
        }
    }
    if (c.url.empty()) c.url = std::string{"https://github.com/"} + kRepo + "/releases";
    c.update_available = !c.target.empty();
    // The newest release is ahead of us but not installable here yet -- the
    // streaming window. Said distinctly so the UI can wait instead of failing.
    c.pending = !c.latest.empty() && version_less(current, c.latest) && c.target != c.latest;
    return c;
}

CheckResult check_latest(bool force) {
    if (!force) {
        if (auto c = cached_check()) return *c;
    }
    CheckResult c;
    c.current = current_version();

    // The last few releases, not /releases/latest: "latest" can be a release
    // whose binary for this platform is still building (#119), and the one
    // before it is then the newest thing that can actually be installed.
    auto r = https_get("api.github.com",
                       std::string{"/repos/"} + kRepo + "/releases?per_page=5",
                       4 * 1024 * 1024);
    if (!r.err.empty()) { c.error = r.err; return c; }
    if (r.status != 200) {
        c.error = "GitHub API HTTP " + std::to_string(r.status);
        return c;
    }
    c = choose_release(r.body, c.current, platform_asset());
    if (c.error.empty()) write_cache(c);
    return c;
}

bool self_update_possible(std::string& reason) {
#ifdef _WIN32
    reason = "self-update is not supported on Windows yet — download the "
             "installer from https://github.com/agentty-org/agentty/releases";
    return false;
#else
#ifndef NDEBUG
    constexpr bool dev_build = true;
#else
    constexpr bool dev_build = false;
#endif
    // A dev build lives in a build tree. Replacing it with the release
    // binary silently swaps out the code you're working on.
    if (dev_build) {
        reason = "this is a development build; rebuild it instead";
        return false;
    }
    fs::path self = self_path();
    if (self.empty()) {
        reason = "could not resolve the running binary's path";
        return false;
    }
    std::string p = self.string();
    // Once the binary is replaced, /proc/self/exe reads "<path> (deleted)".
    // Writing there creates a stray file instead of updating anything.
    if (p.ends_with(" (deleted)")) {
        reason = "the running binary was replaced on disk; restart agentty first";
        return false;
    }
    // Package-manager territory: overwriting under /usr (except /usr/local
    // hand-installs) fights rpm/pacman/portage — tell the user to use them.
    if (p.starts_with("/usr/") && !p.starts_with("/usr/local/")) {
        reason = "installed by a package manager (" + p +
                 ") — update with your package manager instead";
        return false;
    }
    if (p.find("/nix/store/") != std::string::npos) {
        reason = "installed via nix — update through your nix configuration";
        return false;
    }
    // Writable check on the parent dir (the atomic rename target).
    auto dir = self.parent_path();
#ifndef _WIN32
    if (::access(dir.c_str(), W_OK) != 0) {
        reason = "no write permission for " + dir.string() +
                 " — re-run with appropriate permissions";
        return false;
    }
#endif
    return true;
#endif
}

std::string perform_update(
    const std::string& tag,
    const std::function<void(std::size_t, std::size_t)>& progress,
    const std::string& expected_sha256) {
    std::string reason;
    if (!self_update_possible(reason)) return reason;

    const std::string asset = platform_asset();
    if (asset.empty()) return "no prebuilt binary for this platform";

    const std::string vtag = tag.starts_with("v") ? tag : "v" + tag;

    // GitHub's stable download URL 302s to a CDN; follow up to 4 hops.
    std::string host = "github.com";
    std::string path = "/" + std::string{kRepo} + "/releases/download/" +
                       vtag + "/" + asset;
    HttpResult r;
    for (int hop = 0; hop < 4; ++hop) {
        r = https_get(host, path, 512ull * 1024 * 1024, progress);
        if (!r.err.empty()) return "download failed: " + r.err;
        if (r.status == 301 || r.status == 302 || r.status == 307
            || r.status == 308) {
            auto [h, p] = split_url(r.location);
            if (h.empty()) return "download failed: bad redirect";
            host = std::move(h);
            path = std::move(p);
            continue;
        }
        break;
    }
    // A 404 here is almost always the streaming window (#119): the release
    // exists, this platform's binary is still building. Say THAT, not a bare
    // status code -- the user's next move is "wait", not "file a bug".
    if (r.status == 404)
        return "the " + vtag + " binary for this platform (" + asset +
               ") isn't published yet \xe2\x80\x94 release builds take up to an hour. "
               "Try again later; `agentty update` will pick it up.";
    if (r.status != 200)
        return "download failed: HTTP " + std::to_string(r.status) +
               " for " + asset + " (release " + vtag + ")";
    if (r.body.size() < 1024 * 1024)
        return "downloaded binary suspiciously small (" +
               std::to_string(r.body.size()) + " bytes) — aborting";
    // ELF/Mach-O magic sanity: never install an HTML error page as a binary.
#ifndef _WIN32
    const bool looks_binary =
        (r.body.size() > 4)
        && ((r.body[0] == 0x7f && r.body[1] == 'E' && r.body[2] == 'L'
             && r.body[3] == 'F')                                    // ELF
            || ((unsigned char)r.body[0] == 0xcf
                && (unsigned char)r.body[1] == 0xfa)                 // Mach-O 64
            || ((unsigned char)r.body[0] == 0xca
                && (unsigned char)r.body[1] == 0xfe));               // Mach-O fat
    if (!looks_binary)
        return "downloaded file is not an executable — aborting";
#endif
    // GitHub records a sha256 for every release asset. When the check gave
    // us one, the bytes must match it before anything is installed: a
    // truncated CDN response or a swapped asset would otherwise become the
    // next launch's binary.
    if (!expected_sha256.empty()) {
        const std::string got = auth::sha256_hex(r.body);
        if (got != expected_sha256)
            return "downloaded " + asset + " failed its checksum (got " + got.substr(0, 12) +
                   "\xe2\x80\xa6, expected " + expected_sha256.substr(0, 12) + "\xe2\x80\xa6) \xe2\x80\x94 nothing installed";
    }

    // Atomic swap over the live binary: the running process keeps executing
    // its old (unlinked) image, the next launch gets the new one. Born 0755,
    // and durable, so a power cut can't leave a half-written agentty.
    fs::path self = self_path();
    if (!persistence::write_atomic(::agentty::IoAccess::grant(), self, r.body,
                                   {.durability = persistence::Durability::Durable,
                                    .mode = 0755}))
        return "could not replace " + self.string();
    std::error_code ec;
    // Invalidate the check cache so the fresh binary re-checks cleanly.
    fs::remove(cache_path(), ec);
    return {};
}

} // namespace agentty::update
