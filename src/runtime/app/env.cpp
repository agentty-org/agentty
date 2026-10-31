// src/runtime/app/env.cpp — read the launch environment, ONCE, at init.
//
// This is the only place in the app layer that calls getenv for a fact
// update() depends on. init() calls read_launch_env() and stores the result
// in Model::env; every reducer reads the Model. See the comment on Model::Env
// for why a reducer must never read the environment itself.

#include "agentty/runtime/app/env.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#include <maya/terminal/ansi.hpp>

#include "agentty/util/home_dir.hpp"
#include "agentty/util/user_root.hpp"
#include "agentty/util/update.hpp"   // self_update_possible
#include "agentty/rag/embed_backend.hpp"   // apply_env
#include "agentty/runtime/view/helpers.hpp"      // max_context_tokens_from_env
#include "agentty/runtime/view/host_escape.hpp"  // detect_integration

namespace agentty::app {

namespace {

bool set(const char* name) noexcept { return std::getenv(name) != nullptr; }

// A flag var is ON when present, non-empty, and not "0".
bool flag(const char* name) noexcept {
    const char* v = std::getenv(name);
    return v && v[0] && v[0] != '0';
}

// "1", "t"/"T", "y"/"Y" — the truthy spellings AGENTTY_FROZEN_COLLAPSE has
// always accepted. Kept exact so no existing setting changes meaning.
bool truthy(const char* name) noexcept {
    const char* v = std::getenv(name);
    if (!v || !*v) return false;
    return v[0] == '1' || v[0] == 't' || v[0] == 'T'
        || v[0] == 'y' || v[0] == 'Y';
}

// Unset → on; "0"/"false"/"off"/"no" (any case) → off; anything else → on.
bool on_unless_off(const char* name) noexcept {
    const char* v = std::getenv(name);
    if (!v || !*v) return true;
    std::string s;
    for (const char* p = v; *p; ++p)
        s += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
    return !(s == "0" || s == "false" || s == "off" || s == "no");
}

bool detect_remote() noexcept {
    // Order is the precedence, unchanged from the predicate this replaces:
    // an explicit force-on beats the opt-out, which beats detection.
    //
    // Explicit FORCE-ON: a session the env sniff can't see (a bespoke remote
    // wrapper, a serial console, a laggy container attach) opts into the
    // remote cadence.
    if (flag("AGENTTY_FORCE_REMOTE")) return true;
    // Escape hatch: a fast LAN SSH hop doesn't need throttling.
    if (flag("AGENTTY_NO_SSH_THROTTLE")) return false;
    // Direct SSH: sshd exports these into the remote shell.
    if (set("SSH_CONNECTION") || set("SSH_TTY") || set("SSH_CLIENT"))
        return true;
    // mosh: the SSH_* vars are NOT propagated through mosh-server (it
    // re-execs), but MOSH_* are — and mosh is always remote.
    if (set("MOSH_CONNECTION") || set("MOSH_KEY") || set("MOSH_SERVER_PID"))
        return true;
    // Eternal Terminal exports ET_VERSION into the remote shell.
    if (set("ET_VERSION")) return true;
    return false;
}

// How much history a width-change replay should print back: about what the
// terminal keeps, since anything past its scrollback limit is thrown away.
// Defaults per terminal (rows, bytes); AGENTTY_REPLAY_ROWS overrides the
// rows (0 turns replay off). Ghostty and cmux keep scrollback by BYTES
// (10 MB default), so their cap is in bytes.
void read_replay_caps(Model::Env& e) noexcept {
    auto eq = [](const char* name, const char* want) {
        const char* v = std::getenv(name);
        return v && std::string_view{v} == want;
    };
    int rows = 10'000;
    std::size_t bytes = 8u << 20;
    if (set("VSCODE_INJECTION") || eq("TERM_PROGRAM", "vscode")) {
        rows = 1'000;                      // xterm.js default scrollback
    } else if (set("GHOSTTY_RESOURCES_DIR") || eq("TERM_PROGRAM", "ghostty")
               || set("CMUX_SOCKET_PATH") || set("CMUX_WORKSPACE_ID")) {
        rows = 50'000; bytes = 9u << 20;    // 10 MB byte-limited scrollback
    } else if (set("WT_SESSION")) {
        rows = 9'001;                       // Windows Terminal default
    } else if (set("WEZTERM_EXECUTABLE") || eq("TERM_PROGRAM", "WezTerm")) {
        rows = 3'500;                       // WezTerm default
    } else if (set("KITTY_WINDOW_ID")) {
        rows = 2'000;                       // kitty default
    } else if (set("ALACRITTY_WINDOW_ID") || set("ALACRITTY_LOG")) {
        rows = 10'000;                      // Alacritty default
    } else if (eq("TERM_PROGRAM", "iTerm.app")) {
        rows = 10'000;
    } else if (eq("TERM_PROGRAM", "Apple_Terminal")) {
        rows = 10'000;
    }
    if (const char* v = std::getenv("AGENTTY_REPLAY_ROWS"); v && *v) {
        char* end = nullptr;
        const long n = std::strtol(v, &end, 10);
        if (end && *end == '\0' && n >= 0) rows = static_cast<int>(std::min<long>(n, 1'000'000));
    }
    e.replay_max_rows  = rows;
    e.replay_max_bytes = bytes;
}

}  // namespace

Model::Env read_launch_env(Io io) noexcept {
    Model::Env e;
    e.remote              = detect_remote();
    // Plain SSH only. The composer's clipboard paths key off this rather than
    // `remote`, because the question there is "does OSC 52 ride an SSH pty",
    // not "are frames slow" — they were always two separate checks.
    e.ssh                 = set("SSH_CONNECTION") || set("SSH_TTY");
    e.no_reveal_glide     = flag("AGENTTY_NO_REVEAL_GLIDE");
    e.synchronized_output = maya::ansi::env_supports_synchronized_output();
    e.frozen_collapse     = truthy("AGENTTY_FROZEN_COLLAPSE");
    e.no_update_check     = set("AGENTTY_NO_UPDATE_CHECK");
    e.no_auto_update      = set("AGENTTY_NO_AUTO_UPDATE");
    e.max_context_tokens  = ui::max_context_tokens_from_env();
    e.reveal              = on_unless_off("AGENTTY_REVEAL");
    e.reveal_typewriter   = on_unless_off("AGENTTY_REVEAL_TYPEWRITER");
    e.reveal_decorate     = on_unless_off("AGENTTY_REVEAL_DECORATE");
    e.painted_caret       = set("AGENTTY_PAINTED_CARET");
    e.host_integration    = ui::host::detect_integration();
    read_replay_caps(e);
    e.terminal            = ui_prefs::detect(/*tty=*/true);
    e.settings            = settings::registry::read_env(io);
    {
        std::error_code ec;
        const auto cwd = std::filesystem::current_path(ec);
        if (!ec) e.cwd = cwd.string();
        e.home = util::home_dir().string();
    }
    // Only probe when it could matter: the probe writes a file next to our
    // binary, and an install that opted out of auto-update shouldn't touch
    // its directory at all.
    if (!e.no_auto_update)
        e.self_update_ok = update::self_update_possible(e.self_update_reason);
    rag::embed::apply_env(io, e.embed_defaults);
    e.user_root = util::user_root().string();
    if (const char* v = std::getenv("AGENTTY_RAG_PROACTIVE"); v && v[0]) {
        std::string s{v};
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        e.rag_proactive_override = (s == "1" || s == "true" || s == "on" || s == "yes");
    }
    return e;
}

}  // namespace agentty::app
