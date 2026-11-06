// settings_roundtrip_test -- every user-visible setting survives a save and a
// reload, through the REAL settings.json path.
//
// The report was "settings don't seem to apply and persist, like scrollback".
// The appearance reducer, the form projection and the field mapping were all
// individually fine on inspection, which is exactly why this is a test and not
// a reading: a setting can be correct in memory and still fail to come back,
// and the only way to know is to write it to disk and read it back.
//
// Each case moves a field OFF its default (a default is not written at all, so
// round-tripping the default proves nothing), saves, wipes in-process state,
// reloads, and compares.

#include "agtest.hpp"

#include "agentty/domain/ui_prefs.hpp"
#include "agentty/io/persistence.hpp"
#include "agentty/store/store.hpp"
#include "agentty/util/io.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
namespace up = agentty::ui_prefs;
namespace persistence = agentty::persistence;

namespace {

// A private AGENTTY_HOME per case, so a stale settings.json from another test
// or from the developer's own install can never satisfy an assertion.
struct TmpHome {
    fs::path dir;
    explicit TmpHome(const char* tag) {
        dir = fs::temp_directory_path()
            / (std::string{"agentty-settings-rt-"} + tag + "-"
               + std::to_string(::getpid()));
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        ::setenv("AGENTTY_HOME", dir.c_str(), 1);
    }
    ~TmpHome() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

agentty::store::Settings save_and_reload(const agentty::store::Settings& s) {
    const auto io = ::agentty::IoAccess::grant();
    maya::thread_root([&](const maya::unlocked& c) {
        persistence::save_settings(io, c, s);
    });
    return persistence::load_settings(io);
}

}  // namespace

TEST_CASE("settings round-trip: every appearance pref survives a reload") {
    TmpHome home{"ui"};

    agentty::store::Settings s;
    // Every field moved OFF its default. A default is deliberately omitted
    // from settings.json, so a value equal to it round-trips trivially and
    // would prove nothing about the field it is supposed to cover.
    s.ui.theme            = "nord";
    s.ui.lang             = "de";
    s.ui.tier             = up::ColorTier::Ansi256;
    s.ui.polarity         = up::Polarity::Light;
    s.ui.density          = up::Density::Roomy;
    s.ui.prose_width      = 88;
    s.ui.compact_turns    = true;
    s.ui.thread_preview   = false;
    s.ui.foreign_sessions = false;
    s.ui.motion           = up::Motion::Reduced;
    s.ui.scrollback       = up::Scrollback::Small;
    s.ui.syntax           = false;
    s.ui.tool_output      = up::ToolOutput::Full;
    s.ui.thinking         = up::Thinking::Shown;
    s.ui.timestamps       = up::Timestamps::Relative;
    REQUIRE(s.ui != up::Prefs{});

    const auto back = save_and_reload(s);

    // Field by field, so a failure names the setting rather than "ui differs".
    CHECK(back.ui.theme            == s.ui.theme);
    CHECK(back.ui.lang             == s.ui.lang);
    CHECK(back.ui.tier             == s.ui.tier);
    CHECK(back.ui.polarity         == s.ui.polarity);
    CHECK(back.ui.density          == s.ui.density);
    CHECK(back.ui.prose_width      == s.ui.prose_width);
    CHECK(back.ui.compact_turns    == s.ui.compact_turns);
    CHECK(back.ui.thread_preview   == s.ui.thread_preview);
    CHECK(back.ui.foreign_sessions == s.ui.foreign_sessions);
    CHECK(back.ui.motion           == s.ui.motion);
    CHECK(back.ui.scrollback       == s.ui.scrollback);
    CHECK(back.ui.syntax           == s.ui.syntax);
    CHECK(back.ui.tool_output      == s.ui.tool_output);
    CHECK(back.ui.thinking         == s.ui.thinking);
    CHECK(back.ui.timestamps       == s.ui.timestamps);
    // And the whole struct, which catches a field added to Prefs later and
    // never wired into save or load.
    CHECK(back.ui == s.ui);
}

TEST_CASE("settings round-trip: each scrollback level survives a reload") {
    // The reported field, every value -- including Full (0 rows) and Tiny,
    // the two ends of the enum where an off-by-one or a clamp would bite.
    for (auto lvl : {up::Scrollback::Tiny, up::Scrollback::Small,
                     up::Scrollback::Big, up::Scrollback::Full}) {
        TmpHome home{"sb"};
        agentty::store::Settings s;
        s.ui.scrollback = lvl;
        const auto back = save_and_reload(s);
        INFO("scrollback level " << static_cast<int>(lvl));
        CHECK(back.ui.scrollback == lvl);
    }
}

TEST_CASE("settings round-trip: one instance's save does not undo another's") {
    // Ayush runs several agentty sessions in tmux at once. Each process holds
    // its own in-memory Settings from startup and never re-reads the file,
    // and `ui` merges as ONE top-level key. So:
    //
    //   A and B both start with scrollback=Auto.
    //   A changes scrollback -> Small and saves.
    //   B changes something ELSE in `ui` (its theme) and saves.
    //
    // B's `ui` now differs from B's baseline (the theme), so the merge treats
    // the WHOLE block as B's edit -- and B's block still holds the
    // startup-era scrollback=Auto. A's change is silently reverted on disk.
    // That is "settings don't persist": they did, until a sibling saved.
    TmpHome home{"clobber"};
    const auto io = ::agentty::IoAccess::grant();

    // Both instances load the same starting file.
    agentty::store::Settings start;
    start.ui.theme = "nord";
    maya::thread_root([&](const maya::unlocked& c) {
        persistence::save_settings(io, c, start);
    });
    agentty::store::Settings a = persistence::load_settings(io);
    agentty::store::Settings b = a;        // B loaded the same bytes
    // Each instance remembers the file AS IT LOADED IT. One test process
    // has one baseline, so capture B's now and hand it back before B saves.
    std::string b_baseline;
    {
        std::ifstream f(home.dir / "settings.json");
        b_baseline.assign(std::istreambuf_iterator<char>(f), {});
    }

    // A changes scrollback.
    a.ui.scrollback = up::Scrollback::Small;
    maya::thread_root([&](const maya::unlocked& c) {
        persistence::save_settings(io, c, a);
    });
    REQUIRE(persistence::load_settings(io).ui.scrollback == up::Scrollback::Small);

    // B, never having seen A's edit, changes its theme. Restore B's view of
    // the world first: in a real second process this is simply its memory.
    persistence::debug_set_settings_baseline(b_baseline);
    b.ui.theme = "gruvbox";
    maya::thread_root([&](const maya::unlocked& c) {
        persistence::save_settings(io, c, b);
    });

    const auto disk = persistence::load_settings(io);
    CHECK(disk.ui.theme == "gruvbox");                  // B's edit landed
    CHECK(disk.ui.scrollback == up::Scrollback::Small);  // and A's survived
}

TEST_CASE("settings round-trip: a SECOND change in one session persists") {
    // The baseline merge in save_settings compares each top-level key with
    // the document this process last loaded or wrote. A first change is easy;
    // the interesting case is changing the SAME block again, and changing it
    // back to the default, without a restart in between.
    TmpHome home{"second"};
    const auto io = ::agentty::IoAccess::grant();

    agentty::store::Settings s = persistence::load_settings(io);   // empty
    s.ui.scrollback = up::Scrollback::Small;
    maya::thread_root([&](const maya::unlocked& c) {
        persistence::save_settings(io, c, s);
    });
    CHECK(persistence::load_settings(io).ui.scrollback == up::Scrollback::Small);

    // Change it again, same session.
    s.ui.scrollback = up::Scrollback::Big;
    maya::thread_root([&](const maya::unlocked& c) {
        persistence::save_settings(io, c, s);
    });
    CHECK(persistence::load_settings(io).ui.scrollback == up::Scrollback::Big);

    // And back to the shipped default. That omits the whole `ui` block from
    // the file, which the merge must read as OUR change, not as "absent, so
    // adopt whatever is on disk".
    s.ui.scrollback = up::Scrollback::Auto;
    REQUIRE(s.ui == up::Prefs{});
    maya::thread_root([&](const maya::unlocked& c) {
        persistence::save_settings(io, c, s);
    });
    CHECK(persistence::load_settings(io).ui.scrollback == up::Scrollback::Auto);
}
