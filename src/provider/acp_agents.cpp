// agentty::provider — external ACP agent registry (config surface).
// See acp_agents.hpp for the format + resolution rules.

#include "agentty/provider/acp_agents.hpp"
#include "agentty/io/persistence.hpp"   // write_json_atomic
#include "agentty/util/user_root.hpp"

#include <algorithm>
#include <maya/runtime.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>

#include <nlohmann/json.hpp>

namespace agentty::provider {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// External ACP agents are entirely config-driven (Zed's `agent_servers`
// model). ACP is the generic transport; agentty does not privilege or install
// a particular agent implementation. Users opt into any external agent by
// naming its command in acp-agents.json.

// Resolve the config file path + whether it's workspace-local (untrusted).
// Mirrors mcp::resolve_config: $AGENTTY_ACP_AGENTS > ~/.agentty > ./.agentty.
fs::path resolve_config(bool& out_project_local) {
    out_project_local = false;
    std::error_code ec;
    if (const char* e = std::getenv("AGENTTY_ACP_AGENTS"); e && e[0]) {
        fs::path p{e};
        return fs::is_regular_file(p, ec) ? p : fs::path{};
    }
    if (fs::path root = ::agentty::util::user_root(); !root.empty()) {
        fs::path p = root / "acp-agents.json";
        if (fs::is_regular_file(p, ec)) return p;
    }
    fs::path proj = fs::path{".agentty"} / "acp-agents.json";
    if (fs::is_regular_file(proj, ec)) { out_project_local = true; return proj; }
    return {};
}

[[nodiscard]] bool env_truthy(const char* name) noexcept {
    const char* e = std::getenv(name);
    return e && (e[0] == '1' || e[0] == 't' || e[0] == 'T' || e[0] == 'y' || e[0] == 'Y');
}

// Parse the whole config file into `out` keyed by id. Returns false (and leaves
// out untouched) on a missing / untrusted / malformed config — a built-in
// default can still satisfy the lookup.
bool load_config_uncached(std::vector<AcpAgentSpec>& out) {
    bool project_local = false;
    fs::path cfg = resolve_config(project_local);
    if (cfg.empty()) return false;

    // Untrusted-workspace spawn gate (same policy as .agentty/mcp.json): a
    // project-local config can ride in on a cloned repo and spawn arbitrary
    // commands, so require an explicit opt-in.
    if (project_local && !env_truthy("AGENTTY_ACP_ALLOW_PROJECT")) {
        std::fprintf(stderr,
            "acp: ignoring workspace-local %s (it can spawn arbitrary\n"
            "     commands). Set AGENTTY_ACP_ALLOW_PROJECT=1 to enable it, or\n"
            "     move trusted agents to ~/.agentty/acp-agents.json.\n",
            cfg.string().c_str());
        return false;
    }

    json doc;
    try {
        std::ifstream f(cfg);
        f >> doc;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "acp: failed to parse %s: %s\n",
                     cfg.string().c_str(), e.what());
        return false;
    }

    const json* agents = nullptr;
    if (doc.contains("acpAgents") && doc["acpAgents"].is_object())
        agents = &doc["acpAgents"];
    else if (doc.contains("agents") && doc["agents"].is_object())
        agents = &doc["agents"];
    if (!agents) return false;

    auto as_str = [](const json& v) {
        return v.is_string() ? v.get<std::string>() : v.dump();
    };
    for (auto it = agents->begin(); it != agents->end(); ++it) {
        const json& spec = it.value();
        AcpAgentSpec a;
        a.id      = it.key();
        a.command = spec.value("command", std::string{});
        if (a.command.empty()) {
            std::fprintf(stderr, "acp: agent '%s' has no \"command\" — skipping\n",
                         a.id.c_str());
            continue;
        }
        if (spec.contains("args") && spec["args"].is_array())
            for (const auto& x : spec["args"]) a.args.push_back(as_str(x));
        if (spec.contains("env") && spec["env"].is_object())
            for (auto e = spec["env"].begin(); e != spec["env"].end(); ++e)
                a.env.emplace_back(e.key(), as_str(e.value()));
        a.cwd = spec.value("cwd", std::string{});
        out.push_back(std::move(a));
    }
    return true;
}

// Cached front-door. The provider-picker VIEW calls enumerate_acp_agents()
// once per rendered frame, and is_acp_agent_id() sits on parse_selection's
// path — re-reading + JSON-parsing acp-agents.json each time is disk work
// per frame. Key the parsed vector on (resolved path, mtime, size): a stat
// is ~1µs and picks up edits, deletion, and env-var retargeting alike.
// Keyed on plain values (path string, mtime ticks). Read from view code, so
// a lock-free snapshot: no capability needed.
struct ConfigCache {
    std::vector<AcpAgentSpec> agents;
    bool                      ok = false;
    std::string               path;
    std::int64_t              mtime = 0;
    std::uintmax_t            size = 0;
};
maya::snapshot<ConfigCache>& config_cache() {
    static maya::snapshot<ConfigCache> c;
    return c;
}

bool load_config(std::vector<AcpAgentSpec>& out) {
    bool project_local = false;
    fs::path cfg = resolve_config(project_local);

    std::error_code ec;
    fs::file_time_type mtime{};
    std::uintmax_t size = 0;
    if (!cfg.empty()) {
        mtime = fs::last_write_time(cfg, ec);
        size  = ec ? 0 : fs::file_size(cfg, ec);
    }
    // Hit: copy the cached list out of the current snapshot.
    const std::string  key_path  = cfg.string();
    const std::int64_t key_mtime = mtime.time_since_epoch().count();
    std::optional<std::pair<bool, std::vector<AcpAgentSpec>>> hit;
    {
        const auto c = config_cache().load();
        if (c->path == key_path && c->mtime == key_mtime && c->size == size)
            hit = std::pair{c->ok, c->agents};
    }
    if (!hit) {
        // Miss: parse (disk + JSON), then publish.
        ConfigCache fresh;
        fresh.ok    = load_config_uncached(fresh.agents);
        fresh.path  = key_path;
        fresh.mtime = key_mtime;
        fresh.size  = size;
        hit = std::pair{fresh.ok, fresh.agents};
        config_cache().store(std::move(fresh));
    }
    if (!hit->first) return false;
    out.insert(out.end(), hit->second.begin(), hit->second.end());
    return true;
}

} // namespace

std::optional<AcpAgentSpec> resolve_acp_agent(std::string_view id) {
    std::vector<AcpAgentSpec> cfg;
    load_config(cfg);
    for (auto& a : cfg)
        if (a.id == id) return std::move(a);
    return std::nullopt;
}

bool is_acp_agent_id(std::string_view id) noexcept {
    std::vector<AcpAgentSpec> cfg;
    load_config(cfg);
    for (const auto& a : cfg)
        if (a.id == id) return true;
    return false;
}

std::vector<AcpAgentSpec> enumerate_acp_agents() {
    std::vector<AcpAgentSpec> out;
    load_config(out);
    return out;
}

namespace {

// Public CLIs that speak ACP on stdio, as their own docs launch them. Facts,
// not built-ins: nothing here runs until the user picks it and it's written
// to acp-agents.json. Ordered by how likely someone asking is to have it.
struct KnownAgent {
    const char* id;
    const char* command;
    std::vector<std::string> args;
    const char* label;
    const char* blurb;
};
const std::vector<KnownAgent>& known_agents() {
    static const std::vector<KnownAgent> k = {
        {"devin",            "devin",            {"acp"},   "Devin",       "Devin CLI over ACP (SWE-2 and more)"},
        {"claude-agent-acp", "claude-agent-acp", {},        "Claude Code", "Claude Code over ACP"},
        {"codex-acp",        "codex-acp",        {},        "Codex",       "Codex CLI over ACP"},
        {"gemini",           "gemini",           {"--acp"}, "Gemini CLI",  "Gemini CLI over ACP"},
        {"opencode",         "opencode",         {"acp"},   "OpenCode",    "OpenCode over ACP"},
        {"cursor-agent",     "cursor-agent",     {"acp"},   "Cursor",      "Cursor agent over ACP"},
    };
    return k;
}

// `name` on PATH as an executable file. Stats only.
bool on_path(std::string_view name, std::string_view path) {
#if defined(_WIN32)
    constexpr char sep = ';';
    static constexpr const char* exts[] = {".exe", ".cmd", ".bat", ""};
#else
    constexpr char sep = ':';
    static constexpr const char* exts[] = {""};
#endif
    std::error_code ec;
    while (!path.empty()) {
        const auto cut = path.find(sep);
        const std::string_view dir = path.substr(0, cut);
        path = cut == std::string_view::npos ? std::string_view{} : path.substr(cut + 1);
        if (dir.empty()) continue;
        for (const char* ext : exts) {
            fs::path p = fs::path{std::string{dir}} / (std::string{name} + ext);
            if (fs::is_regular_file(p, ec)) return true;
        }
    }
    return false;
}

struct DiscoverCache {
    std::string path_env;
    std::vector<std::string> installed;   // known ids found on PATH
    bool filled = false;
};
maya::snapshot<DiscoverCache>& discover_cache() {
    static maya::snapshot<DiscoverCache> c;
    return c;
}

} // namespace

std::vector<DiscoveredAcpAgent> discover_acp_agents() {
    // Off for hermetic runs (snapshots, tests): what is on this machine's
    // PATH must not change what a fixture renders.
    if (env_truthy("AGENTTY_NO_AGENT_DISCOVERY")) return {};
    const char* pe = std::getenv("PATH");
    const std::string path_env = pe ? pe : "";
    std::vector<std::string> installed;
    if (const auto c = discover_cache().load(); c->filled && c->path_env == path_env) {
        installed = c->installed;
    } else {
        for (const auto& k : known_agents())
            if (on_path(k.command, path_env)) installed.emplace_back(k.id);
        discover_cache().store(DiscoverCache{path_env, installed, true});
    }
    std::vector<AcpAgentSpec> configured;
    load_config(configured);
    std::vector<DiscoveredAcpAgent> out;
    for (const auto& k : known_agents()) {
        if (std::find(installed.begin(), installed.end(), k.id) == installed.end()) continue;
        // Already configured under its id, or the same command under any id.
        const bool have = std::any_of(configured.begin(), configured.end(),
            [&](const AcpAgentSpec& a) { return a.id == k.id || a.command == k.command; });
        if (have) continue;
        DiscoveredAcpAgent d;
        d.spec.id      = k.id;
        d.spec.command = k.command;
        d.spec.args    = k.args;
        d.label        = k.label;
        d.blurb        = k.blurb;
        out.push_back(std::move(d));
    }
    return out;
}

std::string add_acp_agent(Io io, const AcpAgentSpec& spec) {
    // Always the user-global file: a project-local config is the untrusted
    // one, and an explicit $AGENTTY_ACP_AGENTS is the user's to edit.
    fs::path root = ::agentty::util::user_root();
    if (root.empty()) return "no home directory to save the agent in";
    const fs::path file = root / "acp-agents.json";
    json doc = json::object();
    std::error_code ec;
    if (fs::is_regular_file(file, ec)) {
        try {
            std::ifstream f(file);
            f >> doc;
        } catch (const std::exception& e) {
            return std::string{"acp-agents.json isn't valid JSON ("} + e.what() + "); fix it first";
        }
        if (!doc.is_object()) return "acp-agents.json isn't a JSON object; fix it first";
    }
    const char* key = doc.contains("agents") && !doc.contains("acpAgents") ? "agents" : "acpAgents";
    if (!doc.contains(key) || !doc[key].is_object()) doc[key] = json::object();
    json entry = json::object();
    entry["command"] = spec.command;
    if (!spec.args.empty()) entry["args"] = spec.args;
    if (!spec.env.empty()) {
        json env = json::object();
        for (const auto& [k, v] : spec.env) env[k] = v;
        entry["env"] = std::move(env);
    }
    if (!spec.cwd.empty()) entry["cwd"] = spec.cwd;
    doc[key][spec.id] = std::move(entry);
    fs::create_directories(root, ec);
    if (!persistence::write_json_atomic(io, file, doc.dump(2) + "\n"))
        return "couldn't write " + file.string();
    return {};
}

} // namespace agentty::provider
