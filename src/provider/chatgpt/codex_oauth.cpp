#include "agentty/provider/chatgpt/codex_oauth.hpp"
#include "agentty/version.hpp"
#include "agentty/util/lock_levels.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "agentty/auth/cred_crypt.hpp"
#include "agentty/auth/auth.hpp"
#include "agentty/io/http.hpp"
#include "agentty/io/shared_file.hpp"
#include "agentty/util/base64.hpp"
#include "agentty/util/dbglog.hpp"

#include <maya/runtime.hpp>

namespace agentty::provider::chatgpt {
namespace {
using json = nlohmann::json;
using auth::OAuthError;
using auth::OAuthErrorKind;
namespace fs = std::filesystem;

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// RFC-3986 unreserved passthrough, everything else %HH.
std::string url_escape(std::string_view s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() + s.size() / 4);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
         || (c >= '0' && c <= '9')
         || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

std::string form_encode(const std::vector<std::pair<std::string, std::string>>& kv) {
    std::string out;
    for (std::size_t i = 0; i < kv.size(); ++i) {
        if (i) out += '&';
        out += url_escape(kv[i].first);
        out += '=';
        out += url_escape(kv[i].second);
    }
    return out;
}

// A single POST to auth.openai.com. `json_body` chooses application/json vs
// x-www-form-urlencoded (the Codex refresh uses JSON, the code+key exchanges
// use form). Returns {status, body} or a transport error string.
struct PostResult {
    int status = 0;
    std::string body;
    std::string transport_error;
};

PostResult post(std::string_view path, std::string body, bool json_body) {
    PostResult r;
    http::Request req;
    req.method = http::HttpMethod::Post;
    req.host   = "auth.openai.com";
    req.port   = 443;
    req.path   = std::string{path};
    req.headers = {
        {"content-type", json_body ? "application/json"
                                   : "application/x-www-form-urlencoded"},
        {"accept",       "application/json"},
        {"user-agent",   ::agentty::user_agent()},
    };
    if (const auto& ov = http::agentty_oauth_host_override(); ov.active()) {
        req.dial_host = ov.host;
        req.dial_port = ov.port;
    }
    req.body = std::move(body);
    req.max_body_bytes = 1ull * 1024 * 1024;

    http::Timeouts tos;
    tos.connect = std::chrono::milliseconds(10'000);
    tos.total   = std::chrono::milliseconds(30'000);

    auto resp = http::default_client(::agentty::IoAccess::grant()).send(req, tos);
    if (!resp) { r.transport_error = resp.error().render(); return r; }
    r.status = resp->status;
    r.body   = std::move(resp->body);
    return r;
}

// Pull one claim out of a JWT (middle segment, base64url). Best-effort: any
// malformed input returns empty so the caller degrades to no account_id.
std::string jwt_claim(const std::string& jwt, const char* key) {
    const auto a = jwt.find('.');
    if (a == std::string::npos) return {};
    const auto b = jwt.find('.', a + 1);
    if (b == std::string::npos) return {};
    std::string payload = jwt.substr(a + 1, b - a - 1);
    // base64url → standard base64 for the decoder.
    for (auto& c : payload) { if (c == '-') c = '+'; else if (c == '_') c = '/'; }
    while (payload.size() % 4) payload.push_back('=');
    try {
        auto raw = util::base64_decode(payload);
        auto j = json::parse(raw);
        // account id can live directly or under auth.chatgpt_account_id.
        if (j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
        if (j.contains("https://api.openai.com/auth")) {
            const auto& o = j["https://api.openai.com/auth"];
            if (o.contains(key) && o[key].is_string()) return o[key].get<std::string>();
        }
    } catch (const std::exception& e) {
        util::dbglog("codex.jwt_claim.parse", e.what());
    } catch (...) {
        util::dbglog("codex.jwt_claim.parse", "non-std exception");
    }
    return {};
}

// The `exp` claim (seconds since epoch) of a JWT, or 0.
std::int64_t jwt_exp_seconds(const std::string& jwt) {
    const auto a = jwt.find('.');
    if (a == std::string::npos) return 0;
    const auto b = jwt.find('.', a + 1);
    if (b == std::string::npos) return 0;
    std::string payload = jwt.substr(a + 1, b - a - 1);
    for (auto& c : payload) { if (c == '-') c = '+'; else if (c == '_') c = '/'; }
    while (payload.size() % 4) payload.push_back('=');
    try {
        const auto j = json::parse(util::base64_decode(payload));
        if (auto e = j.find("exp"); e != j.end() && e->is_number()) return e->get<std::int64_t>();
    } catch (...) {}
    return 0;
}
std::expected<CodexCredentials, OAuthError>
parse_tokens(const std::string& body, int status,
             const std::string& prior_refresh) {
    json j;
    try { j = json::parse(body); }
    catch (const std::exception& e) {
        return std::unexpected(OAuthError{OAuthErrorKind::BadResponse,
            std::string{"json parse failed: "} + e.what()});
    }
    if (status >= 400) {
        // Keep the server's error code in the detail so the caller can tell
        // "sign in again" from "try again later".
        const std::string code = j.contains("error") && j["error"].is_object()
            ? j["error"].value("code", std::string{}) : j.value("error", std::string{});
        std::string desc = j.contains("error") && j["error"].is_object()
            ? j["error"].value("message", std::string{})
            : j.value("error_description", std::string{});
        if (desc.empty()) desc = "HTTP " + std::to_string(status);
        // "status=NNN " leads so a refresh failure can be classified on it.
        return std::unexpected(OAuthError{OAuthErrorKind::ApiError,
            "status=" + std::to_string(status) + " "
            + (code.empty() ? desc : code + ": " + desc)});
    }
    CodexCredentials c;
    c.access_token  = j.value("access_token", std::string{});
    c.refresh_token = j.value("refresh_token", prior_refresh);
    c.id_token      = j.value("id_token", std::string{});
    if (c.access_token.empty()) {
        return std::unexpected(OAuthError{OAuthErrorKind::MissingToken,
            "no access_token in token response"});
    }
    if (const auto secs = j.value("expires_in", std::int64_t{0}); secs > 0)
        c.expires_at_ms = now_ms() + secs * 1000;
    else if (const auto exp = jwt_exp_seconds(c.access_token); exp > 0)
        c.expires_at_ms = exp * 1000;   // the token says when it dies
    c.last_refresh_ms = now_ms();
    if (!c.id_token.empty())
        c.account_id = jwt_claim(c.id_token, "chatgpt_account_id");
    return c;
}

// RFC-8693 token-exchange: id_token → usable OpenAI api key. Best-effort;
// login still succeeds (with just the bearer token) if this fails.
std::string mint_api_key(const std::string& id_token) {
    if (id_token.empty()) return {};
    auto r = post("/oauth/token", form_encode({
        {"grant_type",         OAuthConfig::token_exchange_grant},
        {"client_id",          OAuthConfig::client_id},
        {"requested_token",    OAuthConfig::requested_token_apikey},
        {"subject_token",      id_token},
        {"subject_token_type", OAuthConfig::subject_token_type_idtoken},
    }), /*json_body=*/false);
    if (!r.transport_error.empty() || r.status >= 400) return {};
    try {
        return json::parse(r.body).value("access_token", std::string{});
    } catch (...) { return {}; }
}

struct DeviceAuthorization {
    std::string device_auth_id;
    std::string user_code;
    int interval_s = 5;
};

struct DeviceGrant {
    std::string authorization_code;
    std::string code_verifier;
    std::string code_challenge;
};

std::string response_error(const PostResult& r, std::string fallback) {
    try {
        const auto j = json::parse(r.body);
        return j.value("error_description", j.value("error", std::move(fallback)));
    } catch (...) {
        return fallback;
    }
}

std::expected<DeviceAuthorization, OAuthError> request_device_authorization() {
    auto r = post("/api/accounts/deviceauth/usercode",
                  json{{"client_id", OAuthConfig::client_id}}.dump(),
                  /*json_body=*/true);
    if (!r.transport_error.empty())
        return std::unexpected(OAuthError{OAuthErrorKind::Network, r.transport_error});
    if (r.status == 404) {
        return std::unexpected(OAuthError{OAuthErrorKind::ApiError,
            "device-code login is disabled for this ChatGPT account or workspace; "
            "enable it in ChatGPT security settings, or reconnect with "
            "`ssh -L 1455:localhost:1455 ...`"});
    }
    if (r.status >= 400) {
        return std::unexpected(OAuthError{OAuthErrorKind::ApiError,
            response_error(r, "device-code request failed (HTTP "
                              + std::to_string(r.status) + ")")});
    }

    try {
        const auto j = json::parse(r.body);
        DeviceAuthorization d;
        d.device_auth_id = j.value("device_auth_id", std::string{});
        d.user_code = j.value("user_code", j.value("usercode", std::string{}));
        if (const auto it = j.find("interval"); it != j.end()) {
            if (it->is_number_integer()) d.interval_s = it->get<int>();
            else if (it->is_string()) d.interval_s = std::stoi(it->get<std::string>());
        }
        d.interval_s = std::clamp(d.interval_s, 1, 30);
        if (d.device_auth_id.empty() || d.device_auth_id.size() > 1024
            || d.user_code.empty() || d.user_code.size() > 128) {
            return std::unexpected(OAuthError{OAuthErrorKind::BadResponse,
                "device-code response omitted a valid id or one-time code"});
        }
        return d;
    } catch (const std::exception& e) {
        return std::unexpected(OAuthError{OAuthErrorKind::BadResponse,
            std::string{"invalid device-code response: "} + e.what()});
    }
}

std::expected<DeviceGrant, OAuthError>
poll_device_authorization(const maya::unlocked& cap, const DeviceAuthorization& device,
                          int timeout_s, const CodexCancelProbe& cancelled) {
    using clock = std::chrono::steady_clock;
    const auto timeout = std::chrono::seconds{std::max(timeout_s, 1)};
    const auto deadline = clock::now() + timeout;
    const auto was_cancelled = [&] { return cancelled.stop_requested(); };
    // The server's interval (or what is left of the deadline), cut short the
    // moment the login is cancelled.
    const auto wait_for_next_poll = [&] {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now());
        (void)maya::delay_for(cap, cancelled,
            std::min<std::chrono::milliseconds>(std::chrono::seconds{device.interval_s}, left));
    };

    while (clock::now() < deadline) {
        if (was_cancelled()) {
            return std::unexpected(OAuthError{OAuthErrorKind::Network,
                "device-code login cancelled"});
        }
        auto r = post("/api/accounts/deviceauth/token", json{
            {"device_auth_id", device.device_auth_id},
            {"user_code", device.user_code},
        }.dump(), /*json_body=*/true);
        if (!r.transport_error.empty()) {
            // A device authorization remains valid across transient network
            // failures. Retry at the server cadence until its deadline.
            wait_for_next_poll();
            continue;
        }

        if (r.status >= 200 && r.status < 300) {
            try {
                const auto j = json::parse(r.body);
                DeviceGrant grant{
                    .authorization_code = j.value("authorization_code", std::string{}),
                    .code_verifier = j.value("code_verifier", std::string{}),
                    .code_challenge = j.value("code_challenge", std::string{}),
                };
                if (grant.authorization_code.empty()
                    || grant.authorization_code.size() > 8192
                    || grant.code_verifier.size() < 43
                    || grant.code_verifier.size() > 128
                    || grant.code_challenge.empty()
                    || grant.code_challenge.size() > 128
                    || auth::code_challenge_s256(grant.code_verifier)
                        != grant.code_challenge) {
                    return std::unexpected(OAuthError{OAuthErrorKind::BadResponse,
                        "device approval returned invalid PKCE parameters"});
                }
                return grant;
            } catch (const std::exception& e) {
                return std::unexpected(OAuthError{OAuthErrorKind::BadResponse,
                    std::string{"invalid device approval response: "} + e.what()});
            }
        }

        // OpenAI currently uses 403/404 for "authorization pending".
        if (r.status != 403 && r.status != 404) {
            return std::unexpected(OAuthError{OAuthErrorKind::ApiError,
                response_error(r, "device-code polling failed (HTTP "
                                  + std::to_string(r.status) + ")")});
        }
        wait_for_next_poll();
    }
    return std::unexpected(OAuthError{OAuthErrorKind::Network,
        "device-code login timed out; start sign-in again for a new code"});
}

// ── Loopback callback server (blocking, single connection) ─────────────────
//
// OpenAI pins the redirect to http://localhost:1455/auth/callback, so we bind
// that port, accept one request, extract ?code=&state=, and reply with a
// browser-friendly page. Returns the raw query on success. Cross-platform via
// Winsock / BSD sockets; no framework needed for a one-shot local exchange.
struct CallbackResult {
    std::string code;
    std::string state;
    std::string error;   // non-empty on failure (bind/accept/timeout)
};

std::string query_param(std::string_view target, std::string_view key) {
    // target is the request-target, e.g. /auth/callback?code=X&state=Y
    const auto q = target.find('?');
    if (q == std::string_view::npos) return {};
    std::string_view qs = target.substr(q + 1);
    std::string want{key};
    want += '=';
    std::size_t pos = 0;
    while (pos < qs.size()) {
        auto amp = qs.find('&', pos);
        std::string_view pair = qs.substr(pos, amp == std::string_view::npos
                                                    ? std::string_view::npos
                                                    : amp - pos);
        if (pair.substr(0, want.size()) == want) {
            std::string_view raw = pair.substr(want.size());
            // Minimal %XX / '+' decode — sufficient for OAuth code/state.
            std::string out;
            for (std::size_t i = 0; i < raw.size(); ++i) {
                if (raw[i] == '%' && i + 2 < raw.size()) {
                    auto hex = [](char c) -> int {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        return -1;
                    };
                    int hi = hex(raw[i + 1]), lo = hex(raw[i + 2]);
                    if (hi >= 0 && lo >= 0) { out.push_back(char(hi * 16 + lo)); i += 2; continue; }
                }
                out.push_back(raw[i] == '+' ? ' ' : raw[i]);
            }
            return out;
        }
        if (amp == std::string_view::npos) break;
        pos = amp + 1;
    }
    return {};
}

// The sockets a cancel should shut down, so a blocked select() wakes at once.
// -1 = none. Read by the cancel callback, written by the waiting thread.
// Lock-free: the cancel callback runs on whichever thread requests the stop
// (it cannot carry a capability), so it must not take a guarded. `busy`
// counts callbacks mid-shutdown; the waiter clears the fds and then waits
// for `busy` to drain before it closes a socket, so the number cannot be
// reused between the callback's read and its shutdown. (All seq_cst.)
struct WakeFds {
    std::atomic<std::intptr_t> srv{-1};
    std::atomic<std::intptr_t> cli{-1};
    std::atomic<int>           busy{0};
};

void shut_read(std::intptr_t fd) noexcept {
    if (fd < 0) return;
#if defined(_WIN32)
    ::shutdown(static_cast<SOCKET>(fd), SD_RECEIVE);
#else
    ::shutdown(static_cast<int>(fd), SHUT_RD);
#endif
}

CallbackResult wait_for_callback_impl(int timeout_s, const CodexCancelProbe& cancelled,
                                      WakeFds& wake, int port) {
    CallbackResult out;
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        out.error = "WSAStartup failed"; return out;
    }
    struct WsaGuard { ~WsaGuard() { WSACleanup(); } } wsa_guard;
    using sock_t = SOCKET;
    const sock_t kInvalid = INVALID_SOCKET;
    auto close_sock = [](sock_t s) { closesocket(s); };
#else
    using sock_t = int;
    const sock_t kInvalid = -1;
    auto close_sock = [](sock_t s) { ::close(s); };
#endif

    sock_t srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv == kInvalid) { out.error = "cannot create callback socket"; return out; }
    int yes = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // bind 127.0.0.1 only
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_sock(srv);
        out.error = "ports 1455 and 1457 are busy \xe2\x80\x94 close any running "
                    "`codex`/Codex login and retry";
        return out;
    }
    if (::listen(srv, 1) != 0) { close_sock(srv); out.error = "listen failed"; return out; }

    // Wait for the browser up to the deadline. A cancel shuts the client
    // socket down, which wakes its select() at once. The LISTENER is another
    // matter: shutdown() on a listening socket only wakes select() on Linux.
    // On darwin/BSD it fails with ENOTCONN and wakes nothing (verified), and
    // Winsock behaves the same way, so a cancelled login would keep its
    // stream thread and port 1455 until the 15-minute deadline (and a retry
    // would see "port 1455 is busy"). So the listener waits in short slices
    // and re-checks the token; the shutdown still makes Linux instant.
    constexpr auto kListenSlice = std::chrono::milliseconds(200);
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(timeout_s);
    const auto drain = [&wake] {
        while (wake.busy.load() != 0) std::this_thread::yield();
    };
    const auto arm = [&wake](sock_t a, sock_t b) {
        wake.srv.store(static_cast<std::intptr_t>(a));
        wake.cli.store(static_cast<std::intptr_t>(b));
    };
    const auto disarm = [&wake, &drain] {
        wake.srv.store(-1);
        wake.cli.store(-1);
        drain();
    };
    arm(srv, kInvalid);
    // A cancel that landed before arming would otherwise be missed.
    if (cancelled.stop_requested()) shut_read(static_cast<std::intptr_t>(srv));
    const auto until_deadline = [&] {
        const auto left = std::chrono::duration_cast<std::chrono::microseconds>(
            deadline - std::chrono::steady_clock::now());
        timeval tv{};
        if (left.count() > 0) {
            tv.tv_sec  = static_cast<decltype(tv.tv_sec)>(left.count() / 1'000'000);
            tv.tv_usec = static_cast<decltype(tv.tv_usec)>(left.count() % 1'000'000);
        }
        return tv;
    };
    for (;;) {
        const bool cancel = cancelled.stop_requested();
        if (cancel || std::chrono::steady_clock::now() >= deadline) {
            disarm(); close_sock(srv);
            out.error = cancel ? "login cancelled" : "login timed out";
            return out;
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv, &rfds);
        timeval tv = until_deadline();
        if (std::chrono::seconds(tv.tv_sec) + std::chrono::microseconds(tv.tv_usec)
            > kListenSlice) {
            tv.tv_sec  = 0;
            tv.tv_usec = static_cast<decltype(tv.tv_usec)>(
                std::chrono::microseconds(kListenSlice).count());
        }
        const int ready = ::select(static_cast<int>(srv + 1), &rfds, nullptr, nullptr, &tv);
        if (ready <= 0) continue;

        sock_t cli = ::accept(srv, nullptr, nullptr);
        if (cli == kInvalid) continue;
        arm(srv, cli);

        // Read through short select() slices so a local peer that connects and
        // stalls cannot pin port 1455 or defeat cancellation/the deadline.
        std::string req;
        char buf[4096];
        while (req.size() <= 16384) {
            const bool read_cancel = cancelled.stop_requested();
            if (read_cancel || std::chrono::steady_clock::now() >= deadline) {
                disarm(); close_sock(cli);
                close_sock(srv);
                out.error = read_cancel ? "login cancelled" : "login timed out";
                return out;
            }
            fd_set cli_rfds;
            FD_ZERO(&cli_rfds);
            FD_SET(cli, &cli_rfds);
            timeval cli_tv = until_deadline();
            const int readable = ::select(static_cast<int>(cli + 1), &cli_rfds,
                                          nullptr, nullptr, &cli_tv);
            if (readable < 0) break;
            if (readable == 0) continue;
#if defined(_WIN32)
            int n = ::recv(cli, buf, sizeof(buf), 0);
#else
            auto n = ::recv(cli, buf, sizeof(buf), 0);
#endif
            if (n <= 0) break;
            req.append(buf, static_cast<std::size_t>(n));
            if (req.find("\r\n\r\n") != std::string::npos) break;
        }

        // Parse "GET /auth/callback?code=...&state=... HTTP/1.1".
        std::string target;
        if (const auto sp = req.find(' '); sp != std::string::npos) {
            const auto sp2 = req.find(' ', sp + 1);
            if (sp2 != std::string::npos)
                target = req.substr(sp + 1, sp2 - sp - 1);
        }
        out.code  = query_param(target, "code");
        out.state = query_param(target, "state");
        const std::string err = query_param(target, "error");

        const bool ok = !out.code.empty() && err.empty();
        const std::string page = ok
            ? "<html><head><title>agentty</title></head><body "
              "style='font-family:system-ui;background:#0b0d10;color:#e6e6e6;"
              "display:flex;align-items:center;justify-content:center;height:100vh'>"
              "<div style='text-align:center'><h2>&#10003; Signed in to ChatGPT</h2>"
              "<p>You can close this tab and return to agentty.</p></div></body></html>"
            : "<html><body style='font-family:system-ui'><h2>Sign-in failed</h2>"
              "<p>Return to agentty and try again.</p></body></html>";
        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                           "Content-Length: " + std::to_string(page.size())
                         + "\r\nConnection: close\r\n\r\n" + page;
        (void)::send(cli, resp.data(), static_cast<int>(resp.size()), 0);
        disarm(); close_sock(cli);
        close_sock(srv);
        if (!ok) out.error = err.empty() ? "no authorization code in callback"
                                         : ("authorization denied: " + err);
        return out;
    }
}

CallbackResult wait_for_callback(int timeout_s, const CodexCancelProbe& cancelled, int port) {
    WakeFds wake;
    std::stop_callback on_cancel(cancelled, [&wake] {
        // Announce ourselves before reading, so the waiter's disarm() waits
        // for this shutdown before it closes (and frees) the fd number.
        wake.busy.fetch_add(1);
        shut_read(wake.srv.load());
        shut_read(wake.cli.load());
        wake.busy.fetch_sub(1);
    });
    return wait_for_callback_impl(timeout_s, cancelled, wake, port);
}

// The callback port to use: 1455, or 1457 when something holds 1455 (a
// Codex login left running). Both are on OpenAI's redirect allow-list for
// this client; codex-rs falls back the same way. 0 = neither is free.
int pick_callback_port() {
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return OAuthConfig::callback_port;
    struct WsaGuard { ~WsaGuard() { WSACleanup(); } } g;
    auto close_sock = [](SOCKET s) { closesocket(s); };
    const SOCKET kInvalid = INVALID_SOCKET;
#else
    auto close_sock = [](int s) { ::close(s); };
    const int kInvalid = -1;
#endif
    for (int port : {OAuthConfig::callback_port, OAuthConfig::callback_fallback_port}) {
        auto s = ::socket(AF_INET, SOCK_STREAM, 0);
        if (s == kInvalid) continue;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(static_cast<uint16_t>(port));
        const bool free = ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
        close_sock(s);
        if (free) return port;
    }
    return 0;
}

std::string redirect_uri_for(int port) {
    return "http://localhost:" + std::to_string(port) + OAuthConfig::callback_path;
}

// ── Credential (de)serialization ───────────────────────────────────────────
json to_json(const CodexCredentials& c) {
    return json{
        {"access_token",  c.access_token},
        {"refresh_token", c.refresh_token},
        {"id_token",      c.id_token},
        {"account_id",    c.account_id},
        {"api_key",       c.api_key},
        {"expires_at_ms", c.expires_at_ms},
        {"last_refresh_ms", c.last_refresh_ms},
    };
}

CodexCredentials from_json(const json& j) {
    CodexCredentials c;
    c.access_token  = j.value("access_token", std::string{});
    c.refresh_token = j.value("refresh_token", std::string{});
    c.id_token      = j.value("id_token", std::string{});
    c.account_id    = j.value("account_id", std::string{});
    c.api_key       = j.value("api_key", std::string{});
    c.expires_at_ms = j.value("expires_at_ms", std::int64_t{0});
    c.last_refresh_ms = j.value("last_refresh_ms", std::int64_t{0});
    // Saved before expires_in was always set: the token itself knows.
    if (c.expires_at_ms == 0)
        if (const auto exp = jwt_exp_seconds(c.access_token); exp > 0) c.expires_at_ms = exp * 1000;
    return c;
}

// The creds file. Every read and write runs inside store().with; the
// generation bumps on each save or clear, so a refresh can tell whether the
// file changed under it while the network call was out.
struct Store { std::uint64_t generation = 0; };
maya::guarded<Store>& store() { static maya::guarded<Store> s; return s; }


std::optional<CodexCredentials> load_codex_credentials_unlocked() {
    std::ifstream ifs(auth::config_dir() / "codex_credentials.json",
                      std::ios::binary);
    if (!ifs) return std::nullopt;
    std::string raw{std::istreambuf_iterator<char>(ifs), {}};
    if (raw.empty()) return std::nullopt;

    std::string plain = raw;
    if (auth::crypt::looks_sealed(raw)) {
        auto pt = auth::crypt::unseal(raw);
        if (!pt) return std::nullopt;
        plain = std::move(*pt);
    }
    try {
        auto c = from_json(json::parse(plain));
        if (c.access_token.empty()) return std::nullopt;
        return c;
    } catch (...) { return std::nullopt; }
}

bool save_codex_credentials_unlocked(const CodexCredentials& c) {
    const auto path = auth::config_dir() / "codex_credentials.json";
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    const std::string payload = to_json(c).dump();
    std::string to_write;
    if (auto sealed = auth::crypt::seal(payload)) to_write = std::move(*sealed);
    else return false;
    return persistence::write_atomic(::agentty::IoAccess::grant(), path, to_write,
                                     {.durability = persistence::Durability::Durable,
                                      .mode = 0600});
}

struct Loaded { std::optional<CodexCredentials> creds; std::uint64_t generation = 0; };
Loaded load_with_generation(const maya::below<maya::leaf>& cap) {
    return store().with(cap, [](Store& st) {
        return Loaded{load_codex_credentials_unlocked(), st.generation};
    });
}

} // namespace

bool CodexCredentials::expired(std::int64_t skew_ms) const noexcept {
    return expires_at_ms != 0 && now_ms() >= expires_at_ms - skew_ms;
}

bool CodexCredentials::needs_refresh() const noexcept {
    constexpr std::int64_t kWindowMs = 5LL * 60 * 1000;         // codex-rs: 5 minutes
    constexpr std::int64_t kMaxAgeMs = 8LL * 24 * 3600 * 1000;  // codex-rs: 8 days
    if (expired(kWindowMs)) return true;
    return last_refresh_ms != 0 && now_ms() - last_refresh_ms > kMaxAgeMs;
}

fs::path codex_credentials_path() {
    return auth::config_dir() / "codex_credentials.json";
}

// Lock-free: saves publish by atomic rename and clears by remove, so a reader
// sees the old file, the new file, or none — never a torn one. The store
// lock only orders writers (and the refresh's generation check).
std::optional<CodexCredentials> load_codex_credentials() {
    return load_codex_credentials_unlocked();
}

bool save_codex_credentials(const maya::below<maya::leaf>& cap, const CodexCredentials& c) {
    return store().with(cap, [](Store& st, CodexCredentials creds) {
        if (!save_codex_credentials_unlocked(creds)) return false;
        ++st.generation;
        return true;
    }, c);
}

bool clear_codex_credentials(const maya::below<maya::leaf>& cap) {
    return store().with(cap, [](Store& st) {
        std::error_code ec;
        const bool ok = fs::remove(codex_credentials_path(), ec) || !ec;
        if (ok) ++st.generation;
        return ok;
    });
}

std::string codex_authorize_url(const auth::PkceVerifier& verifier,
                                const auth::OAuthState&   state,
                                std::string_view          redirect_uri) {
    const std::string challenge = auth::code_challenge_s256(verifier.value);
    std::ostringstream url;
    url << OAuthConfig::authorize_url
        << "?response_type=code"
        << "&client_id="              << OAuthConfig::client_id
        << "&redirect_uri="           << url_escape(redirect_uri)
        << "&scope="                  << url_escape(OAuthConfig::scopes)
        << "&code_challenge="         << url_escape(challenge)
        << "&code_challenge_method=S256"
        << "&id_token_add_organizations=true"
        << "&codex_cli_simplified_flow=true"
        << "&state="                  << url_escape(state.value)
        << "&originator="             << url_escape(OAuthConfig::originator);
    return url.str();
}

std::expected<CodexCredentials, OAuthError>
codex_exchange_code(const auth::OAuthCode& code, const auth::PkceVerifier& verifier,
                    std::string_view redirect_uri) {
    auto r = post("/oauth/token", form_encode({
        {"grant_type",    "authorization_code"},
        {"code",          code.value},
        {"client_id",     OAuthConfig::client_id},
        {"redirect_uri",  std::string{redirect_uri}},
        {"code_verifier", verifier.value},
    }), /*json_body=*/false);
    if (!r.transport_error.empty())
        return std::unexpected(OAuthError{OAuthErrorKind::Network, r.transport_error});

    auto c = parse_tokens(r.body, r.status, "");
    if (!c) return c;
    c->api_key = mint_api_key(c->id_token);
    return c;
}

std::expected<CodexCredentials, OAuthError>
codex_refresh(const CodexCredentials& current) {
    if (current.refresh_token.empty())
        return std::unexpected(OAuthError{OAuthErrorKind::ApiError,
            "no refresh_token — sign in again"});
    // Codex refresh uses a JSON body (not form-encoded).
    const std::string body = json{
        {"client_id",     OAuthConfig::client_id},
        {"grant_type",    "refresh_token"},
        {"refresh_token", current.refresh_token},
    }.dump();
    auto r = post("/oauth/token", body, /*json_body=*/true);
    if (!r.transport_error.empty())
        return std::unexpected(OAuthError{OAuthErrorKind::Network, r.transport_error});

    auto c = parse_tokens(r.body, r.status, current.refresh_token);
    if (!c) return c;
    if (c->account_id.empty()) c->account_id = current.account_id;
    c->api_key = mint_api_key(c->id_token.empty() ? current.id_token : c->id_token);
    if (c->api_key.empty()) c->api_key = current.api_key;   // keep the old key on mint failure
    return c;
}

bool codex_device_auth_preferred() noexcept {
    if (const char* value = std::getenv("AGENTTY_CHATGPT_DEVICE_AUTH")) {
        const std::string_view setting{value};
        if (setting == "0" || setting == "false" || setting == "no"
            || setting == "off") return false;
        if (setting == "1" || setting == "true" || setting == "yes"
            || setting == "on") return true;
    }
    const auto present = [](const char* name) {
        const char* value = std::getenv(name);
        return value && *value;
    };
    return present("SSH_CONNECTION") || present("SSH_CLIENT") || present("SSH_TTY");
}

std::expected<CodexCredentials, OAuthError>
codex_device_login(const maya::unlocked& cap, CodexDeviceCodeSink on_device_code,
                   int timeout_s, CodexCancelProbe cancelled) {
    if (cancelled.stop_requested()) {
        return std::unexpected(OAuthError{OAuthErrorKind::Network,
            "device-code login cancelled"});
    }
    auto device = request_device_authorization();
    if (!device) return std::unexpected(device.error());

    if (on_device_code) {
        on_device_code(CodexDeviceCode{
            .verification_url = std::string{OAuthConfig::issuer} + "/codex/device",
            .user_code = device->user_code,
        });
    }

    auto grant = poll_device_authorization(cap, *device, timeout_s, cancelled);
    if (!grant) return std::unexpected(grant.error());
    if (cancelled.stop_requested()) {
        return std::unexpected(OAuthError{OAuthErrorKind::Network,
            "device-code login cancelled"});
    }

    const std::string redirect_uri = std::string{OAuthConfig::issuer}
                                   + "/deviceauth/callback";
    auto r = post("/oauth/token", form_encode({
        {"grant_type",    "authorization_code"},
        {"code",          grant->authorization_code},
        {"client_id",     OAuthConfig::client_id},
        {"redirect_uri",  redirect_uri},
        {"code_verifier", grant->code_verifier},
    }), /*json_body=*/false);
    if (!r.transport_error.empty())
        return std::unexpected(OAuthError{OAuthErrorKind::Network, r.transport_error});

    auto c = parse_tokens(r.body, r.status, "");
    if (!c) return c;
    c->api_key = mint_api_key(c->id_token);
    return c;
}

std::expected<CodexCredentials, OAuthError>
codex_login(Io io, const maya::unlocked& cap, int timeout_s, CodexDeviceCodeSink on_device_code,
            CodexCancelProbe cancelled) {
    if (codex_device_auth_preferred())
        return codex_device_login(cap, std::move(on_device_code), timeout_s,
                                  std::move(cancelled));

    auth::PkceVerifier verifier{auth::random_urlsafe(io, 64)};
    auth::OAuthState   state{auth::random_urlsafe(io, 32)};

    const int port = pick_callback_port();
    if (port == 0)
        return std::unexpected(OAuthError{OAuthErrorKind::Network,
            "ports 1455 and 1457 are busy \xe2\x80\x94 close any running `codex`/Codex login and retry"});
    const std::string redirect = redirect_uri_for(port);
    const std::string url = codex_authorize_url(verifier, state, redirect);
    auth::open_browser(url);

    auto cb = wait_for_callback(timeout_s, cancelled, port);
    if (!cb.error.empty())
        return std::unexpected(OAuthError{OAuthErrorKind::Network, cb.error});

    // Anti-CSRF: require the IdP to echo our state and compare it in constant
    // time. Missing state is a mismatch, never an optional compatibility path.
    const std::string& want = state.value;
    bool ok = cb.state.size() == want.size();
    unsigned char diff = ok ? 0 : 1;
    for (std::size_t i = 0; i < want.size() && i < cb.state.size(); ++i)
        diff |= static_cast<unsigned char>(want[i] ^ cb.state[i]);
    if (!ok || diff)
        return std::unexpected(OAuthError{OAuthErrorKind::BadResponse,
            "state mismatch — re-run login"});

    if (cancelled.stop_requested()) {
        return std::unexpected(OAuthError{OAuthErrorKind::Network,
            "login cancelled"});
    }
    auto c = codex_exchange_code(auth::OAuthCode{cb.code}, verifier, redirect);
    return c;
}

namespace {
RefreshOutcome refresh_locked(const maya::held<locks::auth_refresh>& cap,
                              std::string_view rejected);

// Last refresh failure, for the request path to report. Written only under
// the refresh lane; an atomic so readers don't need it.
std::atomic<std::uint8_t>& last_failure() {
    static std::atomic<std::uint8_t> v{0};
    return v;
}

maya::guarded<bool, locks::auth_refresh>& refresh_lane() {
    static maya::guarded<bool, locks::auth_refresh> lane;
    return lane;
}
}  // namespace

std::optional<CodexCredentials> codex_fresh_credentials(const locks::below_auth_refresh& cap) {
    // One refresh at a time. The store is only held for file IO, never
    // across the network call, so sign-out and a new login stay responsive.
    // Lock order: refresh lane (auth_refresh), then store (leaf).
    return refresh_lane().with(cap, [](const maya::held<locks::auth_refresh>& h, bool&) {
        return refresh_locked(h, {}).creds;
    });
}

RefreshOutcome codex_force_refresh(const locks::below_auth_refresh& cap,
                                   std::string_view rejected_access_token) {
    return refresh_lane().with(cap, [](const maya::held<locks::auth_refresh>& h, bool&,
                                       std::string rejected) {
        return refresh_locked(h, rejected);
    }, std::string{rejected_access_token});
}

RefreshFailure codex_last_refresh_failure() noexcept {
    return static_cast<RefreshFailure>(last_failure().load(std::memory_order_relaxed));
}

RefreshFailure classify_refresh_failure(int status, std::string_view body) noexcept {
    if (status == 0 || status >= 500 || status == 429) return RefreshFailure::Transient;
    if (body.find("refresh_token_expired") != std::string_view::npos) return RefreshFailure::Expired;
    if (body.find("refresh_token_reused") != std::string_view::npos)  return RefreshFailure::Reused;
    if (body.find("refresh_token_invalidated") != std::string_view::npos
     || body.find("invalid_grant") != std::string_view::npos)          return RefreshFailure::Revoked;
    // Any other 4xx from the token endpoint won't fix itself either.
    return status == 401 || status == 400 ? RefreshFailure::Revoked : RefreshFailure::Transient;
}

std::string_view refresh_failure_message(RefreshFailure f) noexcept {
    switch (f) {
        case RefreshFailure::Expired:
            return "your ChatGPT sign-in expired \xe2\x80\x94 run `agentty login` and sign in again";
        case RefreshFailure::Reused:
            return "your ChatGPT sign-in was refreshed somewhere else and is no longer valid "
                   "\xe2\x80\x94 run `agentty login` and sign in again";
        case RefreshFailure::Revoked:
            return "your ChatGPT sign-in was revoked \xe2\x80\x94 run `agentty login` and sign in again";
        case RefreshFailure::None:
        case RefreshFailure::Transient:
            return {};
    }
    return {};
}

namespace {
// Refresh with the credential file locked against other agentty processes.
// `rejected` is the access token a server just refused, for a forced refresh.
RefreshOutcome
refresh_in_section(const maya::held<locks::cred_file>& cap, bool cross,
                   CodexCredentials loaded, std::uint64_t source_generation,
                   std::string rejected) {
    // Re-read under the lock: a peer that refreshed first has saved its
    // token, and Codex rotates refresh tokens, so adopt it instead.
    if (cross) {
        auto [disk, gen] = load_with_generation(cap);
        source_generation = gen;
        if (!disk) return {std::move(loaded), RefreshFailure::None};
        const bool peer_replaced = !rejected.empty() && disk->access_token != rejected;
        if (peer_replaced || (rejected.empty() && !disk->needs_refresh()))
            return {std::move(disk), RefreshFailure::None};
        if (disk->refresh_token.empty()) return {std::move(disk), RefreshFailure::Revoked};
        loaded = std::move(*disk);
    }

    auto refreshed = codex_refresh(loaded);
    if (!refreshed) {
        const auto& d = refreshed.error().detail;
        int status = 0;
        if (refreshed.error().kind != OAuthErrorKind::Network && d.starts_with("status="))
            status = std::atoi(d.c_str() + 7);
        else if (refreshed.error().kind == OAuthErrorKind::BadResponse)
            status = 502;   // an HTML error page from a proxy, not a verdict on the token
        const RefreshFailure why = classify_refresh_failure(status, d);
        util::dbglog("codex.refresh_failed", std::to_string(static_cast<int>(why)) + " " + d.substr(0, 200));
        return {std::move(loaded), why};
    }

    auto saved = store().with(cap, [](Store& st, std::uint64_t source_gen, CodexCredentials source,
                           CodexCredentials fresh) -> std::optional<CodexCredentials> {
        auto current = load_codex_credentials_unlocked();
        const bool source_unchanged = current
            && current->access_token == source.access_token
            && current->refresh_token == source.refresh_token
            && current->account_id == source.account_id;
        if (st.generation != source_gen || !source_unchanged) {
            // Sign-out, account activation, another process, or a new login
            // won the race. Never resurrect/overwrite it from the stale snapshot.
            return current;
        }
        if (!save_codex_credentials_unlocked(fresh)) return source;
        ++st.generation;
        return fresh;
    }, source_generation, loaded, *refreshed);
    return {std::move(saved), RefreshFailure::None};
}

RefreshOutcome refresh_locked(const maya::held<locks::auth_refresh>& cap,
                              std::string_view rejected) {
    auto [loaded, source_generation] = load_with_generation(cap);
    if (!loaded) return {};
    RefreshOutcome out;
    if (!rejected.empty() && loaded->access_token != rejected) {
        // Someone already replaced the token the server refused.
        out = {std::move(loaded), RefreshFailure::None};
    } else if (rejected.empty() && !loaded->needs_refresh()) {
        return {std::move(loaded), RefreshFailure::None};
    } else if (loaded->refresh_token.empty()) {
        out = {std::move(loaded), RefreshFailure::Revoked};
    } else {
        out = persistence::with_shared_file(
            cap, persistence::cred_lane(codex_credentials_path()), codex_credentials_path(),
            [](const maya::held<locks::cred_file>& h, persistence::Held held,
               CodexCredentials l, std::uint64_t gen, std::string rej) {
                return refresh_in_section(h, held.cross_process, std::move(l), gen, std::move(rej));
            }, *loaded, source_generation, std::string{rejected});
    }
    last_failure().store(static_cast<std::uint8_t>(out.failure), std::memory_order_relaxed);
    return out;
}
} // namespace

} // namespace agentty::provider::chatgpt
