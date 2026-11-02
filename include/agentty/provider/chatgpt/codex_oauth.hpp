#pragma once
// agentty::provider::chatgpt — native ChatGPT (Codex) OAuth.
//
// This is the "log in with ChatGPT, just like Claude" path: agentty runs the
// Authorization-Code + PKCE (S256) flow against auth.openai.com itself, mints
// the tokens (and, via RFC-8693 token-exchange, a usable OpenAI API key),
// and stores them under its OWN encrypted credential file — no dependency on
// the `codex` binary and no touching of ~/.codex secrets.
//
// It reuses the shared PKCE / HTTP / crypto helpers in `agentty::auth`; only
// the OpenAI-specific wiring (endpoints, the loopback callback server, the
// api-key exchange, the separate token store) lives here.

#include <cstdint>
#include <expected>
#include <functional>
#include <stop_token>
#include <optional>
#include <string>

#include "agentty/auth/auth.hpp"
#include "agentty/provider/chatgpt/oauth.hpp"
#include "agentty/util/io.hpp"
#include "agentty/util/lock_levels.hpp"

#include <maya/runtime.hpp>

namespace agentty::provider::chatgpt {

// The persisted ChatGPT credential. `api_key` is the OpenAI key minted from
// the id_token — preferred for API calls; `access_token` is the raw OAuth
// bearer (used against chatgpt.com/backend-api/codex with `account_id`).
struct CodexCredentials {
    std::string  access_token;
    std::string  refresh_token;
    std::string  id_token;
    std::string  account_id;   // chatgpt_account_id claim from the id_token
    std::string  api_key;      // minted OpenAI key (may be empty)
    std::int64_t expires_at_ms = 0;
    // When the tokens were last obtained (login or refresh). Codex refreshes
    // anything older than 8 days even when the access token looks valid, so a
    // long-idle login doesn't die on the first request after a break.
    std::int64_t last_refresh_ms = 0;

    [[nodiscard]] bool empty() const noexcept { return access_token.empty(); }
    [[nodiscard]] bool expired(std::int64_t skew_ms = 0) const noexcept;
    // Time to refresh: within 5 minutes of expiry (read from the token's own
    // `exp` when the server gave no expires_in), or last refreshed over 8
    // days ago. Same rules as codex-rs.
    [[nodiscard]] bool needs_refresh() const noexcept;
};

// Why a refresh failed. Permanent ones mean "sign in again"; a retry can't
// help. The names follow the auth server's error codes.
enum class RefreshFailure : std::uint8_t {
    None,
    Expired,       // refresh_token_expired
    Reused,        // refresh_token_reused: another client rotated it first
    Revoked,       // refresh_token_invalidated / invalid_grant
    Transient,     // network, 5xx: worth another try later
};
[[nodiscard]] RefreshFailure classify_refresh_failure(int status, std::string_view body) noexcept;
// One line for the user, or empty for None/Transient.
[[nodiscard]] std::string_view refresh_failure_message(RefreshFailure) noexcept;

// ── Token store (separate from the Anthropic credentials.json) ─────────────
[[nodiscard]] std::filesystem::path codex_credentials_path();
// Saves/clears take the store's leaf lock; the load is lock-free (atomic
// rename), so views and signed-in probes can call it without a capability.
[[nodiscard]] std::optional<CodexCredentials> load_codex_credentials();
bool save_codex_credentials(const maya::below<maya::leaf>& cap, const CodexCredentials& c);
bool clear_codex_credentials(const maya::below<maya::leaf>& cap);

// ── Authorize URL ──────────────────────────────────────────────────────────
// Pure: same (verifier, state) → same URL. Points at the loopback redirect
// (port 1455 unless login fell back to 1457).
[[nodiscard]] std::string codex_authorize_url(const auth::PkceVerifier& verifier,
                                              const auth::OAuthState&   state,
                                              std::string_view redirect_uri = OAuthConfig::redirect_uri);

// ── Interactive login ──────────────────────────────────────────────────────
// SSH sessions automatically use OpenAI's device-code flow: agentty displays
// a URL + one-time code and polls until the browser on any machine approves
// it. Local sessions retain the loopback callback flow on port 1455.
struct CodexDeviceCode {
    std::string verification_url;
    std::string user_code;
};

using CodexDeviceCodeSink = std::function<void(const CodexDeviceCode&)>;
// Login stops when this is requested (Esc, a newer attempt, shutdown).
using CodexCancelProbe = std::stop_token;

// True when the environment requests device auth explicitly or identifies an
// SSH session. AGENTTY_CHATGPT_DEVICE_AUTH=0/1 is an override for unusual
// terminals and test harnesses.
[[nodiscard]] bool codex_device_auth_preferred() noexcept;

// Runs the appropriate full flow and returns credentials without persisting
// them. The caller must save only after it confirms the attempt is still
// active. `on_device_code` fires once before polling begins.
[[nodiscard]] std::expected<CodexCredentials, auth::OAuthError>
codex_login(Io, const maya::unlocked&, int timeout_s = 900, CodexDeviceCodeSink on_device_code = {},
            CodexCancelProbe cancelled = {});

// Explicit device flow with the same side-effect-free completion contract.
[[nodiscard]] std::expected<CodexCredentials, auth::OAuthError>
codex_device_login(const maya::unlocked&, CodexDeviceCodeSink on_device_code, int timeout_s = 900,
                   CodexCancelProbe cancelled = {});

// ── Exchange / refresh / mint (exposed for the reducer + tests) ────────────
// Exchange an authorization code (from the callback) for tokens.
[[nodiscard]] std::expected<CodexCredentials, auth::OAuthError>
codex_exchange_code(const auth::OAuthCode& code, const auth::PkceVerifier& verifier,
                    std::string_view redirect_uri = OAuthConfig::redirect_uri);

// Refresh an expired access token via the refresh_token (JSON body, per the
// Codex protocol). Returns a fresh credential; re-mints the API key.
[[nodiscard]] std::expected<CodexCredentials, auth::OAuthError>
codex_refresh(const CodexCredentials& current);

// Return a valid credential, refreshing in place (and persisting) if the
// stored access token is stale. Worker-thread safe. Empty optional when no
// ChatGPT credential is saved. Refreshes run one at a time on the
// auth_refresh lane, which then takes the (leaf) store.
[[nodiscard]] std::optional<CodexCredentials>
codex_fresh_credentials(const locks::below_auth_refresh& cap);

// What a refresh attempt came to. `creds` is what to use next (fresh on
// success, the stored ones otherwise); `failure` says why it didn't work.
struct RefreshOutcome {
    std::optional<CodexCredentials> creds;
    RefreshFailure                  failure = RefreshFailure::None;
};

// The server said 401 with this access token: refresh now, whatever the
// expiry says, unless another caller already replaced that token (then
// adopt theirs). Same as codex-rs's unauthorized recovery.
[[nodiscard]] RefreshOutcome
codex_force_refresh(const locks::below_auth_refresh& cap, std::string_view rejected_access_token);

// The last refresh failure seen by codex_fresh_credentials, so a request
// can say "sign in again" instead of sending a dead token. Cleared on success.
[[nodiscard]] RefreshFailure codex_last_refresh_failure() noexcept;

} // namespace agentty::provider::chatgpt
