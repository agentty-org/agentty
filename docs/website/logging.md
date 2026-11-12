---
title: Logging & diagnostics
description: One log, one switch. Leveled channels, raw wire bytes, a crash-time flight recorder, and everything captured by default in non-release builds.
nav_section: Advanced
nav_order: 55
slug: logging
---

agentty writes a log so you can see what went wrong when a provider errors, a
custom host won't answer, or a model seems to ignore tools. The log lives in
one file, `~/.agentty/logs/agentty.log`, and one variable, `AGENTTY_LOG`,
controls how much goes into it.

:::tip You don't have to set anything up front
Release builds keep warnings and errors by default. A healthy session writes
about two lines. So when something breaks, just run `agentty diagnostics`: the
failure is already in the file. Raw wire bytes and per-event detail still need
`AGENTTY_LOG`.

Non-release builds capture everything at `trace` with no variable set.
:::

## See the log {#see-the-log}

```bash
agentty diagnostics                      # collect a report (no setup needed)
tail -f ~/.agentty/logs/agentty.log      # watch it live
AGENTTY_LOG=trace agentty                # capture everything
AGENTTY_LOG=warn,wire=trace agentty      # raw provider bytes, quiet elsewhere
```

Pick the command that matches your problem:

| Symptom | Do this |
|---------|---------|
| Anything just broke | `agentty diagnostics` |
| Model "ignores tools", empty tool args | `AGENTTY_LOG=wire=trace agentty`, then read the raw bytes |
| Custom host / local server won't answer | `grep 'http\.\|provider.select' ~/.agentty/logs/agentty.log` |
| Turn failed and you don't know why | `grep 'stream\.' ~/.agentty/logs/agentty.log` |
| A tool keeps failing | `grep 'tool.exec' ~/.agentty/logs/agentty.log` |
| A tool hung (no outcome logged) | `grep 'tool.dispatch' ~/.agentty/logs/agentty.log`. A `dispatch` with no matching `exec` is the hung call |
| Plugin tools missing | `grep 'mcp.connect' ~/.agentty/logs/agentty.log` |
| Signed out unexpectedly | `grep 'auth.refresh' ~/.agentty/logs/agentty.log` |
| Settings / history not saved | `grep 'settings.save\|thread.save' ~/.agentty/logs/agentty.log` |
| Something feels slow | `AGENTTY_LOG=perf=debug agentty`, then `grep 'turn.ttft\|thread.load' …` |
| Everything, maximum detail | `AGENTTY_LOG=trace agentty` |

Useful greps:

```bash
grep ' E ' ~/.agentty/logs/agentty.log                     # every error
grep ' W \| E ' ~/.agentty/logs/agentty.log                # errors + warnings
grep ' wire ' ~/.agentty/logs/agentty.log                  # everything on the wire
grep 'stream.end' ~/.agentty/logs/agentty.log | tail -20   # how turns ended
grep -n "=== agentty" ~/.agentty/logs/agentty.log          # split into sessions
```

## Reporting a bug

### `agentty diagnostics`

Run one command and attach the file it writes:

```bash
agentty diagnostics
```

It writes `~/.agentty/logs/agentty-diagnostics.txt` (mode `0600`). The file holds:

- your version, OS, architecture and build type;
- the `startup` and `provider.select` lines, read back from the log, so they
  describe the run that failed, not the collector;
- retrieval, storage and log-file status;
- recent warnings and errors from the in-memory flight recorder;
- the last 4000 lines of the log.

You don't need to reproduce the bug under a special flag first. Warnings and
errors are kept by default. Add `AGENTTY_LOG=wire=trace` only when the answer
needs the raw provider bytes.

The report is plain text, so you can read it before you send it. Secrets are
removed as each line is written, and the command prints how many it removed.

If logging was off when the problem happened, the report says so. Then:

```bash
AGENTTY_LOG=trace agentty      # reproduce the problem
agentty diagnostics            # then collect
```

### Manual collection

```bash
AGENTTY_LOG=trace agentty --log-file /tmp/agentty.log
# reproduce, then attach /tmp/agentty.log
```

For a provider or custom-host problem, use `AGENTTY_LOG=wire=trace`. It
captures what the server actually sent, which is usually where the answer is.
If agentty crashed, also attach its stderr (backtrace and flight recorder).

## Settings {#settings}

| Setting | What it does | Default |
|---------|--------------|---------|
| `AGENTTY_LOG` | What to capture: a level, plus optional `channel=level` overrides | `warn` in release builds, `trace` otherwise |
| `AGENTTY_LOG_BODIES` | Also log full bodies, such as tool arguments in `tool.dispatch`. Any value except empty or `0` turns it on | off |
| `--log-file <path>` | Write the log to this file | `~/.agentty/logs/agentty.log` |
| `AGENTTY_HOME` | Root directory; the log goes in `$AGENTTY_HOME/logs/` | `~/.agentty` |
| `AGENTTY_LOGS_DIR` | Log directory. Absolute paths are used as-is; relative ones are resolved against the root, not the current directory | `logs` |

`AGENTTY_LOG` examples:

```bash
AGENTTY_LOG=debug                        # every channel at debug and above
AGENTTY_LOG=wire=trace                   # just the wire channel, at trace
AGENTTY_LOG=warn,wire=trace,auth=debug   # default warn; wire and auth louder
AGENTTY_LOG=off                          # silence, including the default warnings
```

The build type only changes the default level. `AGENTTY_LOG=trace` on a release
binary gives you the same log as a debug build, so you can ask a user of the
released binary for a full trace.

### File and rotation {#rotation}

- The file is append-only (`O_APPEND`).
- At 32 MB, at startup or mid-run, the file is renamed to `agentty.log.old`
  (replacing the previous `.old`) and a new one starts. You keep at most about
  64 MB.
- You can truncate it while agentty runs: `: > ~/.agentty/logs/agentty.log`.
- Running several instances? Give each its own file:
  `./build/agentty --log-file /tmp/agentty-$$.log`.

## Levels {#levels}

| Level | Char | Use |
|-------|------|-----|
| `trace` | `T` | Per event or per byte |
| `debug` | `D` | Per-decision detail |
| `info` | `I` | Notable but healthy |
| `warn` | `W` | A failure or a persisted behaviour change. Kept by default in release |
| `error` | `E` | A broken invariant |
| `off` | | Silences the channel |

A filter of `warn` passes `warn` and `error`.

## Channels {#channels}

| Channel | Covers |
|---------|--------|
| `wire` | HTTP/SSE transports, raw request/response bytes at `trace`, and how every turn ended (`stream.end`) |
| `auth` | OAuth flows, token refresh, key resolution, account switching |
| `persist` | Settings, threads and memory disk I/O, including failed saves |
| `tool` | Every tool call: name, duration, outcome, and the arguments that caused a failure |
| `ui` | Reducer decisions: stream errors as the user sees them, retries and backoff |
| `rag` | The retrieval engine |
| `mcp` | MCP bridge and plugins, including server connect failures |
| `acp` | ACP server and adapter; every JSON-RPC frame at `trace` |
| `smart` | Smart Mode routing decisions |
| `net` | Sockets, TLS, proxy, prewarm, and per-attempt connect failures with the endpoint tried |
| `model` | Provider/model differences: dialect adapter, effort after the capability clamp, learned capabilities, fallbacks, tool-call salvage |
| `perf` | Timings: TTFT per model, tool-batch width, thread-load and view-build cost, per-frame stream pacing |
| `general` | Uncategorised (swallowed exceptions land here) |

## Line format {#format}

```
2026-08-28T01:23:45.678 +0012345ms 1a2b W wire    openai.stream: connect refused host=localhost:8080
└── wall clock ────────┘ └ mono ─┘ tid  L channel  site: message
```

- **wall clock**: local time, millisecond precision.
- **`+…ms`**: time since start. A gap between two lines is a stall.
- **`tid`**: short thread tag.
- **`L`**: level char (`T`/`D`/`I`/`W`/`E`).
- **channel** and **site** (`openai.stream`, `persistence.save`) name the emitter.

Each run starts with a session banner, so you can split a long log by run:

```
=== agentty session: 0.5.0 debug pid=76883 cwd=/Users/you/projects/x ===
```

## Site tags worth knowing {#sites}

Grep for the tag, not for the message text.

| Site | Answers |
|------|---------|
| `startup` | Which version, OS and build is this? (`warn`, kept by default) |
| `provider.select` | Which provider and endpoint was active? (`warn`, kept by default) |
| `stream.end` | How did this turn end? `clean_close` · `cancelled` · `http_error` · `transport_error` · `already_terminated` |
| `stream.http_error` / `stream.transport_error` | The exact failure text the user saw |
| `stream.error` | How the reducer classified a failure (retry or show it) |
| `stream.retry` | Which attempt, and how long the backoff was |
| `models.loaded` | How many models the catalog returned, and any fetch error |
| `dispatch.turn` | The turn fingerprint: route, provider, model, effort, tool count, protocol flags |
| `caps.effort_learned` | A capability learned from a provider rejection. Persisted, so it affects later sessions |
| `salvage.tool_call` / `salvage.dropped_*` | A tool call recovered from (or lost in) JSON the model leaked into its text |
| `responses.tool_args_unroutable` | Tool arguments arrived but couldn't be attached to a call (Responses dialect) |
| `copilot.models.*` / `copilot.auto_session.*` | Why Copilot fell back to the bundled catalog or lost its Auto session |
| `tool.dispatch` | Every tool call before it runs: name, `seq`, args (args need `AGENTTY_LOG_BODIES`). A `dispatch` with no `tool.exec` at the same `seq` hung, blocked, or crashed |
| `tool.pre_hook_blocked` | A `pre_tool` hook refused the call, and its reason |
| `tool.exec` | Every tool call: name, duration, outcome, and the arguments on failure |
| `mcp.connect` | Did each plugin server connect, and how many tools did it offer? |
| `auth.refresh` | Did the OAuth token refresh succeed? |
| `http.connect_failed` / `http.local_failed` | Which host:port refused, and why |
| `thread.save` / `settings.save` | Did your conversation or settings save? |
| `rag.retrieve` | How many passages came back, and through which pipeline |
| `*.request.body` / `wire.chunk` | Raw bytes (needs `wire=trace`) |

Reading `stream.end`:

```
W wire  stream.end: end=transport_error http=0 stop=unspecified replayable=true
```

- **`end`**: `transport_error` means the connection failed; `http_error` means the server answered with a status of 400 or more.
- **`http`**: the status code, or `0` if headers never arrived.
- **`stop`**: why the model stopped: `end_turn` · `tool_use` · `max_tokens` · `stop_sequence` · `unspecified`.
- **`replayable`**: whether agentty may safely retry the request.

## Debugging a provider {#debugging-a-provider}

Read this when a model "ignores tools", sends empty tool arguments, or a turn
fails in a way that looks like model quality. The cause is usually a wire event
the parser didn't decode, not the model.

```bash
AGENTTY_LOG=wire=trace agentty
```

At `trace`, the `wire` channel logs the full request body and every response
chunk, verbatim and untruncated. Each is tagged with its dialect:

| Dialect tag | Endpoint |
|-------------|----------|
| `openai-chat` | `/chat/completions` (OpenAI, Groq, Mistral, custom hosts, Copilot) |
| `openai-responses` | `/responses` (ChatGPT/Codex, some Copilot models) |
| `anthropic-messages` | `/v1/messages` |
| `ollama-native` | Ollama's `/api/chat` |

One account can serve some models on `/chat/completions` and others on
`/responses`, so the tag tells you which decoder to suspect. Bytes are never
pretty-printed, re-encoded or clipped.

:::warn A trace contains your conversation
Prompts, file contents and tool results all appear in the log. Read a trace
before you share it.

Secrets are stripped automatically. Every line passes one redaction step on
its way to disk. It replaces API keys, bearer tokens, OAuth codes and JWTs
with `<redacted>` and keeps the surrounding structure:

```
{"model":"gpt-5.6-luna","api_key":"<redacted>","stream":true}
```

No subsystem can bypass it. `agentty --version` and `agentty diagnostics` both
report how many secrets have been removed so far.
:::

### Same model, different provider {#model-heterogeneity}

Every turn opens with a fingerprint on the `model` channel:

```
D model dispatch.turn: route=copilot provider=copilot model=sol-5.6 effort=low
        tools=24 json_protocol=0 show_reasoning=1 ctx_window=128000
        max_tokens=16384 retry=0
```

It closes with `stream.end`, which says how the turn ended (`end=clean_close`, `cancelled`, `http_error`, `transport_error` or `already_terminated`), plus the HTTP status and stop reason. Between them:

| You observe | The log says | Meaning |
|---|---|---|
| "model ignores tools" | `salvage.tool_call` | The model leaked tool JSON into its text and agentty recovered it. Its native tool grammar is broken or disabled on this host |
| "tool failed: invalid args" | `responses.tool_args_unroutable` | Args arrived but couldn't be attached. An adapter bug: report it |
| "tool failed: invalid args" | no unroutable line, `tool.exec` shows empty args | The model never sent arguments. A model bug |
| "reasoning not showing" | `…result: … thinking_deltas=0` | The server sent no reasoning text (model or tier) |
| "reasoning not showing" | `…result: … thinking_deltas=214` | Reasoning arrived and was lost client-side: report it |
| "effort setting seems ignored" | `caps.effort_learned: key=… effort high -> medium` | The provider rejected that effort once; agentty learned and clamped it (persisted) |
| "model went mute mid-turn" | `*.frame_unparseable` / `*.unknown_event` | A frame agentty couldn't parse or doesn't know: report it with the log |
| "no models in the picker" | `copilot.models.fallback: reason=chat_disabled` | Org policy or entitlement, not an agentty bug |

To capture all of it: `AGENTTY_LOG=model=debug,wire=debug`. Salvage,
learned-caps and fallback events are `warn` or higher, so a plain
`agentty diagnostics` usually has the answer already.

## Worked examples {#worked-examples}

**"The model ignores my tools" / `invalid args, pattern required`.** Tools
that need arguments fail while zero-argument tools (`repo_map`, `list`) work.
That pattern means arguments arrive empty.

```bash
AGENTTY_LOG=wire=trace agentty
grep 'tool.exec' ~/.agentty/logs/agentty.log
# W tool  tool.exec: name=grep ms=0 ok=0 err=invalid args: pattern required args={}
grep 'wire.chunk\|request.body' ~/.agentty/logs/agentty.log | grep -i 'arguments'
```

`args={}` means decoding failed. The second grep shows what the server sent.

**"It just hangs for a second."** Look at the `+…ms` column. A gap between two
adjacent lines is the stall.

```bash
grep ' wire \| net ' ~/.agentty/logs/agentty.log | tail -40
# W net  http.connect_failed: host=api.example.com:443 attempt=1/3 err=[connect] timeout
```

`attempt=1/3` shows the retry count; the time deltas show what each attempt cost.

**"My custom host returns no models."**

```bash
grep 'provider.select\|models.loaded\|http\.' ~/.agentty/logs/agentty.log
# W wire  provider.select: provider=localhost:1 kind=openai endpoint=localhost/v1/chat/completions
# W net   http.local_failed: host=localhost:1 err=[connect] connect: poll hangup
```

`provider.select` shows the endpoint agentty actually dialled. A missing `/v1`
or a wrong port is usually visible right there.

**"A plugin's tools are missing."**

```bash
grep 'mcp.connect' ~/.agentty/logs/agentty.log
# W mcp  mcp.connect: server=date result=spawn_failed
# I mcp  mcp.connect: server=wordcount result=ok tools=2 resources=0 prompts=0
```

## The flight recorder {#flight-recorder}

Even with file logging off, agentty keeps the last 256 events at `warn` or
higher in memory. If any channel is set below `warn`, those events go into the
ring too. On a crash (SIGSEGV / SIGABRT) the handler prints the ring to stderr
after the backtrace:

```
=== agentty: SIGSEGV (segmentation fault) ===
  <backtrace frames>
=== agentty flight recorder (last events, oldest first) ===
2026-08-28T01:23:44.101 +0012310ms 1a2b W wire  openai.stream: retry 3/6 …
2026-08-28T01:23:45.678 +0012345ms 1a2b E persist save_thread: disk full
==================
```

Capture it with `agentty 2> crash.log`.

## For developers {#developers}

A non-release build (the `dev` preset) logs `trace` on every channel with no
variable set. Run `./build/agentty` and everything appends to
`~/.agentty/logs/agentty.log`. Concurrent sessions interleave line by line,
and a crash never loses the tail.

Mark the moment you see a bug. `SIGUSR1` writes a `=== MARK (SIGUSR1) ===`
banner and a flight-recorder snapshot into the live log. It is safe to send at
any time, including mid-stream.

```bash
kill -USR1 $(pgrep agentty)
grep -n "MARK (SIGUSR1)" ~/.agentty/logs/agentty.log
```

Other habits:

```bash
# snapshot the tail before later trace output buries it
tail -c 512k ~/.agentty/logs/agentty.log > ~/bug-$(date +%H%M%S).log
# watch warnings, errors and turn fingerprints while you reproduce
tail -f ~/.agentty/logs/agentty.log | grep --line-buffered 'W \|E \|dispatch.turn'
```

## For contributors: log-site rules {#contributors}

In provider/model code, never drop wire data or degrade a capability silently.

1. Dropping a frame you can't parse: `Warn` on `Wire` with the error and the
   head bytes (`*.frame_unparseable`).
2. Ignoring an unknown event type: `Debug` on `Wire` naming it
   (`*.unknown_event`).
3. Making a capability decision (clamping effort, disabling tools, switching
   dialect, salvage): `Debug` on `Model`, or `Warn` if it is learned and
   persisted (`caps.effort_learned`).
4. Falling back (bundled catalog, degraded session, negative cache): `Warn`
   with the reason.
5. Swallowing an exception: `util::dbglog(where, e.what())`. Site prefixes
   route to channels (`caps.`/`salvage.`/`dispatch.` → `model`,
   `oauth.`/`token.` → `auth`; see `src/util/dbglog.cpp`).

Per-frame paths log at `trace` only; `AGT_LOG_HOT` won't compile at any other
level. Never log secrets: redaction is a safety net.

## Cost {#cost}

No log site is compiled out of release builds. A disabled site is one relaxed
atomic load checked before its arguments are evaluated, so
`AGT_LOG(..., "{}", expensive())` never calls `expensive()`. Format strings are
checked at compile time.

Measured at `-O2 -DNDEBUG`:

| | Cost |
|---|---|
| A site that doesn't fire | about 0.5 ns |
| A site that fires | about 1.3 µs (format, redact, one `write(2)`) |
| A healthy release session, no variable set | about 200 bytes |

## Retired variables {#retired-variables}

Only `AGENTTY_LOG` and `AGENTTY_LOG_BODIES` control what is captured. Where it
goes is the `--log-file` flag. These older variables were removed:

| Was | Now |
|-----|-----|
| `AGENTTY_LOG_FILE=<path>` | `--log-file <path>` |
| `AGENTTY_DEBUG_LOG=<path>` | `--log-file <path>` plus `AGENTTY_LOG=debug` |
| `AGENTTY_DEBUG_API`, `AGENTTY_DEBUG_FILE` | the `wire` channel |
| `AGENTTY_ACP_TRACE` | the `acp` channel |
| `AGENTTY_CACHE_PROF`, `AGENTTY_LOAD_PROF`, `AGENTTY_STREAM_PROF`, `AGENTTY_VIEW_PROF` | the `perf` channel (`AGENTTY_LOG=perf=debug`, or `perf=trace` for per-frame pacing) |
