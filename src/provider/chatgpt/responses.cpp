// agentty::provider::chatgpt — the ChatGPT/Codex **host** for the shared
// Responses-API codec.
//
// The dialect machinery (conversation → input[], tools[], the SSE state
// machine) used to live here; it was extracted to
// src/provider/responses/codec.cpp once we measured that GitHub Copilot
// speaks the exact same wire. What remains is precisely what is TRUE OF
// CHATGPT AND NOTHING ELSE:
//
//   • the endpoint            chatgpt.com/backend-api/codex/responses
//   • ChatGPT OAuth creds     (auto-refreshing) + chatgpt-account-id
//   • the prompt-cache session id derived from the thread id
//   • `store:false` + include[reasoning.encrypted_content]
//   • Codex-flavoured HTTP error prose ("run `agentty login`…")
//   • the live /models catalog for this account
//
// Everything else is shared. Adding a Responses backend is a Site row like
// the one at the bottom of this file — not a second copy of the codec.
#include "agentty/provider/chatgpt/responses.hpp"
#include "agentty/provider/responses/responses.hpp"

#include <maya/runtime.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "agentty/provider/chatgpt/codex_oauth.hpp"
#include "agentty/provider/chatgpt/oauth.hpp"
#include "agentty/io/http.hpp"
#include "agentty/util/dbglog.hpp"

namespace agentty::provider::chatgpt {
namespace {
using json = nlohmann::json;

// UTF-8 scrub — a pasted blob / tool output can carry invalid UTF-8 that
// nlohmann::dump() would throw on. Replace malformed bytes with U+FFFD.
// (Local copy: the codec keeps its own for the body it builds; this one is
// only used by the error formatter below.)
std::string scrub_utf8(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    const auto* p = reinterpret_cast<const unsigned char*>(in.data());
    const auto* end = p + in.size();
    while (p < end) {
        unsigned char c = *p;
        int extra = 0;
        if (c < 0x80) { out.push_back(static_cast<char>(c)); ++p; continue; }
        else if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else { out.append("\xEF\xBF\xBD"); ++p; continue; }
        if (p + extra >= end) { out.append("\xEF\xBF\xBD"); ++p; continue; }
        bool ok = true;
        for (int k = 1; k <= extra; ++k)
            if ((p[k] & 0xC0) != 0x80) { ok = false; break; }
        if (!ok) { out.append("\xEF\xBF\xBD"); ++p; continue; }
        out.append(reinterpret_cast<const char*>(p), extra + 1);
        p += extra + 1;
    }
    return out;
}

std::string format_http_error(int status, std::string_view body) {
    const std::string generic = "Codex backend returned HTTP " + std::to_string(status);
    std::string message = generic;

    auto decode = [&](const json& j) -> bool {
        const json* detail = &j;
        std::string tag;
        if (j.is_object() && j.contains("error")) detail = &j["error"];
        if (detail->is_string()) {
            message = detail->get<std::string>();
            return !message.empty();
        }
        if (!detail->is_object()) return false;
        tag = detail->value("type", detail->value("code", std::string{}));
        std::string text = detail->value("message", std::string{});
        if (text.empty() && detail->contains("detail") && (*detail)["detail"].is_string())
            text = (*detail)["detail"].get<std::string>();
        if (text.empty() && detail != &j && j.contains("message") && j["message"].is_string())
            text = j["message"].get<std::string>();
        if (text.empty()) return false;
        message = tag.empty() ? std::move(text) : tag + ": " + text;
        return true;
    };

    bool decoded = false;
    try { decoded = decode(json::parse(body)); }
    catch (const std::exception& e) {
        util::dbglog("chatgpt.http_error.decode", e.what());
    } catch (...) {}

    // Some edge responses retain SSE framing even on an HTTP error. Decode
    // their `data: {...}` payload instead of showing the raw frame.
    if (!decoded) {
        std::size_t pos = 0;
        while (pos < body.size()) {
            const auto nl = body.find('\n', pos);
            std::string_view line = body.substr(pos, nl == std::string_view::npos
                                                        ? std::string_view::npos
                                                        : nl - pos);
            const auto end = nl;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.remove_suffix(1);
            constexpr std::string_view kData = "data:";
            if (line.size() > kData.size() && line.substr(0, kData.size()) == kData) {
                std::string_view payload = line.substr(kData.size());
                while (!payload.empty() && payload.front() == ' ')
                    payload.remove_prefix(1);
                try {
                    if (decode(json::parse(payload))) { decoded = true; break; }
                } catch (...) {}
            }
            if (end == std::string_view::npos) break;
            pos = end + 1;
        }
    }

    if (!decoded && !body.empty()) {
        std::string raw = scrub_utf8(body.substr(0, 1024));
        while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n' || raw.back() == ' '))
            raw.pop_back();
        if (!raw.empty()) message = generic + ": " + raw;
    }
    if (status == 401)
        message += " — session expired; run `agentty login` and sign in to ChatGPT again";
    return message;
}

std::string new_uuid_v4() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uint64_t a = rng(), b = rng();
    a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;   // version 4
    b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;   // variant 1
    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%012llx",
        static_cast<unsigned>(a >> 32),
        static_cast<unsigned>((a >> 16) & 0xFFFF),
        static_cast<unsigned>(a & 0xFFFF),
        static_cast<unsigned>(b >> 48),
        static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFULL));
    return std::string{buf};
}

// Stable per-conversation session id for the Responses backend.
//
// The Codex backend keys prompt caching on this header, so successive turns
// of the SAME conversation that carry the SAME id hit the cached prefix
// (system instructions + tool schemas + prior turns), while a fresh id every
// turn forces a full re-encode — exactly what a random uuid-per-request did,
// silently defeating caching on every hosted ChatGPT turn. codex-rs keeps one
// id for the whole session; we do the same by deriving a deterministic v4-
// shaped uuid from the caller's stable session_key (the thread id). When no
// session_key is supplied (no durable conversation identity) we fall back to a
// random id — correctness first, caching only when we can be stable.
std::string session_id_for(std::string_view session_key) {
    if (session_key.empty()) return new_uuid_v4();
    // FNV-1a over the key → 128 bits, then formatted as a v4 uuid so the
    // backend accepts it. Deterministic: same conversation → same id every
    // turn → cache continuity.
    auto fnv = [](std::string_view s, std::uint64_t seed) {
        std::uint64_t h = seed;
        for (unsigned char c : s) { h ^= c; h *= 0x00000100000001B3ULL; }
        return h;
    };
    std::uint64_t a = fnv(session_key, 0xcbf29ce484222325ULL);
    std::uint64_t b = fnv(session_key, 0x84222325cbf29ce4ULL);
    a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;   // version 4
    b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;   // variant 1
    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%012llx",
        static_cast<unsigned>(a >> 32),
        static_cast<unsigned>((a >> 16) & 0xFFFF),
        static_cast<unsigned>(a & 0xFFFF),
        static_cast<unsigned>(b >> 48),
        static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFULL));
    return std::string{buf};
}

// ── The ChatGPT Site ──────────────────────────────────────────────────────

std::expected<responses::Target, std::string>
chatgpt_authorize(const maya::unlocked& cap, provider::Request& req) {
    auto creds = codex_fresh_credentials(cap);   // auto-refreshes if stale
    if (!creds || creds->access_token.empty())
        return std::unexpected(
            std::string{"not signed in to ChatGPT \xe2\x80\x94 run `agentty login` and "
                        "choose ChatGPT, or use --provider with an API key"});
    // The refresh failed for good (expired / reused / revoked): sending the
    // dead token would only come back as a confusing 401.
    if (const auto why = refresh_failure_message(codex_last_refresh_failure());
        !why.empty() && creds->expired())
        return std::unexpected(std::string{"401 "} + std::string{why});

    responses::Target t;
    t.host = "chatgpt.com";
    t.port = 443;
    t.path = "/backend-api/codex/responses";
    // codex-rs sends the conversation id three ways: `session_id` (legacy),
    // `session-id` + `thread-id`, and `x-client-request-id`. The backend keys
    // routing and prompt-cache stickiness on them, so send the same set.
    const std::string sid = session_id_for(req.session_key);
    t.headers = {
        {"authorization",     "Bearer " + creds->access_token},
        {"content-type",      "application/json"},
        {"accept",            "text/event-stream"},
        {"openai-beta",       "responses=experimental"},
        {"originator",        OAuthConfig::originator},
        {"session_id",        sid},
        {"session-id",        sid},
        {"thread-id",         sid},
        {"x-client-request-id", sid},
        {"user-agent",        std::string("codex_cli_rs/") + OAuthConfig::codex_client_version},
    };
    if (!creds->account_id.empty())
        t.headers.push_back({"chatgpt-account-id", creds->account_id});
    // The hosted Codex backend's default slug when the caller didn't pin one.
    t.model = (req.model.empty() || req.model == "chatgpt-default")
                  ? "gpt-5-codex" : req.model;
    return t;
}

void chatgpt_decorate_body(json& body, const provider::Request& req) {
    // The Codex backend runs stateless: we replay the whole conversation each
    // turn, so nothing should be stored server-side.
    body["store"] = false;
    // Same stable key codex-rs derives from the session, so turns of one
    // conversation hit the same prompt cache.
    if (!req.session_key.empty()) body["prompt_cache_key"] = session_id_for(req.session_key);
    // Ask the backend to return each reasoning item's encrypted_content so we
    // can replay it in input[] next turn. Under store:false this is the ONLY
    // way to carry chain-of-thought across tool rounds — without it a
    // reasoning model re-derives its plan from scratch each round (worse
    // answers, wasted tokens). Mirrors codex-rs.
    body["include"] = json::array({"reasoning.encrypted_content"});
}

// A 401 on a token that looked valid: refresh it now and resend once.
std::expected<responses::Target, std::string>
chatgpt_reauthorize(const maya::unlocked& cap, provider::Request& req,
                    const responses::Target& rejected) {
    std::string old_token;
    for (const auto& [k, v] : rejected.headers)
        if (k == "authorization" && v.starts_with("Bearer ")) old_token = v.substr(7);
    const auto out = codex_force_refresh(cap, old_token);
    if (const auto why = refresh_failure_message(out.failure); !why.empty())
        return std::unexpected(std::string{"401 "} + std::string{why});
    if (!out.creds || out.creds->access_token == old_token)
        return std::unexpected(std::string{
            "401 ChatGPT rejected the sign-in \xe2\x80\x94 run `agentty login` and sign in again"});
    return chatgpt_authorize(cap, req);
}

const responses::Site kChatGptSite{
    .id                 = "chatgpt",
    .authorize          = &chatgpt_authorize,
    .decorate_body      = &chatgpt_decorate_body,
    .explain_http_error = &format_http_error,
    .reauthorize        = &chatgpt_reauthorize,
};

} // namespace

bool responses_available() {
    // Called by the provider-picker VIEW once per rendered frame. The naive
    // path (read codex_credentials.json → unseal → JSON parse) costs disk +
    // AES work per frame while the picker is open. Cache the boolean keyed
    // on the file's (mtime, size): a stat is ~1µs and invalidates correctly
    // on sign-in, sign-out, and cross-process refreshes alike.
    namespace fs = std::filesystem;
    struct Cache {
        bool           ok = false;
        std::int64_t   mtime = 0;   // file_time_type ticks: a Sendable key
        std::uintmax_t size = static_cast<std::uintmax_t>(-1);
    };
    static maya::snapshot<Cache> cache;
    std::error_code ec;
    const auto p = codex_credentials_path();
    const std::int64_t mtime = fs::last_write_time(p, ec).time_since_epoch().count();
    const auto size  = ec ? 0 : fs::file_size(p, ec);
    if (ec) {   // missing/unreadable → signed out; remember that cheaply
        cache.store(Cache{});
        return false;
    }
    if (auto hit = cache.load(); hit->mtime == mtime && hit->size == size)
        return hit->ok;
    // Changed: unseal + parse, then publish.
    auto c = load_codex_credentials();
    const bool ok = c && !c->access_token.empty();
    cache.store(Cache{ok, mtime, size});
    return ok;
}

std::string codex_client_version_for_test(std::string_view override_version) {
    // Accept only dotted numeric versions, bounded for use in URL and headers.
    if (override_version.empty() || override_version.size() > 32)
        return OAuthConfig::codex_client_version;
    int dots = 0;
    bool digit = false;
    for (char c : override_version) {
        if (c >= '0' && c <= '9') digit = true;
        else if (c == '.' && digit) { ++dots; digit = false; }
        else return OAuthConfig::codex_client_version;
    }
    return dots == 2 && digit ? std::string(override_version) : OAuthConfig::codex_client_version;
}

static std::string codex_client_version() {
    const char* value = std::getenv("AGENTTY_CODEX_CLIENT_VERSION");
    return codex_client_version_for_test(value ? std::string_view(value) : std::string_view{});
}

// ── Live model catalog ────────────────────────────────────────────────────
std::vector<CatalogModel> fetch_models(Io io, const maya::unlocked& cap) {
    auto creds = codex_fresh_credentials(cap);
    if (!creds || creds->access_token.empty()) return {};

    const auto client_version = codex_client_version();
    http::Request hr;
    hr.method = http::HttpMethod::Get;
    hr.host   = "chatgpt.com";
    hr.port   = 443;
    hr.path   = std::string("/backend-api/codex/models?client_version=")
              + client_version;
    hr.headers = {
        {"authorization", "Bearer " + creds->access_token},
        {"accept",        "application/json"},
        {"openai-beta",   "responses=experimental"},
        {"originator",    OAuthConfig::originator},
        {"user-agent",    std::string("codex_cli_rs/") + client_version},
    };
    if (!creds->account_id.empty())
        hr.headers.push_back({"chatgpt-account-id", creds->account_id});

    http::Timeouts tos;
    tos.connect = std::chrono::milliseconds(8'000);
    tos.total   = std::chrono::milliseconds(15'000);
    // Catalogs can be large; bound the response to limit memory consumption.
    hr.max_body_bytes = 2ull * 1024 * 1024;

    auto result = http::default_client(io).send(hr, tos);
    if (!result || result->status < 200 || result->status >= 300) return {};

    return parse_models_payload_for_test(result->body);
}

std::vector<CatalogModel> parse_models_payload_for_test(std::string_view body) {
    std::vector<CatalogModel> out;
    std::unordered_set<std::string> seen;
    json j;
    try {
        j = json::parse(body.begin(), body.end(), nullptr, false);
    } catch (const json::exception&) {
        return out;
    }
    if (j.is_discarded()) return out;
    const auto& arr = j.is_object() && j.contains("models") ? j["models"] : j;
    if (!arr.is_array()) return out;
    for (const auto& m : arr) {
        if (!m.is_object()) continue;
        auto str = [&](const char* key, std::string fallback) {
            auto it = m.find(key);
            return it != m.end() && it->is_string() ? it->get<std::string>() : fallback;
        };
        auto boolean = [&](const char* key, bool fallback) {
            auto it = m.find(key);
            return it != m.end() && it->is_boolean() ? it->get<bool>() : fallback;
        };
        auto integer = [&](const char* key, int fallback, int lo, int hi) {
            auto it = m.find(key);
            if (it == m.end() || !it->is_number_integer()) return fallback;
            if (it->is_number_unsigned()) {
                const auto n = it->get<std::uint64_t>();
                return n <= static_cast<std::uint64_t>(hi) &&
                       (lo <= 0 || n >= static_cast<std::uint64_t>(lo))
                    ? static_cast<int>(n) : fallback;
            }
            const auto n = it->get<std::int64_t>();
            return n >= lo && n <= hi ? static_cast<int>(n) : fallback;
        };
        const auto vis = str("visibility", "list");
        if (vis == "hide" || vis == "hidden") continue;
        if (!boolean("supported_in_api", true)) continue;
        CatalogModel cm;
        cm.slug = str("slug", str("id", {}));
        if (cm.slug.empty() || !seen.insert(cm.slug).second) continue;
        cm.display_name = str("display_name", cm.slug);
        cm.context_window = integer("context_window", cm.context_window, 1, 20000000);
        cm.is_default = boolean("is_default", false);
        // Invalid or absent priorities sort after every valid priority.
        cm.priority = integer("priority", 1000001, -1000000, 1000000);
        out.push_back(std::move(cm));
    }
    std::stable_sort(out.begin(), out.end(), [](const CatalogModel& a, const CatalogModel& b) {
        return a.priority < b.priority;
    });
    return out;
}

provider::StreamResult stream_responses(const maya::unlocked& cap,
                                        provider::Request req,
                                        provider::EventSink sink) {
    return responses::stream(cap, kChatGptSite, std::move(req), std::move(sink));
}

// ── Test seams ────────────────────────────────────────────────────────────
// These keep their historical ChatGPT-flavoured behaviour: build_body_for_test
// returns the body AS SENT (neutral codec body + this host's decorations), so
// the existing expectations about store/include still hold.
nlohmann::json build_body_for_test(const provider::Request& req) {
    provider::Request r = req;
    // Build FOR this site, exactly as stream() does. The seam claims to
    // return the body as sent, so it has to pass the same site id —
    // otherwise encrypted-reasoning replay is gated off here and on
    // nowhere else, and the test would be asserting a body no request
    // ever produces.
    json body = responses::build_body(r, kChatGptSite.id);
    if (r.model.empty() || r.model == "chatgpt-default")
        body["model"] = "gpt-5-codex";
    chatgpt_decorate_body(body, r);
    return body;
}

std::string session_id_for_test(std::string_view session_key) {
    return session_id_for(session_key);
}

std::string format_http_error_for_test(int status, std::string_view body) {
    return format_http_error(status, body);
}

std::vector<Msg> parse_sse_for_test(const std::vector<std::string>& sse_data_lines,
                                    bool show_reasoning) {
    return responses::parse_sse_for_test(sse_data_lines, show_reasoning);
}

} // namespace agentty::provider::chatgpt
