// codex_auth_test — the ChatGPT sign-in edge cases codex-rs handles.
//
//   * a refresh that fails for good says why ("sign in again"), and one that
//     failed on the network does not
//   * a token is refreshed 5 minutes before it dies, or after 8 days anyway
//   * the server's error code survives into the detail, but not into what
//     the user reads

#include "agentty/provider/chatgpt/codex_oauth.hpp"

#include "agtest.hpp"

#include <chrono>

using namespace agentty::provider::chatgpt;

namespace {
std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
}  // namespace

TEST_CASE("codex refresh: each server answer maps to the right reason") {
    using F = RefreshFailure;
    CHECK(classify_refresh_failure(401, R"({"error":{"code":"refresh_token_expired"}})") == F::Expired);
    CHECK(classify_refresh_failure(401, R"({"error":{"code":"refresh_token_reused"}})") == F::Reused);
    CHECK(classify_refresh_failure(401, R"({"error":{"code":"refresh_token_invalidated"}})") == F::Revoked);
    CHECK(classify_refresh_failure(400, "invalid_grant: bad token") == F::Revoked);
    // No verdict on the token: try again later, say nothing scary.
    CHECK(classify_refresh_failure(0, "connection reset") == F::Transient);
    CHECK(classify_refresh_failure(503, "upstream down") == F::Transient);
    CHECK(classify_refresh_failure(429, "slow down") == F::Transient);

    CHECK(!refresh_failure_message(F::Expired).empty());
    CHECK(refresh_failure_message(F::Expired).find("agentty login") != std::string_view::npos);
    CHECK(refresh_failure_message(F::Transient).empty());
    CHECK(refresh_failure_message(F::None).empty());
}

TEST_CASE("codex refresh: 5 minutes early, and after 8 days regardless") {
    CodexCredentials c;
    c.access_token = "a";
    c.last_refresh_ms = now_ms();

    c.expires_at_ms = now_ms() + 60LL * 60 * 1000;          // an hour left
    CHECK(!c.needs_refresh());
    c.expires_at_ms = now_ms() + 4LL * 60 * 1000;           // 4 minutes left
    CHECK(c.needs_refresh(), "inside the 5-minute window");

    c.expires_at_ms = now_ms() + 60LL * 60 * 1000;
    c.last_refresh_ms = now_ms() - 9LL * 24 * 3600 * 1000;  // 9 days old
    CHECK(c.needs_refresh(), "an old login is refreshed even if the token looks fine");

    c.last_refresh_ms = 0;                                  // saved before we tracked it
    c.expires_at_ms = 0;
    CHECK(!c.needs_refresh(), "no information, no forced refresh");
}

TEST_CASE("codex auth: the error code is kept, the user doesn't see the tag") {
    agentty::auth::OAuthError e{agentty::auth::OAuthErrorKind::ApiError,
                                "status=401 refresh_token_expired: Your session expired"};
    const auto shown = e.render();
    CHECK(shown.find("status=") == std::string::npos);
    CHECK(shown.find("refresh_token_expired") != std::string::npos);
}

TEST_CASE("codex login: the redirect follows the port that was bound") {
    agentty::auth::PkceVerifier v{"verifier"};
    agentty::auth::OAuthState s{"state"};
    const auto def = codex_authorize_url(v, s);
    CHECK(def.find("localhost%3A1455") != std::string::npos);
    const auto alt = codex_authorize_url(v, s, "http://localhost:1457/auth/callback");
    CHECK(alt.find("localhost%3A1457") != std::string::npos);
}
