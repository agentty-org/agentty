// custom_host_key_prompt_test — how a custom host gets added. Every host is
// probed first, TLS or not, because plenty of remote hosts need no key. The
// key prompt opens only when the probe gets a 401/403 (key required) or the
// host already has a saved key (to check or change it). On an optional
// prompt, an empty Enter probes keyless instead of failing. Esc at the key
// prompt cancels the switch.
//
// Pure reducer test: stubs deps() with an in-memory store::Settings and
// sets XDG_CONFIG_HOME to a temp dir so auth::load_credentials() returns
// nullopt (no on-disk creds interfere with the non-TLS path). The test
// detects whether commit_provider_switch ran via m.s.models_loading (which
// commit_provider_switch sets at modal.cpp:691) — same indirection
// codex_login_flow_test uses for the ChatGPT path.
//
// Run: build the `custom_host_key_prompt_test` target, execute. Exit 0 = pass.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/login.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/store/store.hpp"
#include "agentty/provider/selection.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

#include "agtest.hpp"
#include "agtest_fx.hpp"

namespace fs = std::filesystem;
namespace login = agentty::ui::login;
namespace msg = agentty::msg;
namespace app = agentty::app;

// Helper: install deps() with an in-memory Settings captured by reference.
// The other Deps members are stubbed to no-ops since login_submit's
// CustomHostInput arm only calls deps().load_settings() and deps().auth.
static void install_stub_deps(agentty::store::Settings& s) {
    app::install_deps(app::Deps{
        .stream         = [](const maya::unlocked&, auto, auto) {},
        .save_thread    = [](const auto&) {},
        .delete_thread  = [](const auto&) {},
        .load_threads   = [] { return std::vector<agentty::Thread>{}; },
        .load_thread    = [](const auto&) -> std::optional<agentty::Thread> { return std::nullopt; },
        .load_settings  = [&s] { return s; },
        .save_settings  = [&s](const agentty::store::Settings& x) { s = x; },
        .new_thread_id  = [] { return agentty::ThreadId{}; },
        .title_from     = [](std::string_view t) { return std::string{t}; },
        .auth           = agentty::auth::AuthHeader{agentty::auth::ApiKeyHeader{std::string{}}},
    });
}

TEST_CASE("setting up a provider does not switch to it") {
    // AUTHENTICATING IS NOT SELECTING. Adding a host or pasting a key used
    // to end in commit_provider_switch, which moves the active provider AND
    // picks a model for it -- so registering a backup key while mid-task
    // silently changed the model out from under you.
    fs::path tmp_xdg = fs::temp_directory_path() /
        ("agentty-noswitch-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::create_directories(tmp_xdg, ec);
    ::setenv("AGENTTY_HOME", tmp_xdg.c_str(), 1);
    agentty::provider::set_custom_auth_header(::agentty::IoAccess::grant(), "");

    // ── A host added while ALREADY configured leaves the session alone ──
    {
        agentty::store::Settings s;
        s.provider_keys["anthropic"] = "sk-ant-existing";   // already set up
        install_stub_deps(s);

        agentty::Model m;
        m.d.persisted = s;
        m.d.auth.loaded = true;
        m.d.auth.providers["anthropic"] =
            agentty::auth::ProviderAuth{.stored = true};
        m.d.model_id = agentty::ModelId{"claude-opus-4-5"};
        const auto before_model = m.d.model_id;
        const auto before_kind  = m.d.selection.kind;
        // host_probed drops a result whose attempt id doesn't match the
        // live HostProbing state, so the picker has to actually be there.
        m.ui.login = login::HostProbing{.spec = "localhost:11434",
                                        .attempt_id = 1};

        agentty::HostProbed r;
        r.attempt_id = 1;
        r.spec = "localhost:11434";
        r.ok = true;
        r.model_count = 7;
        r.latency_ms = 12;
        auto [m2, cmd] = ::agentty::app::detail::step(
            ::agentty::app::detail::login_update, std::move(m),
            msg::LoginMsg{std::move(r)});

        check(m2.d.model_id == before_model,
              "adding a host must not change the active model");
        check(m2.d.selection.kind == before_kind,
              "adding a host must not change the active provider");
        // But it IS saved, so the picker can offer it.
        check(m2.d.persisted.provider_keys.count("localhost:11434") == 1,
              "the host is still registered");
        check(m2.s.status.find("^P") != std::string::npos,
              "the toast names how to switch");
    }

    // ── First run: nothing configured, so adopting IS the right move ──
    {
        agentty::store::Settings s;   // empty: no keys anywhere
        install_stub_deps(s);

        agentty::Model m;
        m.d.persisted = s;
        m.d.auth.loaded = true;
        m.ui.login = login::HostProbing{.spec = "localhost:11434",
                                        .attempt_id = 1};

        agentty::HostProbed r;
        r.attempt_id = 1;
        r.spec = "localhost:11434";
        r.ok = true;
        r.model_count = 3;
        r.latency_ms = 9;
        auto [m2, cmd] = ::agentty::app::detail::step(
            ::agentty::app::detail::login_update, std::move(m),
            msg::LoginMsg{std::move(r)});

        check(m2.d.selection.kind == agentty::provider::Kind::OpenAI,
              "first run adopts the host -- there is no session to protect");
        check(m2.s.status.find("^P") == std::string::npos,
              "and does not tell you to switch to what you are already on");
    }
    // ── Pasting an API KEY for another provider also leaves you put ──
    {
        agentty::store::Settings s;
        s.provider_keys["anthropic"] = "sk-ant-existing";
        install_stub_deps(s);

        agentty::Model m;
        m.d.persisted = s;
        m.d.auth.loaded = true;
        m.d.auth.providers["anthropic"] =
            agentty::auth::ProviderAuth{.stored = true};
        m.d.model_id = agentty::ModelId{"claude-opus-4-5"};
        const auto before_model = m.d.model_id;
        const auto before_kind  = m.d.selection.kind;

        login::ApiKeyInput api;
        api.provider       = "groq";
        api.provider_label = "Groq";
        api.key_input      = "gsk_testkey";
        m.ui.login = std::move(api);

        auto [m2, cmd] = ::agentty::app::detail::step(
            ::agentty::app::detail::login_update, std::move(m),
            msg::LoginMsg{agentty::LoginSubmit{}});

        check(m2.d.model_id == before_model,
              "pasting a Groq key must not change the model you are on");
        check(m2.d.selection.kind == before_kind,
              "nor the active provider");
        check(m2.d.persisted.provider_keys.count("groq") == 1,
              "the key is saved — Groq is ready when you want it");
        check(m2.d.persisted.provider != "groq",
              "and the persisted active provider is untouched");
    }
}

TEST_CASE("first-run host adopt leaves provider and model agreeing") {
    // The reported half-switch: entering a local host changed the provider
    // chip but left the model chip empty, and dismissing the auto-opened
    // picker stranded you there. A switch that cannot name a model must not
    // commit the provider half on its own.
    fs::path tmp_xdg = fs::temp_directory_path() /
        ("agentty-halfswitch-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::create_directories(tmp_xdg, ec);
    ::setenv("AGENTTY_HOME", tmp_xdg.c_str(), 1);
    agentty::provider::set_custom_auth_header(::agentty::IoAccess::grant(), "");

    agentty::store::Settings s;          // nothing configured: first run
    install_stub_deps(s);

    agentty::Model m;
    m.d.persisted = s;
    m.d.auth.loaded = true;
    m.ui.login = login::HostProbing{.spec = "localhost:11434",
                                    .attempt_id = 1};

    agentty::HostProbed r;
    r.attempt_id = 1;
    r.spec = "localhost:11434";
    r.ok = true;
    r.model_count = 4;
    r.latency_ms = 10;
    auto [m2, cmd] = ::agentty::app::detail::step(
        ::agentty::app::detail::login_update, std::move(m),
        msg::LoginMsg{std::move(r)});

    // First run adopts, so the provider SHOULD have moved.
    check(m2.d.selection.kind == agentty::provider::Kind::OpenAI,
          "first run adopts the host");
    // The catalog has not landed yet, so model_id is deliberately empty --
    // carrying the outgoing provider's model would file it under the new
    // provider's recall. That is fine as a TRANSIENT, but it must resolve
    // the moment the catalog arrives, not wait for the user to pick.
    check(m2.d.model_id.empty(),
          "model is empty until the catalog lands (not the outgoing one)");
    check(m2.s.models_loading,
          "and the badge can say so: a fetch is in flight");

    // The catalog arrives. Even if the user dismissed the auto-opened
    // picker without choosing, this must fill the model in -- otherwise
    // the provider chip names a backend with no model behind it.
    agentty::ModelsLoaded loaded;
    loaded.provider_id = m2.d.selection.catalog_id();
    for (const char* id : {"qwen3:30b-a3b", "qwen3:8b"}) {
        agentty::ModelInfo mi;
        mi.id = agentty::ModelId{id};
        mi.display_name = id;
        mi.context_window = 32768;
        loaded.models.push_back(std::move(mi));
    }
    auto [m3, cmd3] = ::agentty::app::detail::step(
        ::agentty::app::detail::models_update, std::move(m2),
        msg::ModelsMsg{std::move(loaded)});

    check(!m3.d.model_id.empty(),
          "the catalog landing resolves the model without a user pick");
    check(!m3.s.models_loading, "and the fetch is no longer in flight");
}

TEST_CASE("a second local host does not adopt over the one in use") {
    // Guards the end-to-end behaviour rather than the predicate: a user
    // working against one local box adds another and keeps their session.
    // (The predicate's own edge -- a keyless host that is NOT active -- is
    // the "saved-but-unusable" case below, which is where the old
    // provider_keys.empty() test and this one actually disagree.)
    fs::path tmp_xdg = fs::temp_directory_path() /
        ("agentty-2ndhost-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::create_directories(tmp_xdg, ec);
    ::setenv("AGENTTY_HOME", tmp_xdg.c_str(), 1);
    agentty::provider::set_custom_auth_header(::agentty::IoAccess::grant(), "");

    agentty::store::Settings s;
    s.provider_keys["localhost:11434"] = "";   // keyless, already added
    install_stub_deps(s);

    agentty::Model m;
    m.d.persisted = s;
    m.d.auth.loaded = true;
    // ...and it is what the user is CURRENTLY working against.
    m.d.selection = agentty::provider::parse_selection("localhost:11434");
    m.d.model_id  = agentty::ModelId{"qwen3:8b"};
    const auto before_sel   = m.d.selection.catalog_id();
    const auto before_model = m.d.model_id;

    m.ui.login = login::HostProbing{.spec = "localhost:8080",
                                    .attempt_id = 2};
    agentty::HostProbed r;
    r.attempt_id = 2;
    r.spec = "localhost:8080";
    r.ok = true;
    r.model_count = 2;
    r.latency_ms = 8;
    auto [m2, cmd] = ::agentty::app::detail::step(
        ::agentty::app::detail::login_update, std::move(m),
        msg::LoginMsg{std::move(r)});

    check(m2.d.selection.catalog_id() == before_sel,
          "the host you are working on stays active");
    check(m2.d.model_id == before_model,
          "and your model is untouched");
    check(m2.d.persisted.provider_keys.count("localhost:8080") == 1,
          "the new host is still saved for later");
    check(m2.s.status.find("^P") != std::string::npos,
          "with the switch offered, not taken");
}

TEST_CASE("a saved-but-unusable host still counts as unconfigured") {
    // The case the two predicates actually disagree on, and the reason the
    // old one was wrong in BOTH directions. A keyless host is stored as an
    // empty-string value, so "provider_keys.empty()" called it configured --
    // even though nothing in the map can serve a turn and the active
    // provider has no credential either. Someone in that state adding a
    // real backend got no adopt and a dead session.
    fs::path tmp_xdg = fs::temp_directory_path() /
        ("agentty-unusable-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::create_directories(tmp_xdg, ec);
    ::setenv("AGENTTY_HOME", tmp_xdg.c_str(), 1);
    agentty::provider::set_custom_auth_header(::agentty::IoAccess::grant(), "");

    agentty::store::Settings s;
    // A keyless host was added once, but it is NOT what is selected, and
    // the selected provider (Anthropic) has no credential.
    s.provider_keys["localhost:11434"] = "";
    install_stub_deps(s);

    agentty::Model m;
    m.d.persisted = s;
    m.d.auth.loaded = true;          // loaded, and empty: nothing stored
    m.ui.login = login::HostProbing{.spec = "localhost:8080",
                                    .attempt_id = 3};
    agentty::HostProbed r;
    r.attempt_id = 3;
    r.spec = "localhost:8080";
    r.ok = true;
    r.model_count = 2;
    r.latency_ms = 7;
    auto [m2, cmd] = ::agentty::app::detail::step(
        ::agentty::app::detail::login_update, std::move(m),
        msg::LoginMsg{std::move(r)});

    check(m2.d.selection.catalog_id() == "localhost:8080",
          "nothing usable was configured, so the new host is adopted");
}

TEST_CASE("custom host key prompt transitions") {

    // Redirect XDG_CONFIG_HOME to a temp dir with no agentty/ subdir so
    // auth::load_credentials() returns nullopt (no credentials.json exists).
    // This keeps the non-TLS path deterministic regardless of the host env.
    fs::path tmp_xdg = fs::temp_directory_path() /
        ("agentty-test-" + std::to_string(::getpid()));
    std::error_code mkdir_ec;
    fs::create_directories(tmp_xdg, mkdir_ec);
    ::setenv("AGENTTY_HOME", tmp_xdg.c_str(), 1);
    // Ensure parse_selection's custom_auth_header() read is deterministic.
    agentty::provider::set_custom_auth_header(::agentty::IoAccess::grant(), "");

    // ── Case 1: TLS host, no saved key → probe it, no key prompt ──
    {
        agentty::store::Settings s;
        install_stub_deps(s);
        agentty::Model m;
        m.d.persisted = s;   // the record, as init() seeds it
        m.ui.login = login::CustomHostInput{.host_input = "https://chat.example.org/api"};
        auto [m2, cmd] = ::agentty::app::detail::step(::agentty::app::detail::login_update, std::move(m), msg::LoginMsg{agentty::LoginSubmit{}});
        auto* hp = std::get_if<login::HostProbing>(&m2.ui.login);
        check(hp != nullptr,
              "1: TLS host with no key is probed (a RunPod pod needs none)");
        check(hp && hp->spec == "https://chat.example.org/api",
              "1: probing the full URL spec");
        check(!m2.s.models_loading, "1: no commit while probing");

        // The server answers 401 → the key prompt opens, marked required.
        agentty::HostProbed needs;
        needs.attempt_id = hp ? hp->attempt_id : 0;
        needs.spec = "https://chat.example.org/api";
        needs.ok = false;
        needs.needs_key = true;
        needs.error = "HTTP 401";
        auto [m3, cmd3] = ::agentty::app::detail::step(::agentty::app::detail::login_update, std::move(m2), msg::LoginMsg{std::move(needs)});
        auto* api = std::get_if<login::ApiKeyInput>(&m3.ui.login);
        check(api != nullptr, "1: a 401 opens the key prompt");
        check(api && api->provider == "https://chat.example.org/api",
              "1: ApiKeyInput.provider is the full URL spec (persistence key)");
        check(api && api->key_required, "1: the key is marked required");

        // Enter on an empty required key stays on the prompt.
        auto [m4, cmd4] = ::agentty::app::detail::step(::agentty::app::detail::login_update, std::move(m3), msg::LoginMsg{agentty::LoginSubmit{}});
        check(std::holds_alternative<login::ApiKeyInput>(m4.ui.login),
              "1: empty submit on a required key stays on the prompt");
        check(!m4.s.models_loading, "1: still no commit");
        (void)cmd; (void)cmd3; (void)cmd4;
    }

    // ── Case 1b: optional key prompt, empty Enter → probe keyless ──
    {
        agentty::store::Settings s;
        install_stub_deps(s);
        agentty::Model m;
        m.d.persisted = s;
        m.ui.login = login::ApiKeyInput{
            .provider = "https://pod-8888.proxy.runpod.net/v1",
            .provider_label = "pod-8888.proxy.runpod.net"};
        auto [m2, cmd] = ::agentty::app::detail::step(::agentty::app::detail::login_update, std::move(m), msg::LoginMsg{agentty::LoginSubmit{}});
        auto* hp = std::get_if<login::HostProbing>(&m2.ui.login);
        check(hp != nullptr, "1b: empty key on a custom host probes keyless");
        check(hp && hp->spec == "https://pod-8888.proxy.runpod.net/v1",
              "1b: probing the host the prompt was for");
        (void)cmd;
    }

    // ── Case 2: TLS host, saved key exists → ApiKeyInput pre-filled ──
    {
        agentty::store::Settings s;
        s.provider_keys["https://chat.example.org/api"] = "sk-test-key";
        install_stub_deps(s);
        agentty::Model m;
        m.d.persisted = s;   // the record, as init() seeds it
        m.ui.login = login::CustomHostInput{.host_input = "https://chat.example.org/api"};
        auto [m2, cmd] = ::agentty::app::detail::step(::agentty::app::detail::login_update, std::move(m), msg::LoginMsg{agentty::LoginSubmit{}});
        auto* api = std::get_if<login::ApiKeyInput>(&m2.ui.login);
        check(api != nullptr,
              "2: TLS host with a saved key opens the key prompt to check it");
        check(api && api->key_input == "sk-test-key",
              "2: key_input pre-filled with the saved key (2a)");
        check(api && api->cursor == 11,
              "2: cursor at end of pre-filled key (sk-test-key = 11 chars)");
        check(!m2.s.models_loading,
              "2: no commit (waiting for key submit)");
        (void)cmd;
    }

    // ── Case 3: Non-TLS bare host:port → PROBE first (HostProbing), commit
    //    only on HostProbed{ok} ──
    {
        agentty::store::Settings s;
        install_stub_deps(s);
        agentty::Model m;
        m.d.persisted = s;   // the record, as init() seeds it
        m.d.available_models.push_back(agentty::ModelInfo{
            .id = agentty::ModelId{"old-model"}, .display_name = "old"});
        m.ui.login = login::CustomHostInput{.host_input = "localhost:8080"};
        auto [m2, cmd] = ::agentty::app::detail::step(::agentty::app::detail::login_update, std::move(m), msg::LoginMsg{agentty::LoginSubmit{}});
        auto* hp = std::get_if<login::HostProbing>(&m2.ui.login);
        check(hp != nullptr,
              "3: non-TLS host enters HostProbing (probe before commit)");
        check(hp && hp->spec == "localhost:8080",
              "3: probing the canonical spec");
        check(!m2.s.models_loading,
              "3: not committed while probing");
        // Successful probe result → commit (models_loading flips, modal closes).
        agentty::HostProbed ok;
        ok.attempt_id = hp->attempt_id;
        ok.spec = "localhost:8080";
        ok.ok = true;
        ok.models_path = "/v1/models";
        ok.model_count = 2;
        auto [m3, cmd3] = ::agentty::app::detail::step(::agentty::app::detail::login_update, std::move(m2), msg::LoginMsg{std::move(ok)});
        // The keyless-host save is an EFFECT; play the host and run it, or
        // the assertion below is checking a write nobody performed.
        {
            agtest::fx::Store store;
            store.settings = s;
            agtest::fx::run(cmd3, store);
            s = store.settings;
        }
        check(std::holds_alternative<login::Closed>(m3.ui.login),
              "3: probe success commits (modal closes)");
        check(m3.s.models_loading,
              "3: commit_provider_switch ran (models_loading true)");
        check(m3.d.available_models.empty(),
              "3: commit_provider_switch cleared available_models");
        // The keyless host was persisted as a saved row (empty key).
        check(s.provider_keys.count("localhost:8080") == 1,
              "3: keyless host persisted into provider_keys");
        (void)cmd; (void)cmd3;
    }

    // ── Case 4: probe FAILURE returns to the input with the spec restored ──
    {
        agentty::store::Settings s;
        install_stub_deps(s);
        agentty::Model m;
        m.d.persisted = s;   // the record, as init() seeds it
        m.ui.login = login::CustomHostInput{.host_input = "http://10.0.0.5:5000/custom"};
        auto [m1, cmd1] = ::agentty::app::detail::step(::agentty::app::detail::login_update, std::move(m), msg::LoginMsg{agentty::LoginSubmit{}});
        auto* hp = std::get_if<login::HostProbing>(&m1.ui.login);
        check(hp != nullptr, "4: http:// host enters HostProbing");
        agentty::HostProbed bad;
        bad.attempt_id = hp ? hp->attempt_id : 0;
        bad.spec = "http://10.0.0.5:5000/custom";
        bad.ok = false;
        bad.error = "nothing listening";
        auto [m2, cmd2] = ::agentty::app::detail::step(::agentty::app::detail::login_update, std::move(m1), msg::LoginMsg{std::move(bad)});
        auto* ch = std::get_if<login::CustomHostInput>(&m2.ui.login);
        check(ch != nullptr,
              "4: probe failure returns to the host input");
        check(ch && ch->host_input == "http://10.0.0.5:5000/custom",
              "4: typed spec restored for re-editing");
        check(!m2.s.models_loading, "4: no commit on failure");
        // A STALE probe result (wrong attempt id) is dropped.
        agentty::HostProbed stale;
        stale.attempt_id = 9999;
        stale.ok = true;
        auto [m3, cmd3] = ::agentty::app::detail::step(::agentty::app::detail::login_update, std::move(m2), msg::LoginMsg{std::move(stale)});
        check(std::holds_alternative<login::CustomHostInput>(m3.ui.login),
              "4: stale probe result is a no-op");
        (void)cmd1; (void)cmd2; (void)cmd3;
    }

    // ── Case 5: Esc at the key prompt → cancel the whole switch ──
    {
        agentty::store::Settings s;
        s.provider_keys["https://chat.example.org/api"] = "sk-test-key";
        install_stub_deps(s);
        agentty::Model m;
        m.d.persisted = s;   // the record, as init() seeds it
        // A host with a saved key lands in ApiKeyInput.
        m.ui.login = login::CustomHostInput{.host_input = "https://chat.example.org/api"};
        auto [m1, cmd1] = ::agentty::app::detail::step(::agentty::app::detail::login_update, std::move(m), msg::LoginMsg{agentty::LoginSubmit{}});
        check(std::holds_alternative<login::ApiKeyInput>(m1.ui.login),
              "5: setup — host with saved key entered ApiKeyInput");
        // Now press Esc (CloseLogin).
        auto [m2, cmd2] = ::agentty::app::detail::step(::agentty::app::detail::login_update, std::move(m1), msg::LoginMsg{agentty::CloseLogin{}});
        check(std::holds_alternative<login::Closed>(m2.ui.login),
              "5: Esc at key prompt closes the modal");
        check(!m2.s.models_loading,
              "5: no commit happened (models_loading false)");
        (void)cmd1; (void)cmd2;
    }

    // Cleanup the temp XDG dir.
    std::error_code ec;
    fs::remove_all(tmp_xdg, ec);
}
