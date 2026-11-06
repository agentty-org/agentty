#!/usr/bin/env python3
"""prefs_sync -- the four hand-written lists of ui::Prefs fields must agree.

WHY THIS EXISTS
===============
Every ui::Prefs field is spelled out by hand in four places:

  1. the struct itself          include/agentty/domain/ui_prefs.hpp
  2. the settings.json writer   src/io/persistence.cpp  (u["x"] = ...)
  3. the settings.json loader   src/io/persistence.cpp  (s.ui.x = u.value(...))
  4. the save-time merge table  src/io/persistence.cpp  (kUiDefaults)

Nothing made them agree. Each omission fails differently, silently, and only
on a path a unit test has to think to exercise:

  * missing from the writer -> the setting never reaches disk
  * missing from the loader -> it is written and then ignored on restart
  * missing from the merge table -> it is clobbered when another agentty
    session saves (this is what "settings don't persist" looked like)

A DEFAULT is restated twice more -- the struct's initialiser and the loader's
fallback -- and if those two disagree, a default reads back as a deliberate
choice and the merge misjudges who changed what.

This is a lint rather than a runtime test because the failure is structural:
the moment a field is added to the struct, the other three lists are wrong,
and the build should say so before anyone runs anything.

The real fix is for the field list to exist ONCE (an X-macro, or reflection
when the toolchain allows it) so these lists are generated. Until then this
makes drift loud and immediate instead of a bug report weeks later.

Usage: prefs_sync.py <ui_prefs.hpp> <persistence.cpp>
"""
import re
import sys

# Field types that appear in Prefs. Listed rather than matched loosely so a
# new type has to be added here deliberately -- an unrecognised type would
# otherwise make a field invisible to this lint, which is the exact failure
# the lint exists to prevent.
TYPES = (
    "ThemeName", "ColorTier", "Polarity", "Density", "Motion", "Scrollback",
    "ToolOutput", "Thinking", "Timestamps", "std::string", "int", "bool",
)


def die(msg: str) -> None:
    print(f"prefs_sync: {msg}", file=sys.stderr)
    sys.exit(1)


def struct_fields(hdr: str) -> list[str]:
    m = re.search(r"struct Prefs \{(.*?)\n\};", hdr, re.S)
    if not m:
        die("could not find `struct Prefs` in ui_prefs.hpp")
    body = m.group(1)
    pat = r"^\s+(?:" + "|".join(re.escape(t) for t in TYPES) + r")\s+(\w+)\s*(?:=|;)"
    out = [f for f in re.findall(pat, body, re.M) if f != "operator"]
    if not out:
        die("parsed zero fields out of struct Prefs -- the lint itself is broken")
    return out


def writer_keys(src: str) -> set[str]:
    m = re.search(r"if \(s\.ui != ui_prefs::Prefs\{\}\) \{(.*?)\n    \}", src, re.S)
    if not m:
        die("could not find the ui writer block in persistence.cpp")
    return set(re.findall(r'u\["(\w+)"\]', m.group(1)))


def loader_keys(src: str) -> set[str]:
    return set(re.findall(r"s\.ui\.(\w+)\s*=", src))


def merge_table(src: str) -> set[str]:
    m = re.search(r"static const json kUiDefaults = \{(.*?)\n            \};", src, re.S)
    if not m:
        die("could not find kUiDefaults in persistence.cpp")
    return set(re.findall(r'\{"(\w+)",', m.group(1)))


def main() -> None:
    if len(sys.argv) != 3:
        die("usage: prefs_sync.py <ui_prefs.hpp> <persistence.cpp>")
    hdr = open(sys.argv[1], encoding="utf-8").read()
    src = open(sys.argv[2], encoding="utf-8").read()

    fields = struct_fields(hdr)
    lists = {
        "settings.json writer":  writer_keys(src),
        "settings.json loader":  loader_keys(src),
        "save-time merge table": merge_table(src),
    }

    failed = False
    for name, have in lists.items():
        missing = [f for f in fields if f not in have]
        extra = sorted(have - set(fields))
        if missing:
            failed = True
            print(f"prefs_sync: {name} is missing: {', '.join(missing)}",
                  file=sys.stderr)
        if extra:
            failed = True
            print(f"prefs_sync: {name} mentions unknown field(s): "
                  f"{', '.join(extra)}", file=sys.stderr)

    if failed:
        print("\nEvery ui::Prefs field must appear in all three, or it silently\n"
              "fails to save, fails to load, or gets clobbered when a second\n"
              "agentty session saves. See tests/lint/prefs_sync.py.",
              file=sys.stderr)
        sys.exit(1)

    print(f"prefs_sync: {len(fields)} ui::Prefs fields, all three lists agree")


if __name__ == "__main__":
    main()
