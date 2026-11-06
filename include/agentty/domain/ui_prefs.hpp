#pragma once
// agentty::ui_prefs — every knob that changes how agentty LOOKS, in one
// value type.
//
// ── Why this is its own concern ──────────────────────────────────────────
//
// Appearance is the one part of the config that belongs to the PERSON, not
// the project. A permission profile is a property of the repo you are in;
// a light terminal is a property of your eyes and your office. So these
// live in the user store and follow you between checkouts, and nothing
// here is ever written to a project .agentty/.
//
// ── Why every field has an explicit "auto" ───────────────────────────────
//
// Two of these — the color tier and the background polarity — are things
// agentty can usually DETECT. Detection is right almost always and wrong
// unrecoverably: a terminal that lies about COLORTERM, or one that never
// set COLORFGBG, leaves a user staring at an unreadable screen with no way
// to say "no, it's light". Auto is the default and the override is the
// escape hatch, which is the whole of agentty issue #37.

#include <cstdint>
#include <string>
#include <string_view>

namespace agentty::ui_prefs {

// ── Theme ────────────────────────────────────────────────────────────────

// Which palette paints the UI.
//
// Empty means NATIVE: agentty states no colors of its own and the user's
// own terminal palette comes through (maya::theme::native). That is the
// default because it is the only choice that is correct on a light
// terminal, a dark one, and a monochrome one alike.
//
// Any other value names one of maya's built-in schemes ("Dracula",
// "Gruvbox Dark", …). Choosing one is the user saying "paint it like this",
// and agentty then owns the whole surface including the background.
using ThemeName = std::string;

// What agentty may emit.
//
// Auto runs maya's detection stack (NO_COLOR, TERM, COLORTERM, tty). The
// rest force a tier, for a terminal that misreports — or for someone who
// simply wants a black-and-white screen, which Mono gives them.
enum class ColorTier : std::uint8_t { Auto, TrueColor, Ansi256, Ansi16, Mono };

// Whether the terminal is light or dark.
//
// Only consulted when a named theme has both variants; `native` does not
// care, which is the point of it. Auto reads COLORFGBG and, failing that,
// leaves it unknown rather than guessing.
enum class Polarity : std::uint8_t { Auto, Dark, Light };

// ── Density ──────────────────────────────────────────────────────────────

// How tall a panel's scrollable body may be.
//
// The shared default is 14 rows, chosen so a picker fits a short terminal.
// On a tall one that is needlessly cramped — several stats tabs scroll for
// want of six rows — so this raises the ceiling. It is a MAXIMUM: the panel
// still clamps to what the terminal actually has.
enum class Density : std::uint8_t { Compact, Normal, Roomy };

[[nodiscard]] constexpr int viewport_rows(Density d) noexcept {
    switch (d) {
        case Density::Compact: return 10;
        case Density::Normal:  return 14;
        case Density::Roomy:   return 22;
    }
    return 14;
}

// ── Motion ───────────────────────────────────────────────────────────────

// Whether anything moves.
//
// Off is an accessibility setting before it is a preference: vestibular
// disorders make a typewriter reveal genuinely unpleasant, and a spinner
// on a slow SSH link is just noise on the wire. Off makes streamed text
// appear in whole lines and replaces every spinner with a static mark.
enum class Motion : std::uint8_t { Full, Reduced, Off };

// ── Scrollback ───────────────────────────────────────────────────────────

// How much of the conversation is redrawn into the terminal's scrollback
// when a thread opens or the window is resized. The redraw is the biggest
// memory and time cost of a long thread, and rows past the terminal's own
// scrollback limit are thrown away anyway. Auto matches the terminal's
// scrollback length (maya/terminal/scrollback.hpp). Full redraws everything.
enum class Scrollback : std::uint8_t { Tiny, Small, Big, Full, Auto };

// Rows for a fixed level; 0 = no limit. Auto is resolved by the caller.
[[nodiscard]] constexpr int scrollback_rows(Scrollback s) noexcept {
    switch (s) {
        case Scrollback::Tiny:  return 1'000;
        case Scrollback::Small: return 5'000;
        case Scrollback::Big:   return 20'000;
        case Scrollback::Full:  return 0;
        case Scrollback::Auto:  return 5'000;   // until the terminal is known
    }
    return 5'000;
}

// ── Chrome ───────────────────────────────────────────────────────────────

// How much of a tool's output is shown before you ask for more.
enum class ToolOutput : std::uint8_t { Collapsed, Preview, Full };

// Whether the model's reasoning is shown.
enum class Thinking : std::uint8_t { Shown, Collapsed, Hidden };

// Whether turns carry a time, and in what form.
enum class Timestamps : std::uint8_t { Off, Relative, Absolute };

// ── The whole of it ──────────────────────────────────────────────────────

struct Prefs {
    ThemeName  theme;                              // empty = native
    ColorTier  tier        = ColorTier::Auto;
    Polarity   polarity    = Polarity::Auto;

    // UI language, as a BCP-47 tag ("de", "pt-BR"). EMPTY means "follow the
    // environment" -- $LC_ALL / $LC_MESSAGES / $LANG, then English.
    //
    // Empty-as-auto rather than defaulting to "en", for the same reason
    // `theme` is empty-as-native: a user who never opened the picker should
    // get their system's language, and a stored "en" would be
    // indistinguishable from an explicit choice of English. Someone who
    // deliberately picks English gets "en" written out, and then a later
    // `LANG=de_DE` does not override them.
    //
    // This is agentty's CHROME only. What language the model answers in is
    // decided by what the user types to it, and is deliberately not coupled
    // to this -- see docs/design/i18n.md.
    std::string lang;

    Density    density     = Density::Normal;
    // Reading measure for assistant prose, in columns. Long lines are hard
    // to track back to the next line's start; 0 means "no cap".
    int        prose_width = 0;
    bool       compact_turns = false;
    // The framed preview of the highlighted thread in ^J. Off gives the
    // list the whole panel.
    bool       thread_preview = true;
    // List Codex / Claude Code sessions in ^J so they can be continued here.
    bool       foreign_sessions = true;

    Motion     motion      = Motion::Full;
    Scrollback scrollback  = Scrollback::Auto;

    bool       syntax      = true;
    ToolOutput tool_output = ToolOutput::Preview;
    Thinking   thinking    = Thinking::Collapsed;
    Timestamps timestamps  = Timestamps::Off;

    bool operator==(const Prefs&) const = default;
};

// ── Labels ───────────────────────────────────────────────────────────────
//
// One spelling per value, used by the settings rows AND anything else that
// reports the state, so the two can never drift.

[[nodiscard]] constexpr std::string_view label(ColorTier t) noexcept {
    switch (t) {
        case ColorTier::Auto:      return "auto";
        case ColorTier::TrueColor: return "truecolor";
        case ColorTier::Ansi256:   return "256 colors";
        case ColorTier::Ansi16:    return "16 colors";
        case ColorTier::Mono:      return "monochrome";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view label(Polarity p) noexcept {
    switch (p) {
        case Polarity::Auto:  return "auto";
        case Polarity::Dark:  return "dark";
        case Polarity::Light: return "light";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view label(Density d) noexcept {
    switch (d) {
        case Density::Compact: return "compact";
        case Density::Normal:  return "normal";
        case Density::Roomy:   return "roomy";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view label(Motion m) noexcept {
    switch (m) {
        case Motion::Full:    return "full";
        case Motion::Reduced: return "reduced";
        case Motion::Off:     return "off";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view label(Scrollback s) noexcept {
    switch (s) {
        case Scrollback::Tiny:  return "tiny";
        case Scrollback::Small: return "small";
        case Scrollback::Big:   return "big";
        case Scrollback::Full:  return "full";
        case Scrollback::Auto:  return "auto";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view label(ToolOutput t) noexcept {
    switch (t) {
        case ToolOutput::Collapsed: return "collapsed";
        case ToolOutput::Preview:   return "preview";
        case ToolOutput::Full:      return "full";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view label(Thinking t) noexcept {
    switch (t) {
        case Thinking::Shown:     return "shown";
        case Thinking::Collapsed: return "collapsed";
        case Thinking::Hidden:    return "hidden";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view label(Timestamps t) noexcept {
    switch (t) {
        case Timestamps::Off:      return "off";
        case Timestamps::Relative: return "relative";
        case Timestamps::Absolute: return "absolute";
    }
    return "?";
}

// ── Cycling ──────────────────────────────────────────────────────────────
//
// Enter on a row advances it. Every enum wraps, so a row is a loop the user
// can always get back around — never a dead end at the last value.

template <typename E, std::uint8_t N>
[[nodiscard]] constexpr E cycle(E v) noexcept {
    return static_cast<E>((static_cast<std::uint8_t>(v) + 1) % N);
}

[[nodiscard]] constexpr ColorTier  next(ColorTier v)  noexcept { return cycle<ColorTier, 5>(v); }
[[nodiscard]] constexpr Polarity   next(Polarity v)   noexcept { return cycle<Polarity, 3>(v); }
[[nodiscard]] constexpr Density    next(Density v)    noexcept { return cycle<Density, 3>(v); }
[[nodiscard]] constexpr Motion     next(Motion v)     noexcept { return cycle<Motion, 3>(v); }
[[nodiscard]] constexpr Scrollback next(Scrollback v) noexcept { return cycle<Scrollback, 5>(v); }
[[nodiscard]] constexpr ToolOutput next(ToolOutput v) noexcept { return cycle<ToolOutput, 3>(v); }
[[nodiscard]] constexpr Thinking   next(Thinking v)   noexcept { return cycle<Thinking, 3>(v); }
[[nodiscard]] constexpr Timestamps next(Timestamps v) noexcept { return cycle<Timestamps, 3>(v); }

// ── The field table ──────────────────────────────────────────────────────
//
// EVERY Prefs field, declared ONCE. The settings.json writer, its loader and
// the save-time merge all walk this; none of them names a field.
//
// WHY THIS EXISTS
// ===============
// Those three used to spell the list out by hand, and nothing made them
// agree. Each omission failed differently and silently: missing from the
// writer meant the setting never reached disk; missing from the loader meant
// it was written and then ignored on restart; missing from the merge meant a
// second agentty session clobbered it on its next save. That last one is what
// "settings don't persist" looked like in practice.
//
// The other settings blocks (smart, rag, web_search, paste) have had this
// shape for a while via settings_registry.hpp. `ui` predates it. It does not
// move ONTO that registry because the registry stores an enum as a STRING
// ("convex", "auto") while ui stores typed enums written as integers --
// migrating would change the on-disk format for every existing user to buy
// nothing the table below does not already give.
//
// A field here is `member pointer + json key`. The default comes from
// `Prefs{}` itself, through the member pointer, so there is no second copy of
// it to drift: this table cannot disagree with the struct about what a
// default is, because it never states one.

// Visit every Prefs field, passing (member-pointer, json-key) to `f`.
//
// A generic lambda sees each field at its OWN type, so a visitor can switch
// on it with `if constexpr` -- an int stays an int, an enum stays its enum,
// and nothing round-trips through a variant or a string.
//
// ADDING A FIELD: add it to Prefs, add one line here. The writer, loader and
// merge pick it up with no edit. Forgetting the line is caught by
// tests/lint/prefs_sync.py, which compares this table against the struct.
template <typename F>
constexpr void for_each_field(F&& f) {
    f(&Prefs::theme,            std::string_view{"theme"});
    f(&Prefs::lang,             std::string_view{"lang"});
    f(&Prefs::tier,             std::string_view{"tier"});
    f(&Prefs::polarity,         std::string_view{"polarity"});
    f(&Prefs::density,          std::string_view{"density"});
    f(&Prefs::prose_width,      std::string_view{"prose_width"});
    f(&Prefs::compact_turns,    std::string_view{"compact_turns"});
    f(&Prefs::thread_preview,   std::string_view{"thread_preview"});
    f(&Prefs::foreign_sessions, std::string_view{"foreign_sessions"});
    f(&Prefs::motion,           std::string_view{"motion"});
    f(&Prefs::scrollback,       std::string_view{"scrollback"});
    f(&Prefs::syntax,           std::string_view{"syntax"});
    f(&Prefs::tool_output,      std::string_view{"tool_output"});
    f(&Prefs::thinking,         std::string_view{"thinking"});
    f(&Prefs::timestamps,       std::string_view{"timestamps"});
}

}  // namespace agentty::ui_prefs
