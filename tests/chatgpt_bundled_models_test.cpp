// chatgpt_bundled_models_test — guards the ChatGPT (Codex) provider's model
// discovery against two regressions that broke the picker:
//
//   1. The retired `gpt-5.1-codex` slug. The server withdrew it and now rejects
//      it on the first turn ("model is not supported when using Codex with a
//      ChatGPT account"). It must NEVER appear in list_models() nor be returned
//      by default_model() — not from the live catalog, not from the offline
//      bundled fallback.
//   2. Hidden/internal models (e.g. `codex-auto-review`, visibility="hide")
//      must never surface in the user-facing catalog.
//
// The test adapts to the environment:
//   • unsigned  → exercises the OFFLINE bundled fallback (must be `gpt-5`).
//   • signed in → exercises the LIVE catalog (must be non-empty, real slugs).
// Either way the retired slug must be absent and a model must be selectable.
//
// Run: build the `chatgpt_bundled_models_test` target, execute. Exit 0 = pass.

#include <string>
#include <vector>

#include "agtest.hpp"

#include "agentty/provider/chatgpt/provider.hpp"
#include "agentty/provider/chatgpt/oauth.hpp"
#include "agentty/provider/chatgpt/responses.hpp"

using namespace agentty;
namespace cc = agentty::provider::chatgpt;

TEST_CASE("chatgpt catalog parsing filters and orders models") {
    const auto models = cc::parse_models_payload_for_test(R"({"models":[
        {"slug":"later","priority":20,"context_window":12345},
        {"slug":"hidden","visibility":"hide"},
        {"slug":"hidden2","visibility":"hidden"},
        {"slug":"not-api","supported_in_api":false,"priority":4},
        {"slug":"other","visibility":"internal","priority":5},
        {"slug":"first","priority":1,"is_default":true},
        {"slug":"first","priority":2},
        {"id":"fallback","priority":3,"context_window":-1},
        null, {"slug":42}
    ]})");
    REQUIRE(models.size() == 4);
    CHECK(models[0].slug == "first");
    CHECK(models[0].is_default);
    CHECK(models[1].slug == "fallback");
    CHECK(models[1].context_window == 272000);
    CHECK(models[2].slug == "other");
    CHECK(models[3].slug == "later");
    CHECK(models[3].context_window == 12345);
    CHECK(cc::parse_models_payload_for_test("not json").empty());
    CHECK(cc::parse_models_payload_for_test(R"({"models":{}})").empty());
    CHECK(cc::parse_models_payload_for_test(R"({"models":[{"slug":"bad","priority":1e1000}]})").empty());
}

TEST_CASE("chatgpt catalog prioritizes valid priorities before missing or invalid ones") {
    const auto models = cc::parse_models_payload_for_test(R"({"models":[
        {"slug":"missing"},
        {"slug":"unsigned-overflow","priority":18446744073709551615,"context_window":18446744073709551615},
        {"slug":"invalid","priority":"1"},
        {"slug":"out-of-range","priority":1000001},
        {"slug":"last-valid","priority":1000000},
        {"slug":"first","priority":1},
        {"slug":"equal","priority":1}
    ]})");
    REQUIRE(models.size() == 7);
    CHECK(models[0].slug == "first");
    CHECK(models[1].slug == "equal");
    CHECK(models[2].slug == "last-valid");
    CHECK(models[3].slug == "missing");
    CHECK(models[4].slug == "unsigned-overflow");
    CHECK(models[4].context_window == 272000);
    CHECK(models[5].slug == "invalid");
    CHECK(models[6].slug == "out-of-range");
}

TEST_CASE("chatgpt codex version override validation") {
    const std::string fallback = cc::OAuthConfig::codex_client_version;
    CHECK(cc::codex_client_version_for_test("") == fallback);
    CHECK(cc::codex_client_version_for_test("1.2.3") == "1.2.3");
    CHECK(cc::codex_client_version_for_test("1.2") == fallback);
    CHECK(cc::codex_client_version_for_test("1..2.3") == fallback);
    CHECK(cc::codex_client_version_for_test("1.2.3\r\nX:bad") == fallback);
    CHECK(cc::codex_client_version_for_test("1.2.3&foo=bar") == fallback);
    CHECK(cc::codex_client_version_for_test(std::string(33, '1')) == fallback);
}

TEST_CASE("chatgpt bundled model catalog") {
    const bool signed_in = cc::responses_available();

    auto models = cc::list_models(::agentty::IoAccess::grant(), agtest::cap());
    CHECK(!models.empty());   // a model must ALWAYS be selectable

    // The retired, server-rejected slug must never surface — this is the exact
    // failure the user hit (picker defaulting to `gpt-5.1-codex` → first turn
    // 400s). Also assert the internal auto-review model is filtered out.
    for (const auto& m : models) {
        CHECK(m.id.value != "gpt-5.1-codex");
        CHECK(m.id.value != "codex-auto-review");
    }

    // The default (catalog index 0) must be a live, selectable slug.
    const std::string def = cc::default_model(::agentty::IoAccess::grant(), agtest::cap());
    CHECK(!def.empty());
    CHECK(def != "gpt-5.1-codex");

    if (signed_in) {
        // Live catalog path: the fetched list must contain real slugs and the
        // default must be one of them (never the bundled placeholder blindly).
        bool default_in_list = false;
        for (const auto& m : models)
            if (m.id.value == def) { default_in_list = true; break; }
        CHECK(default_in_list);
    } else {
        // Offline bundled fallback: must be the stable, always-accepted `gpt-5`.
        CHECK(def == "gpt-5");
    }

    (void)signed_in;
}
