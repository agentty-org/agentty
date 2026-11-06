#!/usr/bin/env python3
"""prefs_sync -- every ui::Prefs field must appear in the field table.

WHY THIS EXISTS
===============
The settings.json writer, its loader and the save-time merge used to spell
out every Prefs field by hand. Nothing made those three lists agree, and each
omission failed differently and silently: missing from the writer meant the
setting never reached disk; missing from the loader meant it was written and
then ignored on restart; missing from the merge meant a second agentty
session clobbered it on its next save. That last one is what "settings don't
persist" looked like.

All three now walk ONE table -- `ui_prefs::for_each_field` in
include/agentty/domain/ui_prefs.hpp -- so none of them names a field and they
cannot disagree. Defaults come from `Prefs{}` through the member pointer, so
there is no second copy of a default either.

What is left to check is the single remaining seam: a field added to the
struct and NOT to the table. The compiler cannot see that (C++ has no
reflection here), and the field would simply never persist. So: compare the
two, and fail the build naming the field.

Usage: prefs_sync.py <ui_prefs.hpp>
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
    pat = r"^\s+(?:" + "|".join(re.escape(t) for t in TYPES) + r")\s+(\w+)\s*(?:=|;)"
    out = [f for f in re.findall(pat, m.group(1), re.M) if f != "operator"]
    if not out:
        die("parsed zero fields out of struct Prefs -- the lint itself is broken")
    return out


def table_fields(hdr: str) -> list[str]:
    m = re.search(r"constexpr void for_each_field\(F&& f\) \{(.*?)\n\}", hdr, re.S)
    if not m:
        die("could not find for_each_field in ui_prefs.hpp")
    out = re.findall(r"&Prefs::(\w+)", m.group(1))
    if not out:
        die("parsed zero fields out of for_each_field -- the lint itself is broken")
    return out


def main() -> None:
    if len(sys.argv) != 2:
        die("usage: prefs_sync.py <ui_prefs.hpp>")
    hdr = open(sys.argv[1], encoding="utf-8").read()

    fields = struct_fields(hdr)
    table = table_fields(hdr)

    missing = [f for f in fields if f not in table]
    extra = [f for f in table if f not in fields]
    dupes = sorted({f for f in table if table.count(f) > 1})

    if missing or extra or dupes:
        if missing:
            print("prefs_sync: in struct Prefs but NOT in for_each_field: "
                  + ", ".join(missing), file=sys.stderr)
        if extra:
            print("prefs_sync: in for_each_field but not a Prefs field: "
                  + ", ".join(extra), file=sys.stderr)
        if dupes:
            print("prefs_sync: listed twice in for_each_field: "
                  + ", ".join(dupes), file=sys.stderr)
        print("\nThe settings writer, loader and save-time merge all walk\n"
              "for_each_field. A field missing from it is never written,\n"
              "never loaded, and clobbered when a second agentty session\n"
              "saves. Add one line to the table.", file=sys.stderr)
        sys.exit(1)

    print(f"prefs_sync: {len(fields)} ui::Prefs fields, all in the field table")


if __name__ == "__main__":
    main()
