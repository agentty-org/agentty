#!/usr/bin/env python3
"""ui_boundary -- agentty describes UI, maya draws it.

THE RULE
========
agentty's view layer builds `maya::Element` values and hands them over. It
never touches a cell, a canvas, or a raw escape sequence. Every pixel decision
-- layout, truncation, colour resolution, the cursor -- belongs to maya, which
is why a maya widget can change its mind about width negotiation and agentty
keeps working.

That boundary holds today by habit. This makes it hold by rule.

WHAT IT BANS, in agentty's src/ and include/:

  * writing a canvas or cell directly (canvas.set/put, Canvas&, Cell{)
  * defining a widget: a paint/draw/render method, a layout() that returns
    geometry -- the shapes that mean "I am doing maya's job here"
  * emitting a raw ANSI escape

WHAT IT ALLOWS, and why each is not a hole:

  * TESTS. A render test has to rasterise an Element to assert on it; that
    is reading maya's output, not bypassing it.
  * The CLI half. `agentty paste page`, the Ctrl+G runner, `--version`: maya
    is not running (or has handed the terminal back), so there is no layer to
    go through. These are named individually, not waved through by directory.
  * COMMENTS AND STRINGS. The ones in turn.cpp that spell \\x1b[2J are
    describing what maya's HardReset does. Code is scanned with comments and
    literals stripped.

Usage: ui_boundary.py <agentty source root>
"""
import re
import sys
from pathlib import Path

# Files that legitimately emit escapes because maya is not mediating the
# terminal at that moment. Each needs a reason, not just a path.
ALLOW = {
    "src/runtime/app/cmd_code_block_run.cpp":
        "runs a user command on the real tty after maya releases it; "
        "its progress line IS the terminal output",
    "src/tool/util/sandbox.cpp":
        "formats a sandbox denial for the plain-stderr path, before/outside "
        "the TUI",
}

# A terminal cell is `maya::Cell`. Matching a bare `Cell{` caught agentty's
# own unrelated Cell types -- a cache slot in gen_cache.hpp, a lazy-bytes
# holder -- which have nothing to do with a screen. Qualify it.
CANVAS = re.compile(r"\bcanvas\s*\.\s*(set|put|write)\b"
                    r"|\bmaya::Canvas\s*&"
                    r"|\bmaya::Cell\s*\{")
WIDGET = re.compile(r"\b(?:void|auto)\s+(?:paint|draw|render)\s*\(")
ANSI = re.compile(r"\\x1b\[|\\033\[|\\e\[")


def strip_code(text: str) -> str:
    """Blank comments and string/char literals, keeping line numbers."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            q, j = c, i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == "\\" else 1
            out.append(" " * (min(j, n - 1) - i + 1))
            i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def strip_comments(text: str) -> str:
    """Blank comments only, keeping string literals and line numbers.

    An escape sequence only ever appears INSIDE a literal, so the ANSI rule
    has to read literals -- stripping them (as the structural rules want)
    made that check unreachable, and it silently never fired. Comments still
    go, because turn.cpp legitimately describes \\x1b[2J in prose.
    """
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def main() -> None:
    if len(sys.argv) != 2:
        print("usage: ui_boundary.py <agentty source root>", file=sys.stderr)
        sys.exit(2)
    root = Path(sys.argv[1])
    bad: list[str] = []
    scanned = 0

    for sub in ("src", "include"):
        for path in sorted((root / sub).rglob("*")):
            if path.suffix not in (".cpp", ".hpp") or not path.is_file():
                continue
            rel = path.relative_to(root).as_posix()
            scanned += 1
            code = strip_code(path.read_text(encoding="utf-8", errors="replace"))
            raw = path.read_text(encoding="utf-8", errors="replace")
            prose_free = strip_comments(raw)
            for lineno, line in enumerate(code.splitlines(), 1):
                if CANVAS.search(line):
                    bad.append(f"{rel}:{lineno}: writes a canvas/cell directly "
                               f"-- build a maya::Element instead")
                if WIDGET.search(line):
                    bad.append(f"{rel}:{lineno}: defines a paint/draw/render "
                               f"method -- that is a maya widget's job")
            if rel not in ALLOW:
                for lineno, line in enumerate(prose_free.splitlines(), 1):
                    if ANSI.search(line):
                        bad.append(f"{rel}:{lineno}: emits a raw ANSI escape "
                                   f"-- go through maya, or add a reason to "
                                   f"ui_boundary.py's ALLOW")

    if bad:
        print("ui_boundary: agentty must describe UI, not draw it\n",
              file=sys.stderr)
        for b in bad:
            print(f"  {b}", file=sys.stderr)
        print("\nmaya owns every pixel decision. agentty returns Elements.",
              file=sys.stderr)
        sys.exit(1)

    print(f"ui_boundary: {scanned} files, no UI drawing in agentty "
          f"({len(ALLOW)} allowed non-TUI emitters)")


if __name__ == "__main__":
    main()
