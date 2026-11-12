---
title: CLI Reference
description: Every agentty subcommand and flag.
nav_section: User Manual
nav_order: 60
slug: cli
---

This page lists every agentty subcommand and flag. Start with the five commands below. The tables after them are the full reference.

## The five commands most people use

```bash
agentty                                # start in the current folder
agentty -m claude-opus-4-5             # pick a model
agentty --provider openai -m gpt-4o    # use another backend
agentty run "add a --dry-run flag"     # one prompt, answer on stdout, no UI
agentty login                          # sign in (works over SSH)
agentty --sandbox off                  # turn off the OS sandbox for bash
```

More examples:

```bash
agentty -w ~/code/project              # work in another folder
agentty --provider kimi                # sign in with Kimi (no API key)
agentty --provider deepseek -m deepseek-v4-pro   # DeepSeek (DEEPSEEK_API_KEY)
agentty --provider ollama              # local model, no key, no cloud
agentty -k sk-ant-…                    # key for this session only, never saved
agentty --sandbox on                   # refuse to run bash without a sandbox
```

## Subcommands

| Command | What it does |
|---|---|
| `agentty` | Start an interactive thread in the workspace. |
| `agentty login` | Sign in without the UI. Works over SSH. |
| `agentty logout` | Delete saved credentials. |
| `agentty status` | Show which auth source agentty will use. |
| `agentty run [prompt]` | Run one prompt with tools and print only the answer. For scripts and CI. See [below](#agentty-run). |
| `agentty airgap user@host` | Run the agent on a remote host through an SSH SOCKS tunnel. See [Air-gap flags](#air-gap-flags). |
| `agentty acp` | Run headless as an [Agent Client Protocol](/docs/acp) agent for Zed (JSON-RPC over stdio). |
| `agentty sessions` | List Codex and Claude Code sessions agentty can continue, newest first. `--dump N` shows how session `N` reads as an agentty thread. Open them with [[Ctrl+J]]. See [Coming from Codex or Claude Code](/docs/switching). |
| `agentty paste …` | Clipboard tools. See [agentty paste](#agentty-paste). |
| `agentty mcp-serve` | Serve agentty's native tools over [MCP](/docs/mcp) (stdio). Point any MCP client at it. |
| `agentty mcp-login <server> [--metadata <url>] [--client-id <id>]` | Authorize an OAuth-gated MCP server from `mcp.json` (OAuth 2.1 + PKCE in your browser). `--metadata` and `--client-id` override discovery. |
| `agentty mcp-logout <server>` | Delete a stored MCP server token. |
| `agentty mcp-status` | List configured MCP servers and whether each is authorized. |
| `agentty mcp import [--from junie\|claude\|cursor\|vscode\|codex]` | Copy another tool's MCP servers into `~/.agentty/mcp.json`. With no `--from`, list what each tool offers. `--dry-run` (or `-n`) shows the change; `--force` replaces a server agentty already has. See [MCP](/docs/mcp#importing-from-other-tools). |
| `agentty plugin add\|list\|remove\|approve` | Manage [plugins](/docs/plugins). A plugin is an MCP server in any language. Example: `agentty plugin add today --python today.py`. Other sources: `--uvx pkg`, `--npx pkg`, `--http <url>`, `-- cmd args`. `--project` targets the repo config. Plugins never run until you approve them. |
| `agentty skill add\|list\|remove\|approve` | Manage installed [Agent Skills](/docs/skills). |
| `agentty skills` | List discovered [Agent Skills](/docs/skills) with spec-lint warnings. Exits 1 on warnings, so CI can use it. |
| `agentty hooks [list]` | Show configured lifecycle [hooks](/docs/hooks) and whether they are approved. |
| `agentty hooks approve` | Inspect and approve the active hooks file. Hooks never run unapproved; any change needs approval again. |
| `agentty diagnostics` | Write a redacted bug-report bundle (build info, provider, log tail) to `~/.agentty/logs/agentty-diagnostics.txt`. See [Logging](/docs/logging#reporting-a-bug). |
| `agentty config` | Show every folder agentty reads or writes, where it is, and its size. `agentty config <name>` (e.g. `threads`, `mcp`) shows one. See [Storage](/docs/storage). |
| `agentty config env` | Show every storage variable and its value. |
| `agentty config move <dir> <path>` | Move a folder's data, then print the `export` line to add. It refuses while agentty runs and checks the copy. `default` moves it back. |
| `agentty config doctor` | Check that folders are usable, secrets are private, and storage variables are valid. Exits 1 if something needs fixing. |
| `agentty config clean [--yes]` | List files that older versions left behind. `--yes` deletes them. |
| `agentty rag-bench [dir]` | Benchmark [`search_docs` retrieval](/docs/retrieval#measuring-retrieval) on your own files: recall@k, MRR and nDCG per pipeline stage. Uses the auto-found docs folder unless you pass a directory. |
| `agentty rag-eval <cases.json>` | Run a judged evaluation of `search_code` against your cases file. |
| `agentty update [--check]` | Update agentty to the latest release. `--check` only reports whether an update exists. |
| `agentty version`, `--version`, `-V` | Print `agentty <version>` and exit. |
| `agentty help`, `--help`, `-h` | Print usage and exit. |

## Global flags

These match `agentty --help`, which prints:

```run
agentty --help
```

| Flag | Effect |
|---|---|
| `-m`, `--model <id>` | Model id for the session, e.g. `claude-opus-4-5`. Saved for next time. |
| `--provider <p>` | LLM backend. `anthropic` is the default. Other choices are OpenAI-compatible: `openai` · `chatgpt` · `codex` · `copilot` · `kimi` · `deepseek` · `gemini` · `xai` · `mistral` · `groq` · `openrouter` · `together` · `cerebras` · `fireworks` · `ollama` · `llama.cpp`. You can also pass a raw `host:port`, or a full URL `https://host/path` for a server with a path prefix. Saved like `-m`. Switch live with `^P`. See [Providers & Models](/docs/providers). |
| `-k`, `--key <key>` | API key for this session only. Never written to disk. |
| `--auth-header <name>` | For OpenAI-compatible gateways that don't accept `Authorization: Bearer` (e.g. `X-API-Key`). The key from `-k` or `OPENAI_API_KEY` is sent raw under this header name. This session only. Default: Bearer. |
| `-w`, `--workspace <dir>` | Limit file tools to this folder (default: the current folder). Tools refuse paths outside it. `--workspace /` turns the limit off. |
| `--sandbox <mode>` | Run `bash` and `diagnostics` inside an OS sandbox (claybin; macOS falls back to sandbox-exec). `auto` (default) uses it if available. `on` refuses to run without it. `off` turns it off. |
| `--sandbox-backend <b>` | Sandbox engine. Only `claybin` is accepted; `bwrap` is kept as an alias and runs claybin. |
| `-p`, `--profile <mode>` | ACP permission tier (Zed shows the prompts). `ask` (default) prompts for write, exec and network. `minimal` also prompts for reads. `write` never prompts. |
| `--lang <code>` | UI language for this run. Beats `ui.lang` in settings.json and the `LC_ALL` / `LC_MESSAGES` / `LANGUAGE` / `LANG` variables. Falls back to English. |
| `--log-file <path>` | Write the diagnostic log here instead of `~/.agentty/logs/agentty.log` (or `$AGENTTY_LOGS_DIR`). `AGENTTY_LOG` sets what gets logged (default: warnings and errors), e.g. `AGENTTY_LOG=debug` or `AGENTTY_LOG=wire=trace`. See [Logging](/docs/logging). |
| `--events jsonl` | `agentty run` only. Streams events to stderr. See [Events](#events). |
| `-V`, `--version` | Print the agentty version and exit. |
| `-h`, `--help` | Print usage and exit. |

## agentty run

`agentty run` sends one prompt through the full agent loop (tools, sandbox, no UI) and prints the answer to stdout. Errors go to stderr, so you can capture stdout safely. If a run fails, the last stderr line is `agentty run: <status>: <reason>`.

```bash
agentty run "add a --dry-run flag to the cli"
git diff | agentty run "review this"          # piped input is added to the prompt
agentty run --agent reviewer -o review.md "review src/"
agentty run --json --timeout 10m --max-turns 40 "fix the failing test"
agentty run --read-only --no-network "review this repo for secrets"   # safe on untrusted code
agentty run --save "plan the migration" && agentty run --resume last "now do step 1"
```

With no prompt, or with `-`, the prompt is read from stdin.

| Flag | Effect |
|---|---|
| `--agent <type>` | Role: `general` (default), `explorer`, `reviewer`, `tester`, `coder`, or one of your own [subagents](/docs/subagents). |
| `--json` | Print one JSON object: `status`, `result`, `error`, `model`, `turns`, `tool_calls`, `tool_errors`, `usage` (tokens, plus `last_input_tokens` and `context_window`), `duration_ms`. |
| `-o`, `--output <file>` | Write the answer (or the JSON) to a file instead of stdout. |
| `--max-turns <n>` | Stop after `n` model turns. Default 80, or 32 for read-only roles. |
| `--timeout <d>` | Stop after `90`, `90s`, `5m` or `1h`. Default 15m. A running tool is killed when time is up. |
| `--effort <level>` | Let the model think: `low`, `medium`, `high`, `xhigh`, `max` or `auto`. Clamped to what the model supports. Off by default. |
| `--read-only` | Only tools that read. Writes, commands and network tools are hidden from the model and refused if it asks anyway. |
| `--no-network` | Commands run with no network (an empty network namespace), and network tools are refused. Needs the claybin sandbox; without it the run won't start. |
| `--save` | Save the conversation as a thread. The id is in `--json` output and in `-v`. |
| `--resume <id\|last>` | Continue a saved thread with a new prompt. `last` is the newest. The result is saved back to the same thread. |
| `-v`, `--verbose` | Print stats on stderr at the end, including how full the context window got. |
| `--events jsonl` | One JSON line per event on stderr. See [Events](#events). |

Ctrl-C or SIGTERM stops the run and kills the running tool. A second Ctrl-C exits at once.

### Events

With `--events jsonl`, stderr gets one JSON object per line during the run. stdout still gets only the answer.

```json
{"ev":"turn","turn":1,"seq":0}
{"ev":"reasoning","turn":1,"text":"the test fails because…","seq":1}
{"ev":"tool","tool":"read","ms":3,"ok":true,"args_sha":"1a2b3c4d","seq":2}
{"ev":"text","turn":2,"text":"Fixed it by…","seq":3}
```

- `turn` marks each model turn. `text` and `reasoning` carry what the model writes. `tool` is one line per tool call.
- `args_sha` is a hash of the arguments, not the arguments. Use it to tell whether two calls were identical.
- `reasoning` appears when the provider streams the model's thinking. Models that reason on their own (deepseek-r1 and similar, which send `reasoning_content`) show it without a flag. For Claude, add `--effort` so it thinks at all.
- Text is sent one paragraph at a time. Join the lines of one kind, in order, to get the whole text.

```bash
agentty run --effort high --events jsonl "fix the flaky test" 2> events.jsonl
jq -r 'select(.ev=="reasoning") | .text' events.jsonl
```

### Exit codes

| Exit code | Meaning |
|---|---|
| 0 | Finished with an answer. |
| 1 | The agent failed or ran out of turns. Any partial answer is still printed. |
| 2 | Bad usage (unknown flag, bad value, no prompt). |
| 3 | Provider or auth problem. Also a rate limit the provider says lasts more than 15 minutes (a daily or plan quota); that ends the run at once. Shorter rate limits are retried up to 3 times, at most 30 s apart. |
| 4 | `--resume` found no such thread. |
| 5 | The model's context window filled up. stderr says how big the window was and how big the last prompt got. |
| 124 | Hit `--timeout`. |
| 130 / 143 | Stopped by Ctrl-C / SIGTERM. |

## Command palette and slash names

Type `/` at the start of the composer to get a menu of everything you can run by name: built-in actions (`/new`, `/compact`, `/model`, `/rename`, `/fork`, `/rewind`, `/review`, `/threads`, `/settings`, `/tools`, `/login`, `/quit`), your own commands, skills and MCP prompts. See [Slash Commands](/docs/slash-commands).

`^K` opens the fuzzy command palette with every action:

| Command | What it does |
|---|---|
| New thread | Start a fresh conversation. |
| Compact context | Replace the history with a structured summary to free up the context window. It runs on the cheapest capable model of the active provider, so it costs a fraction of a normal turn. |
| Compaction depth | Set how full the context window gets before agentty compacts in the background: 75% Aggressive → 90% Balanced → 95% Deep. Higher values help on a 1M-context model (see [Providers](/docs/providers#1m-context-models)). Kept across restarts. |
| Review changes | Open the diff review pane (also `^R`). |
| Accept all / Reject all changes | Apply or discard every pending hunk. |
| Rewind to checkpoint | Go back to an earlier turn's worktree snapshot through a diff-preview picker. Needs a git repo and an idle session. See [Checkpoints](/docs/threads#checkpoints--rewind). |
| Cycle profile | Write → Ask → Minimal (also `S-Tab`). |
| Open model picker / Switch provider | Change the model (`^/`) or the backend (`^P`). |
| Open threads / Open plan | Browse saved conversations (`^J`) or view the todo plan (`^T`). |
| Run code block | Run a fenced block from the last reply (`^G`). |
| Login / Quit | Sign in, or exit agentty. |

## Air-gap flags

Flags for `agentty airgap`:

```run
agentty airgap --help
```

| Flag | Effect |
|---|---|
| `--setup` | Copy credentials to the remote on the first run. |
| `--acp` | Print a ready-to-paste Zed `agent_servers` config that tunnels `agentty acp` over ssh stdio. Flags after it go to the remote agent. See [Zed / ACP](/docs/acp#agentty-in-zed-on-an-air-gapped-remote). |
| `--clipboard-relay` | Forward this machine's clipboard agent for this session, so Ctrl+V on the remote pastes images and files. Not needed after `agentty paste setup`. |
| `--remote-agentty <path>` | Path to agentty on the remote, if it isn't on `PATH`. |

## agentty paste

The clipboard commands. Ctrl+V in the composer uses the same reader and settings. Full guide: [Clipboard & Images](/docs/clipboard).

| Command | What it does |
|---|---|
| `agentty paste [-o DIR]` | Print text, or save images and files (into `DIR`, else the save folder) and print their paths. Works locally and over ssh. Exits 1 when nothing could be read. |
| `agentty paste doctor` | Show the settings in effect, state folder, agent, display, ssh, forwarded sockets, and the result of a real paste. Exits 1 if that paste fails. |
| `agentty paste setup` | Run once on the machine you type on. Adds (or updates) the ssh hook block at the top of `~/.ssh/config` and starts the agent, so Ctrl+V over ssh reads that machine's clipboard. |
| `agentty paste agent` | Run the clipboard agent in the foreground (`setup` starts it in the background). |
| `agentty paste config [get\|set\|reset]` | The paste settings: `config set <key> <value>`, `config get <key>`, `config reset <key\|all>`. Same rows as Settings → Paste. See [Configuration → Paste](/docs/configuration#paste). |
| `agentty paste howto` | One-time macOS and Windows snippets that turn the clipboard into a `richpaste:1:` line, for terminals that never pass the clipboard on. |
| `agentty paste page` | Print the web page that does the same in any browser (also at [agentty.org/paste](https://agentty.org/paste)). |
| `agentty paste encode FILE` | Print `FILE` as one `richpaste:1:` line. |
| `agentty paste decode [DIR]` | Read such lines on stdin and save the files they carry. |
| `agentty paste help` | List all paste commands. |
