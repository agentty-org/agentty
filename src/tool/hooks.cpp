// hooks.cpp — user-authored lifecycle hooks. See hooks.hpp for the design
// and the consent-gate rationale. Implementation notes:
//
//   • The payload is JSON on the hook's stdin, in Claude Code's shape, so a
//     hook written for Claude Code runs unchanged. It is also written to
//     AGENTTY_HOOK_PAYLOAD_FILE for hooks written before stdin existed.
//   • Hook commands run through run_shell_command_io — the SAME sandbox the
//     shell tool uses — so an approved hook is workspace-confined exactly
//     like model-driven shell. stdout and stderr come back apart: stdout is
//     the JSON reply, stderr the reason for exit 2.
//   • Approval store: content-hash trust via the SHARED scope::Approvals
//     primitive (the same one plugins use), persisted at
//     ~/.agentty/hooks_approved.json as a list of approved file hashes. Any
//     byte change to a hooks file yields a new hash → re-gated. A pre-scope
//     {path: hash} store is migrated transparently on first read.

#include "agentty/tool/hooks.hpp"
#include "agentty/util/user_root.hpp"
#include "agentty/config/inventory.hpp"   // kHooksApprovals — the one name
#include "agentty/util/capped_read.hpp"   // the one capped-read primitive

#include "agentty/auth/auth.hpp"            // auth::sha256_hex (file-content hash)
#include "agentty/scope/scope.hpp"          // scope::Approvals (shared trust store)
#include "agentty/io/persistence.hpp"       // random_hex_id
#include "agentty/io/shared_file.hpp"       // create_exclusive
#include "agentty/tool/util/sandbox.hpp"    // run_shell_command
#include "agentty/tool/util/subprocess.hpp"
#include "agentty/tool/util/utf8.hpp"        // strip_terminal_controls, to_valid_utf8
#include "agentty/util/logx.hpp"             // AGT_LOG: broken hooks

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <initializer_list>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>   // chmod
#include <unistd.h>     // getpid
#else
#include <process.h>    // _getpid
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace agentty::tools::hooks {

namespace {

constexpr std::size_t kMaxHooksFileBytes = 64 * 1024;
constexpr std::size_t kMaxPayloadBytes   = 4 * 1024 * 1024;
constexpr auto        kHookTimeout       = std::chrono::seconds{30};

[[nodiscard]] inline std::uint64_t current_pid() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(::_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

struct HookEntry {
    std::string match;   // matcher on the tool name (see name_matches)
    std::string run;     // shell command
    std::chrono::seconds timeout{kHookTimeout};
    // Claude Code's protocol (exit 2 blocks, JSON reply) vs agentty's own
    // (any non-zero exit blocks, stdout is the reason).
    bool cc = false;
};

struct HooksFile {
    std::string            path;      // absolute path of the file loaded
    std::string            hash;      // sha256 of the raw bytes
    std::vector<HookEntry> pre_tool;
    std::vector<HookEntry> post_tool;
    std::vector<HookEntry> post_tool_failure;
    bool                   ok = false;
    // Set when a hooks file EXISTS but is past the size cap, so nothing
    // could be loaded from it. Distinct from "no file": a user whose
    // blocking hook is silently inert deserves to be told which it is.
    std::string            too_big;   // the oversized path, else empty
};

[[nodiscard]] bool hooks_disabled() {
    const char* off = std::getenv("AGENTTY_NO_HOOKS");
    return off && off[0] && off[0] != '0';
}

// NOTE: the local read_all() is gone — see util/capped_read.hpp. It, and the
// two other copies in skills.cpp and commands.cpp, all shipped the same bug:
// "" meant missing AND too big, so `agentty hooks` printed "no hooks file"
// with the user's file right there and a blocking hook silently inert.

// Locate + parse the active hooks file. NOT cached across calls on
// purpose: hooks fire once per tool call (seconds apart), a stat+parse is
// noise there, and skipping the cache removes a whole class of staleness
// bugs (edit hooks.json → next tool call sees it, gated on approval).
[[nodiscard]] HooksFile load_hooks_file() {
    HooksFile out;
    const fs::path candidates[] = {
        fs::path{".agentty"} / "hooks.json",
        ::agentty::util::user_root().empty() ? fs::path{}
                                  : ::agentty::util::user_root() / "hooks.json",
    };
    for (const auto& c : candidates) {
        if (c.empty()) continue;
        const auto got = ::agentty::util::capped_read(c, kMaxHooksFileBytes);
        if (::agentty::util::oversize_bytes(got) > 0 && out.too_big.empty())
            out.too_big = c.string();
        const std::string& raw = ::agentty::util::bytes_or_empty(got);
        if (raw.empty()) continue;
        std::error_code ec;
        auto abs = fs::weakly_canonical(c, ec);
        out.path = (ec ? fs::absolute(c, ec) : abs).string();
        out.hash = auth::sha256_hex(raw);
        json j = json::parse(raw, nullptr, /*throw=*/false);
        if (!j.is_object()) return out;   // ok=false: unparseable
        auto read_list = [&](const char* key, std::vector<HookEntry>& dst) {
            if (!j.contains(key) || !j[key].is_array()) return;
            for (const auto& e : j[key]) {
                if (!e.is_object()) continue;
                HookEntry h;
                h.match = e.value("match", std::string{});
                h.run   = e.value("run", std::string{});
                if (e.contains("timeout") && e["timeout"].is_number())
                    h.timeout = std::chrono::seconds{std::clamp(e["timeout"].get<int>(), 1, 600)};
                if (!h.run.empty()) dst.push_back(std::move(h));
            }
        };
        read_list("pre_tool",  out.pre_tool);
        read_list("post_tool", out.post_tool);
        // Claude Code's shape: hooks.<Event>[] = {matcher, hooks[]: {type,
        // command, timeout}}. Only command hooks; other types are skipped.
        auto read_cc = [&](const char* event, std::vector<HookEntry>& dst) {
            const auto hooks = j.find("hooks");
            if (hooks == j.end() || !hooks->is_object()) return;
            const auto groups = hooks->find(event);
            if (groups == hooks->end() || !groups->is_array()) return;
            for (const auto& g : *groups) {
                if (!g.is_object()) continue;
                const std::string matcher = g.value("matcher", std::string{});
                const auto list = g.find("hooks");
                if (list == g.end() || !list->is_array()) continue;
                for (const auto& e : *list) {
                    if (!e.is_object() || e.value("type", std::string{"command"}) != "command")
                        continue;
                    HookEntry h;
                    h.cc    = true;
                    h.match = matcher;
                    h.run   = e.value("command", std::string{});
                    if (e.contains("timeout") && e["timeout"].is_number())
                        h.timeout = std::chrono::seconds{std::clamp(e["timeout"].get<int>(), 1, 600)};
                    if (!h.run.empty()) dst.push_back(std::move(h));
                }
            }
        };
        read_cc("PreToolUse",         out.pre_tool);
        read_cc("PostToolUse",        out.post_tool);
        read_cc("PostToolUseFailure", out.post_tool_failure);
        out.ok = true;
        return out;
    }
    return out;   // no file
}

// The pre-scope {path: hash} store, if one is still around. scope owns the
// current location (user root / state/); this only has to find an old file
// to migrate, so it looks in both.
[[nodiscard]] fs::path legacy_approvals_path() {
    auto root = ::agentty::util::user_root();
    if (root.empty()) return {};
    const fs::path leaf{::agentty::config::kHooksApprovals};
    std::error_code ec;
    const fs::path in_state = ::agentty::util::user_state_dir() / leaf;
    if (fs::is_regular_file(in_state, ec)) return in_state;
    return root / leaf;
}

// Hooks trust now rides on the SHARED content-hash primitive
// (scope::Approvals) instead of a bespoke store — one trust mechanism for
// every executable-from-untrusted-origin surface (plugins, hooks). The hash
// is the same value either way (scope::content_hash == auth::sha256_hex, and
// hf.hash is already sha256_hex(raw)), so nothing about the security property
// changes: approve a file's exact bytes; any byte change yields a new hash
// that isn't in the approved set, so it re-gates.
//
// The store leaf stays "hooks_approved.json" for continuity. scope::Approvals
// is a flat hash LIST; the pre-scope format was a {abs_path: hash} OBJECT.
// load_hook_approvals() reads either — an object is migrated in place by
// taking its hash VALUES — so nobody loses an approval across the upgrade.
constexpr std::string_view kHooksApprovalsLeaf =
    ::agentty::config::kHooksApprovals;

[[nodiscard]] scope::Approvals load_hook_approvals() {
    // Fast path: the scope loader handles the new array form (and returns
    // empty on the legacy object form, which it can't parse as an array).
    scope::Approvals a = scope::load_approvals(kHooksApprovalsLeaf);
    if (!a.shas.empty()) return a;
    // Legacy {path: hash} object — migrate its values into the list.
    const auto p = legacy_approvals_path();
    std::string raw = std::string{::agentty::util::bytes_or_empty(
        ::agentty::util::capped_read(p, kMaxHooksFileBytes))};
    if (raw.empty()) return a;
    json j = json::parse(raw, nullptr, /*throw=*/false);
    if (j.is_object())
        for (auto it = j.begin(); it != j.end(); ++it)
            if (it->is_string()) a.approve(it->get<std::string>());
    return a;
}

[[nodiscard]] bool is_approved(const HooksFile& hf) {
    if (hf.path.empty() || hf.hash.empty()) return false;
    return load_hook_approvals().approved(hf.hash);
}

void store_approval(Io io, const maya::unlocked& cap, const HooksFile& hf) {
    if (hf.hash.empty()) return;
    scope::Approvals a = load_hook_approvals();   // includes any migrated legacy hashes
    a.approve(hf.hash);
    (void)scope::save_approvals(io, cap, kHooksApprovalsLeaf, a);   // rewrites as the array form
}

// ── matching ────────────────────────────────────────────────────────────
//
// Claude Code's matcher rules, so its configs mean the same thing here:
// empty or "*" matches all; only letters, digits, _ - space , | is an exact
// name or a list of them; anything else is an unanchored regex. A tool
// matches under its agentty name or its Claude Code name, so "Bash" and
// "shell" both pick the shell tool.
[[nodiscard]] bool one_matches(const std::string& m, std::string_view name) {
    if (m.empty() || m == "*") return true;
    const bool plain = std::all_of(m.begin(), m.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'
            || c == ' ' || c == ',' || c == '|';
    });
    if (plain) {
        std::size_t p = 0;
        while (p <= m.size()) {
            auto q = m.find_first_of(",|", p);
            if (q == std::string::npos) q = m.size();
            auto tok = std::string_view{m}.substr(p, q - p);
            while (!tok.empty() && tok.front() == ' ') tok.remove_prefix(1);
            while (!tok.empty() && tok.back() == ' ') tok.remove_suffix(1);
            if (!tok.empty() && tok == name) return true;
            p = q + 1;
        }
        return false;
    }
    try {
        std::regex re(m, std::regex::ECMAScript | std::regex::nosubs);
        return std::regex_search(std::string{name}, re);
    } catch (const std::regex_error&) {
        return false;   // a bad pattern never matches
    }
}

[[nodiscard]] bool name_matches(const std::string& m, std::string_view tool) {
    return one_matches(m, tool) || one_matches(m, cc_tool_name(tool));
}

// agentty's own `match`: a POSIX ERE on agentty's tool name, unchanged.
[[nodiscard]] bool legacy_matches(const std::string& ere, std::string_view tool) {
    if (ere.empty()) return true;
    try {
        std::regex re(ere, std::regex::extended | std::regex::nosubs);
        return std::regex_search(std::string{tool}, re);
    } catch (const std::regex_error&) {
        return false;   // a bad pattern never matches
    }
}

[[nodiscard]] bool entry_matches(const HookEntry& h, std::string_view tool) {
    return h.cc ? name_matches(h.match, tool) : legacy_matches(h.match, tool);
}

// Write the payload to a private temp file; returns its path ('' on error).
// The temp dir is shared with other users, so the file is created whole at
// 0600 and never over an existing name: nobody can read it early or plant a
// symlink where it will be written.
[[nodiscard]] std::string write_payload(const std::string& payload) {
    std::error_code ec;
    auto dir = fs::temp_directory_path(ec);
    if (ec) return {};
    const auto base = "agentty_hook_" + std::to_string(current_pid()) + "_"
                    + persistence::random_hex_id();
    for (int attempt = 0; attempt < 8; ++attempt) {
        auto p = dir / (base + "_" + std::to_string(attempt));
        if (persistence::detail::create_in_section(p.string(), payload, 0600)) return p.string();
    }
    return {};
}

// Prefix `cmd` with variable assignments the shell applies to that command
// only. POSIX: `export A='v' B='v'; <cmd>` (exported so a hook that is a
// script sees them, and single-quoted so nothing in a value is interpreted).
// Windows cmd.exe: `set "A=v" && set "B=v" && call <cmd>`; the values here
// are an event name, a tool name and a temp path, none of which carry `"`
// or `%`. The `call` is load-bearing: cmd expands %VAR% when it PARSES the
// line, before any `set` on it has run, so a hook written as
// `findstr x "%AGENTTY_HOOK_PAYLOAD_FILE%"` saw the literal text. `call`
// re-parses its argument after the sets, which is when the vars exist.
[[nodiscard]] std::string with_hook_env(
    const std::string& cmd,
    std::initializer_list<std::pair<const char*, std::string>> vars) {
    std::string out;
#ifndef _WIN32
    out = "export";
    for (const auto& [k, v] : vars) {
        out += ' ';
        out += k;
        out += "='";
        for (char c : v) {
            if (c == '\'') out += "'\\''";
            else out += c;
        }
        out += '\'';
    }
    out += "; ";
#else
    for (const auto& [k, v] : vars) {
        out += "set \"";
        out += k;
        out += '=';
        out += v;
        out += "\" && ";
    }
    out += "call ";
#endif
    out += cmd;
    return out;
}

// Run one hook: payload on stdin and in AGENTTY_HOOK_PAYLOAD_FILE, stdout
// and stderr kept apart.
struct HookRun { int exit_code = 0; bool timed_out = false; std::string out, err; };
[[nodiscard]] HookRun run_hook(const HookEntry& h, std::string_view event,
                               std::string_view tool, const std::string& payload) {
    const std::string pf = write_payload(payload);
    // The hook's context goes in ITS command line, not the process env:
    // setenv/unsetenv mutate state every thread shares, so concurrent tools
    // needed a global lock around the whole run. A prefix is per-process.
    const std::string cmd = with_hook_env(h.run, {
        {"AGENTTY_HOOK_EVENT",        std::string{event}},
        {"AGENTTY_HOOK_TOOL",         std::string{tool}},
        {"AGENTTY_HOOK_PAYLOAD_FILE", pf},
        {"AGENTTY_PROJECT_DIR",       fs::current_path().string()},
        {"CLAUDE_PROJECT_DIR",        fs::current_path().string()},
    });
    auto res = util::sandbox::run_shell_command_io(cmd, payload, kMaxPayloadBytes, h.timeout);
    if (!pf.empty()) {
        std::error_code ec;
        fs::remove(pf, ec);
    }
    HookRun r;
    r.exit_code = res.started ? res.exit_code : 1;
    r.timed_out = res.timed_out;
    r.out = std::move(res.output);
    r.err = res.started ? std::move(res.err) : res.start_error;
    return r;
}

// Text from a hook that the model or the user will read: no terminal
// controls, valid UTF-8, and capped (Claude Code caps at 10,000 chars too).
[[nodiscard]] std::string clean_text(std::string s, std::size_t cap = 10'000) {
    s = util::to_valid_utf8(util::strip_terminal_controls(s));
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    if (s.size() > cap) {
        s.resize(cap);
        s = util::to_valid_utf8(std::move(s));
        s += "\n[hook output truncated]";
    }
    return s;
}

// The JSON object a hook printed, if stdout is one. Leading noise (a shell
// profile echoing something) defeats it, like in Claude Code.
[[nodiscard]] std::optional<json> reply_json(const std::string& out) {
    std::string_view s{out};
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    if (s.empty() || s.front() != '{') return std::nullopt;
    try {
        auto j = json::parse(s);
        if (j.is_object()) return j;
    } catch (const json::exception&) {}
    return std::nullopt;
}

// The fields every event shares.
[[nodiscard]] json base_payload(std::string_view event, std::string_view tool,
                                const json& args, std::string_view tool_use_id,
                                std::string_view session_id) {
    json p = {
        {"hook_event_name", std::string{event}},
        {"session_id",      std::string{session_id}},
        {"cwd",             fs::current_path().string()},
        {"permission_mode", "default"},
        {"tool_name",       cc_tool_name(tool)},
        {"tool_input",      to_cc_input(tool, args)},
        // agentty's own names, for hooks written against agentty.
        {"agentty_tool",    std::string{tool}},
        {"event",           event == "PreToolUse" ? "pre_tool" : "post_tool"},
        {"tool",            std::string{tool}},
        {"args",            args.dump(-1, ' ', false, json::error_handler_t::replace)},
    };
    if (!tool_use_id.empty()) p["tool_use_id"] = std::string{tool_use_id};
    return p;
}

[[nodiscard]] std::string dump(const json& j) {
    // replace: args and results can carry raw bytes, and dump() throws on
    // invalid UTF-8 by default, a crash inside the tool path.
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

// continue:false / systemMessage, shared by every event.
template <class D>
void read_common(const json& j, D& d) {
    if (j.contains("continue") && j["continue"].is_boolean() && !j["continue"].get<bool>()) {
        d.stop = true;
        if (auto it = j.find("stopReason"); it != j.end() && it->is_string())
            d.stop_reason = clean_text(it->get<std::string>(), 2000);
    }
    if (auto it = j.find("systemMessage"); it != j.end() && it->is_string())
        d.system_message = clean_text(it->get<std::string>(), 2000);
}

void append_context(std::string& dst, std::string add) {
    add = clean_text(std::move(add));
    if (add.empty()) return;
    if (!dst.empty()) dst += "\n";
    dst += add;
}

// The shell tool wraps output for the model: a ``` fence, a "Command ...
// failed" header, [elapsed] notes. Claude Code's Bash hands hooks the plain
// stdout, and output filters (RTK) expect that, so unwrap it.
[[nodiscard]] std::string shell_stdout(std::string_view s) {
    const auto open = s.find("```");
    if (open == std::string_view::npos) return std::string{s};
    auto body = s.find('\n', open);
    if (body == std::string_view::npos) return std::string{s};
    ++body;
    const auto close = s.rfind("```");
    if (close == std::string_view::npos || close < body) return std::string{s.substr(body)};
    return std::string{s.substr(body, close - body)};
}

} // namespace

// ── Claude Code names ───────────────────────────────────────────────────

std::string cc_tool_name(std::string_view tool) {
    static constexpr std::pair<std::string_view, std::string_view> kNames[] = {
        {"shell", "Bash"}, {"bash", "Bash"}, {"read", "Read"}, {"write", "Write"},
        {"edit", "Edit"}, {"grep", "Grep"}, {"glob", "Glob"}, {"web_fetch", "WebFetch"},
        {"web_search", "WebSearch"}, {"todo", "TodoWrite"}, {"task", "Task"},
        {"list_dir", "LS"},
    };
    for (const auto& [a, c] : kNames)
        if (a == tool) return std::string{c};
    return std::string{tool};
}

json to_cc_input(std::string_view tool, const json& args) {
    if (!args.is_object()) return args;
    json in = args;
    auto rekey = [&](const char* from, const char* to) {
        if (auto it = in.find(from); it != in.end() && !in.contains(to)) {
            in[to] = *it;
            in.erase(from);
        }
    };
    const auto name = cc_tool_name(tool);
    if (name == "Bash") {
        // Claude Code's Bash has `description`; agentty's shell has
        // display_description. Timeout is ms there, seconds here.
        rekey("display_description", "description");
        if (auto it = in.find("timeout"); it != in.end() && it->is_number())
            in["timeout"] = static_cast<std::int64_t>(it->get<double>() * 1000);
    } else if (name == "Read" || name == "Write") {
        rekey("path", "file_path");
    } else if (name == "Edit") {
        rekey("path", "file_path");
        // One edit is Claude Code's shape; agentty batches them.
        if (auto it = in.find("edits"); it != in.end() && it->is_array() && it->size() == 1) {
            const auto& e = (*it)[0];
            in["old_string"]  = e.value("old_text", e.value("old_string", std::string{}));
            in["new_string"]  = e.value("new_text", e.value("new_string", std::string{}));
            in["replace_all"] = e.value("replace_all", false);
        }
    }
    return in;
}

json from_cc_input(std::string_view tool, const json& in, const json& original) {
    if (!in.is_object()) return original;
    json out = in;
    auto rekey = [&](const char* from, const char* to) {
        if (auto it = out.find(from); it != out.end()) {
            out[to] = *it;
            out.erase(from);
        }
    };
    const auto name = cc_tool_name(tool);
    if (name == "Bash") {
        rekey("description", "display_description");
        if (auto it = out.find("timeout"); it != out.end() && it->is_number())
            out["timeout"] = std::max<std::int64_t>(1, static_cast<std::int64_t>(it->get<double>() / 1000));
        // Claude Code-only fields agentty's shell would reject.
        out.erase("run_in_background");
        out.erase("dangerouslyDisableSandbox");
    } else if (name == "Read" || name == "Write") {
        rekey("file_path", "path");
    } else if (name == "Edit") {
        rekey("file_path", "path");
        if (out.contains("old_string") && out.contains("edits") && out["edits"].is_array()
            && out["edits"].size() == 1) {
            auto& e = out["edits"][0];
            e["old_text"]    = out["old_string"];
            e["new_text"]    = out.value("new_string", std::string{});
            e["replace_all"] = out.value("replace_all", false);
            e.erase("old_string");
            e.erase("new_string");
        }
        out.erase("old_string");
        out.erase("new_string");
        out.erase("replace_all");
    }
    return out;
}

// ── events ──────────────────────────────────────────────────────────────

// A matcher string from pre_tool_matchers: "ere:<pattern>" for agentty's own
// entries, the Claude Code matcher otherwise.
bool pre_tool_matches(const std::vector<std::string>& matchers, std::string_view tool) {
    return std::any_of(matchers.begin(), matchers.end(), [&](const std::string& m) {
        return m.starts_with("ere:") ? legacy_matches(m.substr(4), tool) : name_matches(m, tool);
    });
}

std::vector<std::string> pre_tool_matchers(Io) {
    if (hooks_disabled()) return {};
    HooksFile hf = load_hooks_file();
    if (!hf.ok || !is_approved(hf)) return {};
    std::vector<std::string> out;
    for (const auto& h : hf.pre_tool)
        out.push_back(h.cc ? (h.match.empty() ? std::string{"*"} : h.match) : "ere:" + h.match);
    return out;
}

PreToolDecision run_pre_tool(std::string_view tool, const json& args,
                             std::string_view tool_use_id, std::string_view session_id) {
    using P = PreToolDecision::Permission;
    PreToolDecision d;
    if (hooks_disabled()) return d;
    HooksFile hf = load_hooks_file();
    if (!hf.ok || hf.pre_tool.empty()) return d;
    if (!is_approved(hf)) return d;   // unapproved file NEVER runs

    json current = args;   // each hook sees what the one before it left
    auto rank = [](P p) { return p == P::Deny ? 3 : p == P::Ask ? 2 : p == P::Allow ? 1 : 0; };
    auto decide = [&](P p, std::string reason) {
        if (rank(p) > rank(d.permission)) { d.permission = p; d.reason = std::move(reason); }
    };
    for (const auto& h : hf.pre_tool) {
        if (!entry_matches(h, tool)) continue;
        HookRun r = run_hook(h, "pre_tool", tool,
                             dump(base_payload("PreToolUse", tool, current, tool_use_id, session_id)));
        const auto reply = r.exit_code == 0 ? reply_json(r.out) : std::nullopt;

        if (!h.cc) {
            // agentty's own protocol: any non-zero exit blocks, stdout is
            // the reason. A JSON reply on exit 0 is read like Claude Code's.
            if (r.exit_code != 0) {
                std::string why = clean_text(r.out.empty() ? r.err : r.out, 4096);
                if (why.empty())
                    why = "blocked by pre_tool hook (`" + h.run + "` exited "
                        + std::to_string(r.exit_code) + ")";
                decide(P::Deny, std::move(why));
                return d;
            }
        } else if (r.exit_code == 2) {
            std::string why = clean_text(r.err);
            if (why.empty()) why = "blocked by PreToolUse hook (`" + h.run + "`)";
            decide(P::Deny, std::move(why));
            return d;
        } else if (r.exit_code != 0) {
            // A broken hook doesn't stop the call. Claude Code shows it as a
            // non-blocking error; here it lands in the log.
            AGT_LOG(Tool, Warn, "hook.error", "event=PreToolUse cmd={} exit={} timed_out={} err={}",
                    h.run, r.exit_code, r.timed_out, clean_text(r.err, 500));
            continue;
        }
        if (!reply) continue;

        read_common(*reply, d);
        const json* hso = nullptr;
        if (auto it = reply->find("hookSpecificOutput"); it != reply->end() && it->is_object())
            hso = &*it;
        // The deprecated top-level shape: decision approve|block + reason.
        if (auto it = reply->find("decision"); it != reply->end() && it->is_string()) {
            const auto v = it->get<std::string>();
            const std::string why = clean_text(reply->value("reason", std::string{}));
            if (v == "block") decide(P::Deny, why);
            else if (v == "approve") decide(P::Allow, why);
        }
        if (hso) {
            if (auto it = hso->find("permissionDecision"); it != hso->end() && it->is_string()) {
                const auto v = it->get<std::string>();
                const std::string why = clean_text(hso->value("permissionDecisionReason", std::string{}));
                if (v == "deny") decide(P::Deny, why.empty() ? "blocked by PreToolUse hook" : why);
                else if (v == "ask") decide(P::Ask, why);
                else if (v == "allow") decide(P::Allow, why);
            }
            if (auto it = hso->find("updatedInput"); it != hso->end() && it->is_object()) {
                current = from_cc_input(tool, *it, current);
                d.updated_args = current;
            }
            if (auto it = hso->find("additionalContext"); it != hso->end() && it->is_string())
                append_context(d.additional_context, it->get<std::string>());
        }
        if (d.permission == P::Deny || d.stop) return d;
    }
    return d;
}

PostToolDecision run_post_tool(std::string_view tool, const json& args,
                               const std::string& result_text, bool failed,
                               std::string_view tool_use_id, std::string_view session_id,
                               std::int64_t duration_ms) {
    PostToolDecision d;
    if (hooks_disabled()) return d;
    HooksFile hf = load_hooks_file();
    if (!hf.ok || !is_approved(hf)) return d;
    // agentty's own post_tool hooks fire on every result; Claude Code's
    // PostToolUse only on success, PostToolUseFailure only on failure.
    std::vector<const HookEntry*> todo;
    for (const auto& h : hf.post_tool)
        if (!h.cc || !failed) todo.push_back(&h);
    if (failed)
        for (const auto& h : hf.post_tool_failure) todo.push_back(&h);
    if (todo.empty()) return d;

    const std::string_view event = failed ? "PostToolUseFailure" : "PostToolUse";
    std::string current = result_text;
    for (const auto* h : todo) {
        if (!entry_matches(*h, tool)) continue;
        const std::string shown = current.size() > kMaxPayloadBytes
                                      ? current.substr(0, kMaxPayloadBytes) : current;
        json p = base_payload(h->cc ? event : "PostToolUse", tool, args, tool_use_id, session_id);
        p["event"]  = "post_tool";
        p["result"] = shown;   // agentty's own name
        if (failed) {
            p["error"] = shown;
        } else if (cc_tool_name(tool) == "Bash") {
            p["tool_response"] = {{"stdout", shell_stdout(shown)}, {"stderr", ""},
                                  {"interrupted", false}, {"isImage", false}};
        } else {
            p["tool_response"] = shown;
        }
        if (duration_ms >= 0) p["duration_ms"] = duration_ms;
        HookRun r = run_hook(*h, "post_tool", tool, dump(p));

        if (h->cc && r.exit_code == 2) {
            // The tool already ran; exit 2 shows stderr to the model.
            append_context(d.additional_context, r.err);
            continue;
        }
        if (r.exit_code != 0) {
            AGT_LOG(Tool, Warn, "hook.error", "event={} cmd={} exit={} timed_out={} err={}",
                    event, h->run, r.exit_code, r.timed_out, clean_text(r.err, 500));
            continue;
        }
        const auto reply = reply_json(r.out);
        if (!reply) continue;
        read_common(*reply, d);
        if (reply->value("decision", std::string{}) == "block")
            append_context(d.additional_context, reply->value("reason", std::string{}));
        if (auto hso = reply->find("hookSpecificOutput"); hso != reply->end() && hso->is_object()) {
            if (auto it = hso->find("additionalContext"); it != hso->end() && it->is_string())
                append_context(d.additional_context, it->get<std::string>());
            // updatedToolOutput: Bash's shape is {stdout, stderr}; anything
            // else is taken as text. Never applied to a failure.
            const json* upd = nullptr;
            if (auto it = hso->find("updatedToolOutput"); it != hso->end()) upd = &*it;
            else if (auto it2 = hso->find("updatedMCPToolOutput"); it2 != hso->end()) upd = &*it2;
            if (upd && !failed) {
                std::string text;
                if (upd->is_string()) text = upd->get<std::string>();
                else if (upd->is_object() && (upd->contains("stdout") || upd->contains("stderr"))) {
                    text = upd->value("stdout", std::string{});
                    const auto e = upd->value("stderr", std::string{});
                    if (!e.empty()) text += (text.empty() ? "" : "\n") + e;
                } else if (upd->is_object() && upd->contains("content") && (*upd)["content"].is_array()) {
                    for (const auto& b : (*upd)["content"])
                        if (b.is_object() && b.value("type", "") == "text")
                            text += b.value("text", std::string{});
                } else {
                    text = upd->dump();
                }
                current = util::to_valid_utf8(std::move(text));
                d.updated_output = current;
            }
        }
        if (d.stop) break;
    }
    return d;
}


bool pending_approval(Io) {
    if (hooks_disabled()) return false;
    HooksFile hf = load_hooks_file();
    return hf.ok && !is_approved(hf);
}

std::string active_file(Io) {
    return load_hooks_file().path;
}

int cli(Io io, const maya::unlocked& cap, const std::string& verb) {
    HooksFile hf = load_hooks_file();
    if (verb == "list" || verb.empty()) {
        if (hf.path.empty()) {
            // "No file" and "a file too big to load" are different answers,
            // and printing the first for the second is how a blocking hook
            // stays silently inert while the user believes it is armed.
            if (!hf.too_big.empty()) {
                std::printf("%s is over the %zu KB limit — NOT loaded,"
                            " so no hooks are running\n",
                            hf.too_big.c_str(), kMaxHooksFileBytes / 1024);
                return 1;
            }
            std::printf("no hooks file (.agentty/hooks.json or "
                        "~/.agentty/hooks.json)\n");
            return 0;
        }
        std::printf("hooks file: %s\n", hf.path.c_str());
        if (!hf.ok) {
            std::printf("  PARSE ERROR — file is not valid JSON\n");
            return 1;
        }
        std::printf("  approval:  %s\n",
                    is_approved(hf) ? "APPROVED"
                                    : "NOT APPROVED (hooks will not run — "
                                      "`agentty hooks approve`)");
        auto show = [](const char* ev, const char* cc_ev, const std::vector<HookEntry>& v) {
            for (const auto& h : v)
                std::printf("  %-19s match=%-20s run=%s\n", h.cc ? cc_ev : ev,
                            h.match.empty() ? "(all)" : h.match.c_str(),
                            h.run.c_str());
        };
        show("pre_tool",  "PreToolUse",  hf.pre_tool);
        show("post_tool", "PostToolUse", hf.post_tool);
        show("post_tool", "PostToolUseFailure", hf.post_tool_failure);
        return 0;
    }
    if (verb == "approve") {
        if (hf.path.empty()) {
            if (!hf.too_big.empty()) {
                std::fprintf(stderr, "agentty hooks approve: %s is over the "
                                     "%zu KB limit — nothing to approve\n",
                             hf.too_big.c_str(), kMaxHooksFileBytes / 1024);
                return 1;
            }
            std::fprintf(stderr, "agentty hooks approve: no hooks file found\n");
            return 1;
        }
        if (!hf.ok) {
            std::fprintf(stderr, "agentty hooks approve: %s is not valid "
                                 "JSON — fix it first\n", hf.path.c_str());
            return 1;
        }
        if (is_approved(hf)) {
            std::printf("already approved: %s\n", hf.path.c_str());
            return 0;
        }
        // Show the exact bytes being approved, then require explicit y.
        std::printf("── %s ──\n%s\n────────\n"
                    "These hooks run ARBITRARY SHELL automatically on "
                    "matching tool calls\n(sandboxed like the bash tool, "
                    "but still able to act inside your workspace).\n"
                    "Approve this exact file content? [y/N] ",
                    hf.path.c_str(),
                    std::string{::agentty::util::bytes_or_empty(
                        ::agentty::util::capped_read(
                            hf.path, kMaxHooksFileBytes))}.c_str());
        std::fflush(stdout);
        std::string line;
        std::getline(std::cin, line);
        if (line != "y" && line != "Y" && line != "yes") {
            std::printf("not approved\n");
            return 1;
        }
        store_approval(io, cap, hf);
        std::printf("approved (any byte change re-gates)\n");
        return 0;
    }
    std::fprintf(stderr, "usage: agentty hooks [list|approve]\n");
    return 2;
}

} // namespace agentty::tools::hooks
