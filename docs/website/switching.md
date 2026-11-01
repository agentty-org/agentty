---
title: Coming from Codex or Claude Code
description: Pick up your Codex and Claude Code conversations in agentty and keep going, on any model. Your CLAUDE.md, AGENTS.md, skills, commands and MCP servers come along too.
nav_section: Getting Started
nav_order: 17
slug: switching
---

You don't start over when you move to agentty. Your past Codex and Claude Code conversations show up in the thread list, and you can open one and keep talking. Your instructions, skills and slash commands are read from where those tools keep them. MCP servers come over with one command.

Nothing here changes the other tool's files. You can keep using both.

## Continue a conversation

Open the thread list with [[Ctrl+J]]. Codex and Claude Code sessions sit next to your agentty threads, newest first. They're marked with `↗` and the tool's name:

```text
▎ ● fix the windows build                                  3 msgs · now
  ↗ what did you do with the agentty? It's not opening.    Codex · 75 msgs · 3mo
  ↗ Skip to content 1ay1 agentty Repository navigation…    Codex · 212 msgs · 3mo
```

Press [[Tab]] to cycle through **all → this folder → Codex & Claude**, and [[S-Tab]] to go back. Tab skips any list that would be empty. The panel title shows which list you're on, and the hint row shows where Tab goes next.

With **Thread preview** on (Appearance), the card above the list shows the session's turns. Replies are labelled with the tool that wrote them, and the hint reads `Enter continue here`.

Press [[Enter]] and the session opens as an agentty thread. Type your next message and send it. The model gets the whole conversation, so it knows what you were doing, even if it's a different model from a different company. A line marks where agentty took over:

```text
┈┈┈┈┈┈┈┈┈┈┈┈┈┈  → continued in agentty · from Codex  ┈┈┈┈┈┈┈┈┈┈┈┈┈┈
```

### What comes across

| | |
|---|---|
| your messages and the replies | yes, with timestamps |
| images you pasted | yes |
| tool calls | yes, shown with agentty's own cards (below) |
| tool output | yes, including errors |
| reasoning | the readable summary, when the tool saved one |
| token counts | yes, so the context meter is right from the start |
| compactions | yes, the summary is kept and a divider shows where it applied |
| which model wrote each reply | yes, in the turn header |

### What doesn't come across, and why

- **Encrypted or signed reasoning.** Codex stores its reasoning encrypted, and Claude Code signs its thinking. Both only work for the account and model that made them, so they're dropped. The readable part stays.
- **Sessions that weren't yours.** Runs started by other programs (`codex exec`, SDK and editor integrations) and Claude Code's subagent side-chats aren't listed.
- **Rewound branches.** If you rewound a Claude Code conversation, you get the branch you ended on, the same thing Claude Code shows.

### How tool calls look

Each call is mapped to the agentty tool that does the same job, so you get the same cards you'd get from an agentty turn:

| Codex | Claude Code | shows as |
|---|---|---|
| `exec_command`, `shell` | `Bash` | Shell, with the command and its output |
| `apply_patch` | `Edit`, `MultiEdit` | Patch / Edit, with a coloured diff |
| | `Write` | Write |
| `view_image` | `Read` | Read |
| | `Grep`, `Glob`, `LS` | Grep, Glob, List |
| `update_plan` | `TodoWrite` | Todo list |
| `web_search` | `WebSearch`, `WebFetch` | Search, Fetch |
| | `Task` | Agent |

For Codex patches, the diff shown is the one Codex actually applied, not just the patch it asked for. Anything else keeps its own name and arguments and shows as a generic card. Codex runs its tools through small scripts. A script that makes one tool call shows as that tool; a script with several shows as **Codex Script**.

### When you keep using the other tool

Say you open a Codex session in agentty, ask a couple of things, then go back to Codex and carry on there. Next time you open it in agentty, it re-reads the session: Codex's new turns come in, and your agentty turns stay after them.

Opening the same session again always goes back to the same thread. You never get duplicates.

### Where it's saved

Opening a session saves it as a normal thread under `~/.agentty/threads/`. From then on it works like any other thread: rename it with [[Ctrl+R]] in the thread list, fork it, compact it, delete it with [[Ctrl+D]] in the thread list. Deleting it in agentty doesn't touch the original session.

## Find sessions from the command line

```bash
agentty sessions
```

lists every session agentty can see, newest first:

```text
  0  Codex        212 msgs   3mo ago  Skip to content 1ay1 agentty Repository navigation…
      /home/you/projects/agentty
  1  Codex         75 msgs   3mo ago  what did you do with the agentty? It's not opening.
      /home/you/projects/agentty

1 other file(s) skipped (subagent or SDK runs, empty)
```

`agentty sessions --dump N` prints how session `N` reads once it's an agentty thread: each turn, each tool call with its arguments and output, and where compactions are. It's handy when a session doesn't look the way you expect.

## Where agentty looks

| tool | sessions | override |
|---|---|---|
| Codex | `~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl` | `$CODEX_HOME` |
| Claude Code | `~/.claude/projects/<project>/<session>.jsonl` | `$CLAUDE_CONFIG_DIR` |

The list is read in the background each time you open [[Ctrl+J]]. A file that hasn't changed since the last look isn't read again, so a long history costs nothing after the first scan.

Don't want them in the list? Turn off **Appearance → Codex & Claude sessions**. Threads you already opened stay, since they're agentty threads now.

## Your instructions come with you

agentty reads guidance files from the same places those tools do. There's nothing to copy.

| file | read from |
|---|---|
| Claude Code, personal | `~/.claude/CLAUDE.md` (or `$CLAUDE_CONFIG_DIR/CLAUDE.md`) |
| Claude Code, project | `./CLAUDE.md` and `./.claude/CLAUDE.md` |
| Claude Code, local | `./CLAUDE.local.md` |
| Codex, personal | `~/.codex/AGENTS.md` (or `$CODEX_HOME/AGENTS.md`), when you have no `~/.agentty/AGENTS.md` or `~/.agents/AGENTS.md` |
| any tool, project | `./AGENTS.md`, plus the nearest one in a monorepo package |

Details and the order they're applied in: [Configuration → CLAUDE.md](/docs/configuration#claudemd-guidance) and [AGENTS.md](/docs/configuration#agentsmd-guidance).

## Skills, subagents and slash commands

These are plain markdown files, and agentty reads them where Claude Code keeps them: `.claude/skills/`, `.claude/agents/` and `.claude/commands/`, in the project and in your home folder. The shared `.agents/` folders work too. They behave the same, `$ARGUMENTS` included. See [Skills](/docs/skills) and [Slash Commands](/docs/slash-commands).

## MCP servers

MCP servers start programs, so agentty doesn't run another tool's config behind your back. It copies the servers over once, after showing you what it found:

```bash
agentty mcp import                 # what each tool offers
agentty mcp import --from codex    # copy Codex's servers
agentty mcp import --from claude   # copy Claude Code's servers
```

`--dry-run` shows what would change, and `--force` replaces servers agentty already has with the same name.

From Codex, `[mcp_servers.*]` in `~/.codex/config.toml` (and `.codex/config.toml` in the project) is translated to agentty's format: command, args, env, working directory, URL, headers, timeouts, disabled tools and the off switch. Two things don't come over, and the import tells you which server they belong to:

- **Secrets held in environment variables** (`bearer_token_env_var`, `env_http_headers`). agentty stores headers as written, so it won't copy a token into a file for you. Add the header by hand, or use `agentty mcp-login` if the server supports OAuth.
- **`enabled_tools`.** agentty filters with an exclude list only.

More in [MCP](/docs/mcp).

## Sign-in

Your Claude Pro/Max or ChatGPT plan works in agentty too. `agentty login` signs in with the same accounts. agentty doesn't read the other tools' saved logins: if two programs share one refresh token, one of them gets signed out. See [Authentication](/docs/authentication).
