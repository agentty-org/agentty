#!/bin/sh
# question_repro.sh — agentty in a tmux pane against the fake server
# (tests/acp/fake_openai_server.py, "ask me questions"), a few turns of
# history, then the question card: captures the real screen after opening,
# ↓, Enter and a typed key. For layout bugs that only show in a terminal.
# Not a ctest.
#   usage: question_repro.sh <agentty> [cols] [rows]
set -u
A="$1"; W="${2:-140}"; H="${3:-27}"
HERE=$(cd "$(dirname "$0")" && pwd)
S=$(mktemp -d /tmp/agentty_qrepro_XXXXXX)
python3 "$HERE/acp/fake_openai_server.py" 0 > "$S/port" 2>/dev/null </dev/null &
SRV=$!
i=0; while [ $i -lt 100 ] && [ ! -s "$S/port" ]; do i=$((i+1)); timeout 0.1 tail -f /dev/null; done
PORT=$(cat "$S/port")
mkdir -p "$S/home" "$S/ws"
printf '{"provider":"127.0.0.1:%s","model_id":"o4-mini"}\n' "$PORT" > "$S/home/settings.json"
SOCK="$S/tmux.sock"
tmux -S "$SOCK" new-session -d -x "$W" -y "$H" \
  "cd $S/ws && env -u ANTHROPIC_API_KEY -u OPENAI_API_KEY AGENTTY_HOME=$S/home AGENTTY_NO_PREWARM=1 AGENTTY_NO_UPDATE_CHECK=1 $A"
cap() { tmux -S "$SOCK" capture-pane -p -t 0 > "$S/$1.txt"; }
wait_for() { # pattern, tries
  n=0; while [ $n -lt "$2" ]; do
    tmux -S "$SOCK" capture-pane -p -t 0 | grep -q "$1" && return 0
    n=$((n+1)); timeout 0.2 tail -f /dev/null; done; return 1; }
wait_for "type a message" 50 || wait_for "❯" 50
for k in 1 2 3 4 5 6; do
  tmux -S "$SOCK" send-keys -t 0 -l "hi $k"; tmux -S "$SOCK" send-keys -t 0 Enter
  wait_for "turn $k" 50; done
tmux -S "$SOCK" send-keys -t 0 -l "ask me questions"
tmux -S "$SOCK" send-keys -t 0 Enter
wait_for "A few questions" 50
timeout 1 tail -f /dev/null
cap open
tmux -S "$SOCK" send-keys -t 0 Down; timeout 0.6 tail -f /dev/null; cap down
tmux -S "$SOCK" send-keys -t 0 Enter; timeout 0.6 tail -f /dev/null; cap q2
tmux -S "$SOCK" send-keys -t 0 -l "x"; timeout 0.6 tail -f /dev/null; cap typed
for f in open down q2 typed; do echo "===== $f"; cat -n "$S/$f.txt"; done
tmux -S "$SOCK" kill-server 2>/dev/null
kill $SRV 2>/dev/null
rm -rf "$S"
