---
title: Tool Overview
description: The full set of tools agentty can call, and how they render.
nav_section: Tools
nav_order: 10
slug: tools
---

This page lists every tool the model can call. Each tool has its own display. Diffs show as diffs with a line-number gutter. Search results are grouped by file with their real line numbers. File reads keep their true line numbers. Shell commands and background processes show exit codes. Todos become checklists.

| Tool | Effect class | What it does |
|---|---|---|
| `read` | Read | Read a file or a line range. Large files return a symbol outline first. `symbol="name"` reads one function or type, body included. Images (PNG, JPEG, GIF or WebP, up to 8 MiB, detected by content) are attached for a vision model; if the provider refuses, agentty drops the image and retries the turn. |
| `write` | Write | Create a new file with an atomic write. |
| `edit` | Write | Replace text in an existing file and show the diff. |
| `apply_patch` | Write | Apply a unified diff to one file. All hunks apply or none do. |
| `replace` | Write | Find and replace text (literal or regex) across files under a glob. Previews first. |
| `rewrite_structural` | Write | Replace a code shape (same patterns as `search_structural`) across files. Previews first. |
| `move` | Write | Move or rename a file or folder without a shell. |
| `remove` | Write | Delete a file or folder. Folders need an explicit `recursive` flag. A path that is already gone counts as done. |
| `shell` | Shell | Run a shell command in the sandbox. Shows the exit code and output. |
| `process_start` / `process_poll` / `process_stop` | Shell | Start, check and stop a long-running background process (dev server, watcher) without blocking the turn. Output shows the tail, with exit codes and test or compiler summaries pulled out. |
| `grep` | Read | Regex search across files, shown as a table per file. Uses ripgrep when installed. Skips generated trees (`build*`, `_deps`, `node_modules`, `vendor`, `.git`, …). `word=true` matches whole words only. `context:"block"` returns the whole enclosing function. |
| `glob` | Read | Find files by glob pattern. |
| `list_dir` | Read | List a folder with type, size and name. |
| `read_filter` | Read | Read a file but keep only lines that match a pattern, plus context. |
| `extract` | Read | Pull the matched values of a pattern out of files (like `rg -o`). |
| `aggregate` | Read | Count or group a pattern's matches by file, match or capture group. |
| `json_query` | Read | Query a JSON file by path, jq-style. |
| `outline` | Read | Show a file's definitions as a tree, without bodies. |
| `repo_map` | Read | A size-limited, PageRank-ranked map of the codebase: top files with their definitions. `focus` re-centres it. Stops at nested repos and never leaves the workspace. Call it first in a large or unfamiliar repo. |
| `find_definition` | Read | Find where a symbol is defined. For uses, use `grep` with `word=true`. |
| `search_structural` | Read | Search by code shape, not text. Never matches inside comments or strings. `$X` matches one node (an atom or a balanced `(…)`/`[…]`/`{…}` group); `$$$X` matches many. Examples: `foo($$$)`, `if ($$$C) return $X;`, `catch ($$$) {}`, `$X = $X`. No tree-sitter needed. |
| `web_fetch` | Network | Fetch a URL (output is capped). |
| `web_search` | Network | Search the web and return snippets. In `Ctrl+K → Settings → Web Search` you pick which services answer (free ones need no account; others take an API key, stored encrypted), the result count (auto, on or off), and sites to exclude. See [details](/docs/configuration#web-search). |
| `todo` | Pure | Keep a session todo list, shown as a checklist. |
| `diagnostics` | Shell | Run the project's build or lint and report errors and warnings. |
| `test` | Shell | Run focused tests (CTest, Cargo, Go, npm or Make, auto-detected) with pass/fail results. |
| `skill` | Read | Load a skill's full instructions from `.agentty/skills/`. |
| `task` | Shell | Start a subagent (explorer, reviewer, tester, coder or general) with its own context. It returns one short report. Read-only roles (explorer, reviewer) use the cheapest capable model; the others use the parent's model. |
| `search_docs` | Read, Network | Search your docs folder (`./docs` or `AGENTTY_DOCS_DIR`), installed skills and remembered facts by keyword and meaning. Works with no docs set up. Its only network use is the local embedding model. See [Retrieval](/docs/retrieval). |
| `search_code` | Read, Network | Search source code by meaning ("where is retry backoff handled"). Keyword-only by default; `ollama pull qwen3-embedding:0.6b` turns on meaning search. See [Retrieval](/docs/retrieval). |
| `git_status` | Read | Show the branch, staged and unstaged changes, and untracked files. |
| `git_diff` | Read | Show a diff (unstaged, staged or a ref range). |
| `git_log` | Read | Show commit history. |
| `git_show` | Read | Show a commit, or a file as it was at a revision. |
| `git_blame` | Read | Show which commit, author and date last changed each line. |
| `git_commit` | Write | Stage files and commit. |
| `git_branch` | Write | List, create, switch or delete branches. |
| `git_stash` | Write | Shelve or restore uncommitted work. |
| `git_rebase` | Write | Rebase onto a ref, or continue, skip or abort a rebase. |
| `git_cherry_pick` | Write | Apply existing commits onto HEAD. |
| `remember` / `forget` | Write | Save or remove facts that last across sessions. |
| `wipe_memory` | Write | Clear every remembered fact in a scope. Asks for confirmation. |
| `tool_search` | Pure | Find and load plugin tools that aren't loaded yet. Only present when some load on demand. A found tool stays loaded for the conversation. See [how tools load](/docs/plugins#how-tools-load). |

:::note
The **effect class** decides which permission profile runs the tool without asking. *Pure* and *Read* tools run on their own in **Ask** and **Write**. *Write*, *Shell* and *Network* depend on [your profile](/docs/profiles). **Minimal** asks for every class, reads included.
:::

## Compile-time enforcement

Each tool declares its effects at compile time. A `static_assert` checks them against the permission matrix, so a tool can't gain a side effect the policy doesn't cover. The build fails instead.

## Parallel & speculative execution

The effect classes also drive a scheduler that overlaps tool work:

- **Parallel batches.** When the model sends several tool calls in one turn, agentty runs the safe ones at the same time and waits only where calls conflict. Two reads, a grep and a web fetch all run at once. A `write` waits for anything touching the same path. A `shell` call waits for exclusive access. A wide batch is never unsafe, so the model is encouraged to send many calls at once.
- **Speculative reads.** A *Read* tool starts as soon as its arguments finish streaming, while the model is still writing the rest of the turn. Reads can't change what the model is saying, so this is always safe. Anything that writes, runs commands or uses the network waits for the end of the turn.

Neither changes the result, only when the work happens. Tool-heavy turns finish sooner.

## Extending the toolset

Four features add to the built-in tools:

- **[Plugins](/docs/plugins)** add external tools through MCP servers (a browser driver, a database client, a hosted API).
- **[Subagents](/docs/subagents)** hand a self-contained task to a separate agent with its own context window, through the `task` tool.
- **[Slash commands](/docs/slash-commands)** are reusable prompts you run as `/name`.
- **[Hooks](/docs/hooks)** run your own shell commands around every tool call, to block or observe it.
