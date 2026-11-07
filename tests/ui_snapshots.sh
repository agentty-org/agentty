#!/bin/sh
# Golden snapshots of how agentty looks: every tool card plus the screen
# chrome, rendered by tool_gallery with colour (SGR) included, since colour
# is most of what these surfaces are. A change that alters how something
# looks shows up as a diff here.
#
#   tests/ui_snapshots.sh build/tool_gallery            # check
#   tests/ui_snapshots.sh build/tool_gallery --update   # accept new output
#
# Fixed width, theme and terminal reply so the output is the same on every
# machine.
set -eu
gallery=${1:?usage: ui_snapshots.sh <tool_gallery> [--update]}
mode=${2:-check}
here=$(cd "$(dirname "$0")" && pwd)
dir="$here/fixtures/ui_snapshots"
mkdir -p "$dir"
export COLS=92 THEME=Srcery FREEZE_CLOCK=1 TOOL_CORPUS="$here/fixtures/tool_corpus.json"
unset TERM_FG TERM_BG TERM_PALETTE

# name | args
cases='
cards-typical|--kind typical
cards-large|--kind large
cards-failed|--kind failed
running|--running --kind typical bash edit grep
ask|--ask --kind typical bash edit web_fetch git_commit
chrome|--chrome
prose|--prose
welcome|--welcome
panels|--panels
native|--kind typical edit grep
'
fail=0
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
echo "$cases" | while IFS='|' read -r name args; do
    [ -z "$name" ] && continue
    if [ "$name" = native ]; then
        THEME=native "$gallery" $args >"$tmp" 2>&1
    else
        "$gallery" $args >"$tmp" 2>&1
    fi
    snap="$dir/$name.ansi"
    if [ "$mode" = --update ]; then
        cp "$tmp" "$snap"
        echo "updated $name"
    elif [ ! -f "$snap" ]; then
        echo "MISSING $name (run with --update)"; exit 1
    elif ! cmp -s "$tmp" "$snap"; then
        echo "CHANGED $name:"
        diff "$snap" "$tmp" | sed -n '1,20p' | cat -v
        exit 1
    else
        echo "ok      $name"
    fi
done || fail=1
exit $fail
