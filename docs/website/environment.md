---
title: Environment variables
description: Every environment variable agentty reads — what it does, what it defaults to, and when you'd actually reach for it.
nav_section: Advanced
nav_order: 56
slug: environment
---

This page lists every environment variable agentty reads. Most people never
set any of them: agentty runs fine with none.

Use them for one-off overrides, debugging and scripted runs. For lasting
preferences, use [settings](/docs/configuration) ([[Ctrl+S]]) instead.

## Commonly used {#common}

| Variable | What it does |
|----------|--------------|
| `AGENTTY_HOME` | Moves all agentty state. Default `~/.agentty`. |
| `AGENTTY_LOG` | Sets how much goes to the log, e.g. `debug` or `warn,wire=trace`. |
| `ANTHROPIC_API_KEY`, `OPENAI_API_KEY`, ... | Provider API keys. See [Providers & models](#providers--models). |
| `AGENTTY_OLLAMA_HOST` | Where your Ollama server is. Default `http://localhost:11434`. |
| `AGENTTY_POOL_IDLE_TTL` | Lower it if answers fail with `h2 socket hangup`. |
| `AGENTTY_CODEX_CLIENT_VERSION` | Set a newer version if the ChatGPT model list is empty. |

The full list follows, grouped by what each variable affects.

:::tip Debugging something right now?
Start with [`AGENTTY_LOG`](#diagnostics). In a non-release build everything is
already logged to `~/.agentty/logs/agentty.log` with no variable set at all.
:::

## Diagnostics

One log, one switch. See **[Logging & diagnostics](/docs/logging)** for the
channel list and line format.

| Variable | Effect |
|----------|--------|
| `AGENTTY_LOG` | The diagnostic filter. `trace` · `debug` · `info` · `warn` · `error` · `off`, plus `channel=level` overrides (`warn,wire=trace`). **Release builds default to `warn`** (~2 lines per healthy session) so `agentty diagnostics` works with no setup; non-release builds default to everything. |
| `AGENTTY_LOG_BODIES` | Include payloads (wire bodies, tool args) in the log. Off by default — these carry user data. |
| `AGENTTY_TRACE_TOOLS` | Older spelling of `--events jsonl`; `=1` selects the same stream. Prefer the flag — it shows up in `--help`. |
| `AGENTTY_RAG_TRACE` | Fold rag-cpp's per-stage trace into the retrieval `mode` string shown on the tool card. On by default; `=0` disables. |
| `AGENTTY_NO_STDERR_REDIRECT` | Keep subsystem stderr (MCP servers, sandbox) on the terminal instead of capturing it. Debug-only — it will scribble over the TUI. |
| `AGENTTY_STARTUP_TRACE` | A file path. Each startup stage appends `+12.3 ms  name` to it, timed from launch, so you can see where launch time goes. Unset: no cost. |
| `MIMALLOC_PURGE_DELAY` | mimalloc's own knob. agentty sets it to `0` (give freed memory back to the OS at once) unless you set it. |

## Providers & models

| Variable | Effect |
|----------|--------|
| `ANTHROPIC_API_KEY` · `OPENAI_API_KEY` · `GROQ_API_KEY` · `OPENROUTER_API_KEY` · `TOGETHER_API_KEY` · `CEREBRAS_API_KEY` · `DEEPSEEK_API_KEY` · `XAI_API_KEY` · `MISTRAL_API_KEY` · `GEMINI_API_KEY` / `GOOGLE_API_KEY` · `FIREWORKS_API_KEY` | Per-provider API keys. Each provider also falls back to `OPENAI_API_KEY` where its wire is OpenAI-compatible. Keys saved via `agentty login` take precedence. |
| `CLAUDE_CODE_OAUTH_TOKEN` | Use an existing Claude Code OAuth token instead of signing in. |
| `AGENTTY_API_HOST` / `AGENTTY_OAUTH_HOST` | Point the Anthropic wire at a different API / OAuth host (gateways, enterprise proxies). |
| `AGENTTY_OLLAMA_HOST` | Ollama base URL. Default `http://localhost:11434`. |
| `AGENTTY_OLLAMA_NUM_CTX` · `AGENTTY_OLLAMA_NUM_PREDICT` · `AGENTTY_OLLAMA_TEMPERATURE` | Override Ollama's context window, output cap, and sampling temperature. Otherwise agentty picks per model. |
| `AGENTTY_MAX_OUTPUT_TOKENS` | Cap `max_tokens` on every request (mirrors Claude Code's knob). Useful against a provider that rejects large output budgets. |
| `AGENTTY_MAX_SKILLS` | Override the tier-1 skill catalog cap (default 64, clamped to 8–4096). Lower it for small-context models, raise it for large skill libraries. |
| `AGENTTY_FORCE_EFFORT` | Force a reasoning-effort tier for every turn, overriding what the model's capabilities imply. For A/B-ing effort, not everyday use. |
| `AGENTTY_CHATGPT_DEVICE_AUTH` | Use the device-code flow for ChatGPT sign-in instead of the loopback browser flow — for headless or remote machines. |
| `AGENTTY_CODEX_CLIENT_VERSION` | The Codex CLI version agentty reports to ChatGPT, as `x.y.z`. The server sends fewer models (or none) to versions it thinks are old. If the ChatGPT picker is empty, set a newer version. Other formats are ignored. |
| `AGENTTY_NO_PREWARM` | Skip the TLS/connection prewarm at startup. |
| `AGENTTY_POOL_IDLE_TTL` | How many seconds an idle HTTP/2 connection may be reused. Default 90, or 450 behind a SOCKS proxy; `0` turns pooling off. **If answers fail with `h2 socket hangup (server accepted request; not replayed automatically)`, lower it.** That error means a proxy, VPN or CDN closed the connection first. Set this below your network's idle timeout and agentty re-dials quietly instead. `grep http.stream_hangup` in the log shows the idle age of each hangup. |
| `AGENTTY_FORCE_REMOTE` | No effect since 0.9.42: there is no separate remote cadence any more. Still accepted. |

## Smart Mode

Off by default and a byte-for-byte no-op when off — see
**[Smart Mode](/docs/smart-mode)**.

| Variable | Effect |
|----------|--------|
| `AGENTTY_SMART_MODE` | Session pin for the master switch: `1` on, `0` off, for this process only. Never persisted (safe for CI). |
| `AGENTTY_SMART_NO_INTERNAL` | Keep engine-internal calls (compaction summary, thread title) on the main model instead of the Utility slot. |
| `AGENTTY_SMART_NO_SUBAGENTS` · `AGENTTY_SMART_NO_ORCHESTRATE` | Disable subagent delegation / orchestration independently. For bisecting a routing problem. |
| `AGENTTY_SMART_COMPLEX_THRESHOLD` · `AGENTTY_SMART_DEEP_MARGIN` · `AGENTTY_SMART_BIAS_CLAMP` | Router tuning: complexity cut-off, deep-work margin, and the clamp on learned bias. Defaults are measured; change them only with a benchmark. |
| `AGENTTY_SMART_ROUTE_MAIN` | On by default. `0` stops routing the main turn by complexity: every turn runs on the Strategic model and only its *effort* changes. |
| `AGENTTY_SMART_MAIN_FLOOR` | The cheapest role the main turn may be routed to: `utility` (default — the full ladder), `implementation` (never cheaper than the mid model), or `strategic` (never route down; equivalent to `AGENTTY_SMART_ROUTE_MAIN=0`). Raise it if simple turns are being answered too thinly. |

## Context window

agentty finds a model's context window in three places, most specific first:
**your override**, what the provider **advertised**, and what the **model id**
implies. If none of them knows, it uses 200k.

Most setups need nothing here, because the `/v1/models` listing is read for `model_info.max_input_tokens` (LiteLLM),
`context_length` and `top_provider.context_length` (OpenRouter),
`max_model_len` (vLLM), and `n_ctx` (llama.cpp). Where a row advertises both
a catalog window and the serving endpoint's, the **serving** one wins, since it
is the one that rejects an over-long prompt.

Some gateways report no window at all (OpenAI's own `/v1/models` has only
`{id, object, created, owned_by}`). Then the picker shows `auto` and the turn
uses the default. For that case:

> | Variable | Effect |
> |---|---|
> | `AGENTTY_MAX_CONTEXT_TOKENS` | The window to assume when nothing else knows one. Spelled to mirror Claude Code's `CLAUDE_CODE_MAX_CONTEXT_TOKENS`, so it is guessable if you have hit this before. |

It is checked **last**, after the catalog, the probe and the model id. It is
global, so it only applies when nothing better is known. In practice you need
it only for a private gateway serving a model id no catalog lists.

## Retrieval (RAG)

Full detail in **[Retrieval](/docs/retrieval)**. Nothing here is needed: with
Ollama running and `qwen3-embedding:0.6b` or `nomic-embed-text` pulled, search
by meaning turns on by itself. `agentty diagnostics` shows what is in use.

| Variable | Effect |
|----------|--------|
| `AGENTTY_DOCS_DIR` | Folder `search_docs` indexes. Default `./docs`, then `./.agentty/knowledge`. Skills and memory are searched either way. |
| `AGENTTY_EMBED_BACKEND` | `auto` (default: a local Ollama if found), `ollama`, `openai` (any OpenAI-compatible endpoint), `llamacpp`, `gguf`, `onnx`, or `off` (keyword only). |
| `AGENTTY_EMBED_MODEL` | Embedding model. On `auto`, agentty uses `qwen3-embedding:0.6b` when it's pulled, else `nomic-embed-text`. |
| `AGENTTY_EMBED_ENDPOINT` · `AGENTTY_EMBED_PATH` · `AGENTTY_EMBED_API_KEY` | Where a non-Ollama embedder lives (`https://host:port`), its request path, and its key. |
| `AGENTTY_EMBED_MODEL_PATH` · `AGENTTY_EMBED_TOKENIZER` | Model file (and ONNX tokenizer) for the in-process backends. |
| `AGENTTY_EMBED_PREFIX` | `0` stops agentty adding the query/document prefixes some models were trained with (nomic, e5, mxbai, arctic). |
| `AGENTTY_RAG_PERSIST` | Keep indexes on disk so restarts are warm and edits update in place. On by default; `=0` disables. |
| `AGENTTY_RAG_LEARN` | Nudge ranking toward passages you go on to open. Off by default. |
| `AGENTTY_RAG_PROACTIVE` · `AGENTTY_RAG_PROACTIVE_MIN` · `AGENTTY_RAG_PROACTIVE_BYTES` | Pre-turn auto-retrieval and its thresholds. Off by default; persist it in `/settings` → *Proactive retrieval (RAG)*. |
| `AGENTTY_RAG_EXPAND` · `AGENTTY_RAG_HYDE` · `AGENTTY_RAG_GEN_MODEL` | LLM-assisted recall (multi-query, HyDE) via a small local model. Opt-in. |
| `AGENTTY_RAG_PRF` · `AGENTTY_RAG_GRAPH` · `AGENTTY_RAG_CORRECT` | Pseudo-relevance feedback, GraphRAG, corrective retry. Opt-in. |
| `AGENTTY_RAG_BM25_WEIGHT` · `AGENTTY_RAG_DENSE_WEIGHT` · `AGENTTY_RAG_FUSION` · `AGENTTY_RAG_ADAPTIVE` | Hybrid weighting and fusion: `convex` (default, adapts per query) or `rrf` (uses the two weights). |
| `AGENTTY_RAG_MMR` · `AGENTTY_RAG_MMR_LAMBDA` · `AGENTTY_RAG_DEDUP` · `AGENTTY_RAG_DEDUP_THRESHOLD` | Diversity and near-duplicate suppression in results. Both on by default. |
| `AGENTTY_RAG_CODE_RERANK` · `AGENTTY_RAG_SPLIT_IDENT` | `search_code`'s code-aware rerank and identifier splitting. On by default; `=0` turns one off (for A/B tests). |
| `AGENTTY_RAG_AUTOCUT` · `AGENTTY_RAG_AUTOCUT_SENSITIVITY` · `AGENTTY_RAG_RELEVANCE_FLOOR` | Drop weak tail results automatically. On by default. |
| `AGENTTY_RAG_CONTEXTUAL` · `AGENTTY_RAG_EXTRACTIVE` · `AGENTTY_RAG_STITCH` · `AGENTTY_RAG_CORRECT` · `AGENTTY_RAG_PRF` · `AGENTTY_RAG_GRAPH` | Individual retrieval stages (contextual headers, extractive compression, chunk stitching, correction, pseudo-relevance feedback, graph expansion). |
| `AGENTTY_RAG_MEMORY` · `AGENTTY_RAG_SKILLS` · `AGENTTY_RAG_MCP` | Include learned memory / skills / MCP resources in the index. |
| `AGENTTY_RAG_OUTPUT_BYTES` · `AGENTTY_RAG_BUDGET_GAMMA` · `AGENTTY_RAG_CONF_BUDGET_FLOOR` | Output size budget and how it scales with confidence. |
| `AGENTTY_RAG_MEASURE` | Emit retrieval quality metrics — for benchmarking. |

## Tools, MCP & ACP

| Variable | Effect |
|----------|--------|
| `AGENTTY_MCP_CONFIG` | Path to the MCP server config. Otherwise `~/.agentty` then `./.agentty`. |
| `AGENTTY_MCP_ALLOW_PROJECT` | Allow a project-local `.agentty/mcp.json` to add servers. Off by default — a repo you clone should not silently gain tool servers. |
| `AGENTTY_MCP_TIMEOUT_MS` · `AGENTTY_MCP_CONNECT_TIMEOUT_MS` | Per-call and initial-connect timeouts for MCP servers. |
| `AGENTTY_MCP_TOOL_BUDGET` | Most tools (built-in plus plugin) one request loads. Past it, plugin tools load on demand through `tool_search`; none are hidden. Default is the provider's limit: 128 on OpenAI-style providers, none on Anthropic. `0` removes the cap; bad values keep the default. |
| `AGENTTY_MCP_CLIENT_ID` | OAuth client id for MCP servers that require one. |
| `AGENTTY_SUBAGENT_MAX_SECONDS` | Time limit in seconds for one `task` subagent run. Default 900 (15 min); values outside 1–86399 keep the default. It catches a backend that keeps a stream open without finishing. When time runs out, the subagent reports what it has, marked incomplete. For `agentty run`, `--timeout` overrides it. |
| `AGENTTY_ACP_AGENTS` | Path to the ACP agent config (same precedence chain as MCP). |
| `AGENTTY_ACP_ALLOW_PROJECT` | Allow a project-local ACP agent config. Off by default, same reasoning as MCP. |
| `AGENTTY_NO_HOOKS` | Disable all lifecycle hooks for this run. |
| `AGENTTY_HOOK_EVENT` · `AGENTTY_HOOK_TOOL` · `AGENTTY_HOOK_PAYLOAD_FILE` | **Set by agentty for your hook**, not by you — the event name, tool name, and a file holding the JSON payload. See **[Hooks](/docs/hooks)**. |

## Security & storage

| Variable | Effect |
|----------|--------|
| `AGENTTY_HOME` | Root for all agentty state (credentials, threads, settings, logs). Default `~/.agentty`. Point it at a scratch dir to sandbox a session. |
| `AGENTTY_THREADS_DIR` | Conversation history. Default `<root>/threads`. Kept 0700. |
| `AGENTTY_CREDENTIALS_DIR` | Sign-ins and API keys. Default `<root>/credentials`. Kept 0700. |
| `AGENTTY_STATE_DIR` | Approval hashes for project MCP servers, hooks and skills. Default `<root>/state`. |
| `AGENTTY_CACHE_DIR` | Refetchable data, the model catalog and update stamps. Default `<root>/cache`. Safe to delete. |
| `AGENTTY_LOGS_DIR` | Diagnostic logs. Default `<root>/logs`. |
| `AGENTTY_PROJECT_DIR` | The per-project root. Default `<project>/.agentty`. Each project gets its own subfolder under it. |
| `AGENTTY_RAG_DIR` | Search indexes. Default `<project root>/cache`. Safe to delete, the next search rebuilds. |
| `AGENTTY_PROJECT_STATE_DIR` | Search feedback. Default `<project root>/state`. |
| `AGENTTY_THREADS_KEEP_DAYS` | Delete threads idle this many days, once a day. Unset keeps everything. |
| `AGENTTY_ENCRYPT_PASSPHRASE` / `AGENTTY_PASSPHRASE` | Passphrase for the encrypted credential store — for headless machines where no prompt is possible. |
| `AGENTTY_USE_KEYSTORE` | Use the OS keychain for credentials instead of the file store. |
| `AGENTTY_KDF` | Key-derivation parameters for the credential store. Change only if you know why. |
| `AGENTTY_TLS_PINS` | Pin TLS certificates for provider hosts. |
| `AGENTTY_INSECURE` | **Disable TLS verification.** For debugging a proxy with a self-signed cert. Never leave it set. |
| `AGENTTY_SOCKS_PROXY` | Route provider traffic through a SOCKS proxy. See **[Proxies](/docs/proxies)**. |
| `SSL_CERT_FILE` · `SSL_CERT_DIR` · `CURL_CA_BUNDLE` | Standard OpenSSL root-store overrides. |
| `AGENTTY_AIRGAP_SSH` | SSH target for airgap mode. See **[Airgap](/docs/airgap)**. |
| `AGENTTY_NO_SSH_THROTTLE` | No effect since 0.9.42: SSH no longer changes how agentty renders or animates, so there is nothing to turn off. Still accepted. |
| `AGENTTY_NO_UPDATE_CHECK` | Disable the background update check (also implied by airgap mode). |

### Keeping config local and bulk elsewhere

`AGENTTY_HOME` moves everything. The per-directory variables move one thing
each, usually the parts that grow:

```sh
export AGENTTY_THREADS_DIR=/mnt/data/agentty/threads
export AGENTTY_LOGS_DIR=/mnt/data/agentty/logs
```

Use `agentty config move threads /mnt/data/agentty/threads` to bring
existing data along; it prints the export line to add.

See [Storage](/docs/storage) for every file and how long it stays.

### Retrieval indexes are per-project

`AGENTTY_RAG_DIR` works differently. A retrieval index belongs to the source
tree that built it and is useless anywhere else. So indexes live in
`<project>/.agentty`, not under `AGENTTY_HOME`. Only `AGENTTY_RAG_DIR` and
`AGENTTY_PROJECT_DIR` move them.

They are the largest thing agentty writes per project. On the agentty repo
itself they take 31 MB (code) and 7 MB (docs). If your code is on a roomy
disk, there is nothing to do. If not:

```sh
export AGENTTY_RAG_DIR=/mnt/data/agentty/rag
```

With this set, each project gets its own subfolder (`myrepo-04b42962/`), so
two checkouts never share one file.

The "project" is the nearest parent directory that contains `.git`,
`.agentty`, `.hg` or `.svn`, **not** the directory you started in. So
`cd src/ && agentty` reuses the index at the repo root.

#### Superseded indexes are reclaimed

Each index filename includes a tag for the embedding backend that built it
(`rag_code.e45ba1de.ragdb`). This stops agentty from mixing vectors from
different backends, but each switch adds a new file.

After writing an index, agentty deletes old variants. It keeps the current
one and the most recent previous one, so switching back and forth between two
backends doesn't force a full rebuild. Old untagged indexes are deleted, since
no current build can read them.

Two safety limits: variants less than an hour old are left alone, because
another agentty process may be writing them. And if any file in the folder
can't be checked, nothing is deleted.

Details worth knowing:

- A **relative** value is resolved against the root, not your current
  directory — `AGENTTY_LOGS_DIR=logs2` always means `~/.agentty/logs2`.
- An **empty** value counts as unset, so an exported-but-blank variable
  relocates nothing.
- If a directory **can't be created** (unmounted drive, a file in the way),
  agentty prints one warning and falls back to the default rather than losing
  the data silently.
- An overridden threads directory is forced to `0700`, because conversation
  history is as sensitive as the credentials it used to sit beside.

Symlinking the subdirectories works too, and predates these variables:
`ln -s /mnt/data/agentty-logs ~/.agentty/logs`.

## Paste

Each overrides one row of Settings → Paste for this run (it wins over
`settings.json`, locks the row in the pane, and is never saved). Full table with
defaults: **[Configuration → Paste](/docs/configuration#paste)**.

| Variable | Effect |
|----------|--------|
| `AGENTTY_PASTE_FORWARDS` | `0` stops reading the laptop's clipboard through a forwarded agent over ssh. |
| `AGENTTY_PASTE_DISPLAY` | `0` stops reading this machine's own clipboard. |
| `AGENTTY_PASTE_TERMINAL` | `auto` · `osc5522` · `osc52` · `off`: how (or whether) to ask the terminal for the clipboard. |
| `AGENTTY_PASTE_SERVER_CLIPBOARD` | `1`: over ssh, fall back to the server's own clipboard. |
| `AGENTTY_PASTE_DECODE_TEXT` | `0`: paste `richpaste:1:` lines as text instead of the image or file they carry. |
| `AGENTTY_PASTE_MAX_MB` | Largest paste read, in MB. |
| `AGENTTY_PASTE_INLINE_KB` | A copied text file up to this many KB pastes as its text; `0` always saves it. |
| `AGENTTY_PASTE_SAVE_DIR` | Where pasted files are saved. |
| `AGENTTY_PASTE_SOCKET` | Ask only this agent socket. |

## Interface

| Variable | Effect |
|----------|--------|
| `AGENTTY_GRID` | Grid-protocol rendering for cooperating hosts. `=0` opts out. (`AGENTTY_EMACS_GRID` is the legacy name.) |
| `AGENTTY_HOST` | Declare the cooperating host explicitly rather than autodetecting. |
| `AGENTTY_PAINTED_CARET` | `=1` draws the caret manually — for terminals with broken cursor-visibility handling. |
| `AGENTTY_REVEAL` | Master switch for the streaming reveal effect. **On by default everywhere, tmux included**; `0`/`false`/`off`/`no` turns it off, anything else forces it on. Set it to `0` if your terminal/multiplexer combination ghosts the live tail. |
| `AGENTTY_REVEAL_TYPEWRITER` | The typewriter clip alone — text arriving character-by-character. ANDed with `AGENTTY_REVEAL`; same default and same truthiness rule. |
| `AGENTTY_REVEAL_DECORATE` | The decorative overlay on the revealing tail alone. ANDed with `AGENTTY_REVEAL`; same default and same truthiness rule. |
| `AGENTTY_NO_REVEAL_GLIDE` | `=1` disables the bounded **end-of-turn glide** (the visible catch-up when a turn finishes) and restores the immediate finish. Not the reveal animation itself — that's `AGENTTY_REVEAL`. The glide needs synchronized-output support; it runs over SSH too. |
| `AGENTTY_REPLAY_ROWS` | Rows of history redrawn into scrollback on open and resize, overriding Appearance → **Scrollback**. `0` turns the redraw off. |
| `AGENTTY_FROZEN_COLLAPSE` | `=1` opts **in** to collapsing frozen turns. Off by default. |
| `AGENTTY_NO_TRANSFORMS` | Disable output transforms. |

## Development & testing

Not for normal use. Listed so a reader of the source isn't left guessing.

| Variable | Effect |
|----------|--------|
| `AGENTTY_STRICT_TEST_ROOT` · `AGENTTY_TEST_FAKE_PASSWD_HOME` | Test-harness isolation: enforce a sandboxed user root and fake the passwd home lookup. |
| `AGENTTY_UNDER_TEST` | Set by the test mains before anything renders: arms the user-root tripwire so a test can never touch the real `~/.agentty`. |

## Variables agentty *reads* from your environment

Standard variables consulted for platform behaviour, listed for completeness:
`HOME` / `USERPROFILE` / `HOMEDRIVE` + `HOMEPATH` · `XDG_CONFIG_HOME` ·
`APPDATA` · `PATH` · `USER` / `USERNAME` · `SUDO_USER` · `HOSTNAME` ·
`COLUMNS` / `LINES` · `DISPLAY` / `WAYLAND_DISPLAY` / `XDG_SESSION_TYPE` ·
`SSH_CONNECTION` / `SSH_CLIENT` / `SSH_TTY` / `SSH_AUTH_SOCK` ·
`MOSH_CONNECTION` / `MOSH_KEY` / `MOSH_SERVER_PID` · `INSIDE_EMACS`.

Other tools' locations, so their files are found where they are:

| Variable | Used for |
|----------|----------|
| `CODEX_HOME` | Where Codex keeps its sessions, `AGENTS.md` and `config.toml`. Default `~/.codex`. See [Coming from Codex or Claude Code](/docs/switching). |
| `CLAUDE_CONFIG_DIR` | Where Claude Code keeps its sessions (`projects/`) and personal `CLAUDE.md`. Default `~/.claude`. |
