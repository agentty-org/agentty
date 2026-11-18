#!/usr/bin/env python3
"""Time agentty's native tools through `agentty mcp-serve` (the real path:
dispatcher, sandbox, gate). Usage: tool_latency.py [AGENTTY_BIN] [N]"""
import json, subprocess, sys, time, statistics, os

BIN = sys.argv[1] if len(sys.argv) > 1 else "./build/agentty"
N = int(sys.argv[2]) if len(sys.argv) > 2 else 30
ROOT = os.getcwd()

p = subprocess.Popen([BIN, "mcp-serve"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.DEVNULL, text=True, bufsize=1)
nid = 0
def rpc(method, params=None):
    global nid
    nid += 1
    p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": nid, "method": method,
                              "params": params or {}}) + "\n")
    p.stdin.flush()
    while True:
        line = p.stdout.readline()
        if not line:
            raise SystemExit("server exited")
        m = json.loads(line)
        if m.get("id") == nid:
            return m

rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                   "clientInfo": {"name": "bench", "version": "0"}})
p.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
p.stdin.flush()
tools = {t["name"] for t in rpc("tools/list")["result"]["tools"]}

big = "src/tool/util/sandbox.cpp"
cases = [
    ("shell: true", "shell", {"command": "true"}),
    ("shell: ls", "shell", {"command": "ls"}),
    ("shell: git status", "shell", {"command": "git status --short"}),
    ("read: 250-line window", "read", {"path": big, "offset": 400}),
    ("read: whole small file", "read", {"path": "scripts/build.sh"}),
    ("grep: literal, tree", "grep", {"pattern": "box_env_keeps"}),
    ("grep: regex, glob", "grep", {"pattern": "env_allowed\\(", "glob": "*.cpp"}),
    ("glob: *.hpp", "glob", {"pattern": "*.hpp"}),
    ("list_dir", "list_dir", {"path": "src"}),
    ("outline", "outline", {"path": big}),
    ("edit: 1 line, 2k-line file", "edit", {}),
    ("write: small", "write", {"file_path": os.path.join(ROOT, ".agentty_bench_w.txt"), "content": "x\n" * 50}),
]
EDIT = os.path.join(ROOT, ".agentty_bench_edit.txt")
open(EDIT, "w").write("".join(f"line {i} of a typical source file\n" for i in range(2000)))

print(f"{'case':26s} {'p50 ms':>8s} {'p90 ms':>8s} {'min':>7s}")
for label, tool, args in cases:
    if tool not in tools:
        print(f"{label:26s}  (tool not served)"); continue
    ts = []
    for i in range(N):
        if tool == "edit":   # alternate so each call is a real change
            a, b = ("line 1000 of a", "LINE 1000 of a") if i % 2 == 0 else ("LINE 1000 of a", "line 1000 of a")
            args = {"path": EDIT, "edits": [{"old_text": a, "new_text": b}]}
        t = time.perf_counter()
        r = rpc("tools/call", {"name": tool, "arguments": args})
        ts.append((time.perf_counter() - t) * 1000)
        if i == 0 and (r.get("error") or r.get("result", {}).get("isError")):
            print(f"  {label}: ERROR {json.dumps(r)[:200]}")
    ts.sort()
    print(f"{label:26s} {statistics.median(ts):8.1f} {ts[int(.9*len(ts))-1]:8.1f} {ts[0]:7.1f}")
p.terminate()
for f in (EDIT, os.path.join(ROOT, ".agentty_bench_w.txt")):
    try: os.remove(f)
    except OSError: pass
