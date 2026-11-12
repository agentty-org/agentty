---
title: The Interface
description: A tour of the agentty screen: composer, transcript, status bar and tool widgets.
nav_section: User Manual
nav_order: 10
slug: interface
---

This page explains what you see on the agentty screen and how to change how it looks. agentty draws at the bottom of your terminal and never takes over the whole screen, so your scrollback stays intact. To change colours or layout, press [[Ctrl+K]] → **Appearance** ([below](#appearance)).

## The transcript

Your conversation scrolls in your terminal's normal scrollback: your messages, the assistant's replies and every tool call, in order. Queued messages show as preview rows above the composer and look the same as sent messages.

Inside a git repo, a message that saved a worktree snapshot has a small `· ↺ checkpoint` tag in its meta line. That is a restore point you can [rewind to](/docs/threads#checkpoints-rewind) from the command palette.

## The composer

The input box at the bottom. Type and press [[Enter]] to send.

- [[Shift+Enter]] or [[Alt+Enter]] inserts a newline.
- [[Ctrl+E]] makes the composer taller for long prompts.
- If you type while a turn is running, your message is **queued** and sent when the turn ends. A `❚ N queued` chip shows how many are waiting. On an empty composer, [[↑]] brings every queued message back into the box for editing.
- [[@]] [mentions a file](clip:file-mention), [[#]] picks a symbol, and [[/]] at the start of a line opens [the slash commands](clip:slash-commands). That's the same fuzzy list as [the command palette](clip:command-palette) on [[Ctrl+K]], with entries like *Compact context*, *Switch provider* and *New thread*.
- To attach a PNG, JPEG, GIF or WebP, drop the image file's path in or press [[Ctrl+V]]. [[Ctrl+V]] also pastes copied files (small text files as their text, others saved and their path typed in). Over ssh, run `agentty paste setup` once on your laptop. See [Clipboard & Images](/docs/clipboard).

## Run code blocks (Ctrl+G)

When a reply gives you shell commands in a code block, you don't need to copy them.

1. Press [[Ctrl+G]] to list the runnable blocks in the newest reply.
2. Press [[Enter]] or the block's number to run it **on your real terminal**. agentty steps aside, so `sudo` password prompts work and output shows live. [[Ctrl+C]] stops the command, not agentty.
3. When it finishes, a result card lets you attach the output to your next message as a collapsed chip ([[a]]), copy it ([[y]]), or throw it away ([[Esc]]).

A short toast reminds you of [[Ctrl+G]] while commands are on screen. Each block runs in the right shell: `sh`/`bash` blocks through `/bin/sh` on Linux/macOS, `powershell`/`cmd` blocks natively on Windows.

## Threads & quick-cycle

Every conversation is saved as a thread. [[Ctrl+J]] opens the thread list at the current thread (marked with a bold `●`). [[Ctrl+N]] starts a new one.

To switch to the next thread without opening the list, press [[Alt+←/→]] (or [[Ctrl+←/→]] on an empty composer). [[←]] is newer, [[→]] is older. A "thread k/N · title" toast shows where you are. This only works when no turn is running.

When a thread fills the context window, use **Fork thread** from the palette ([[Ctrl+K]]). It opens a fresh thread with a **⑃ Forked** card and almost no context; the old transcript is read only when needed. See [Forking a Thread](/docs/fork).

## Reading the model picker

[[Ctrl+/]] opens [one list across every provider](clip:model-picker) you're signed into. Rows look like this:

```
▎ anthropic   Claude Sonnet 4.6      200k  ★ ✦
  openai      gpt-5-mini             400k    ✦
  ollama      qwen3:4b · chat only   auto
```

Left to right: the `▎` bar (marks the active row), the provider badge, the model name, and a dim reference column.

The **badge colour** shows the model's capability tier, agentty's guess at what it can handle. The list is sorted strongest first.

| Badge | Tier | Meaning |
|---|---|---|
| Magenta | Flagship | The provider's top model: long multi-step work, hard reasoning |
| Blue | Mid | Reliable tool use, good for most turns |
| Cyan | Cheap | Small and fast: fine for text, summaries, titles |
| Grey | Weak | Tool use is unreliable; expect trouble with multi-step work |

The tier comes from the model id (Haiku < Sonnet < Opus, `gpt-5-mini` < `gpt-5`). Local models rank by parameter count. The **active** model's badge uses the brand accent colour instead of its tier colour.

Colour is never the only signal. A model that can't call tools gets **`· chat only`** added to its name.

The reference column shows the context window, then two fixed slots: **★** favourite ([[Ctrl+F]]) and **✦** reasoning-capable. The slots stay in place so rows line up.

`auto` in the context column means agentty couldn't find the model's window size and uses a default. This is rare: the size usually comes from the gateway, a probe, or models.dev. If you run a private gateway with an unknown model id, set [`AGENTTY_MAX_CONTEXT_TOKENS`](/docs/environment).

## Reading the provider picker

[[Ctrl+P]] [lists backends](clip:provider-picker), not models. Each row ends with a chip that tells you whether you can use it right now:

| Chip | Meaning |
|---|---|
| ✓ green | Signed in. On the active row it reads `✓ signed in · accounts`, and [[Enter]] opens that provider's accounts instead of switching |
| ✓ grey | Signed in, but not the current provider. [[Enter]] switches to it |
| ● blue | Works without login: a local server, an external ACP agent, or a key found in your environment (`● key from OPENAI_API_KEY`) |
| ⚠ amber | Needs action: `⚠ sign in`, or the name of the env var to set |
| ✗ amber | [[Ctrl+D]] is armed; press again to sign out or remove |

A key you **pasted** shows `key saved`, and [[Ctrl+D]] removes it. A key from the environment shows `key from VAR`, and [[Ctrl+D]] leaves it alone, because agentty can't unset a variable in your shell.

The footer changes with what you type. Type something that looks like an endpoint and it tells you what [[Enter]] will connect to. Type something that matches no provider and it points you to the `host:port` path.

## The status bar

One row at the bottom shows the permission profile, provider, model and current phase. When something needs your attention (a retry, an error) it briefly shows a message instead (`▎⚠ <text>` for errors, `▎ <text>` for info), then goes back to the key hints. Switch provider with [[Ctrl+P]] and model with [[Ctrl+/]] without leaving the thread.

## Tool widgets

Each tool has its own display instead of raw JSON:

- **Diffs** are colour-coded, with line numbers taken from the diff's `@@` headers, so they match the file. Numbers are blank across a skipped gap and resume at the next hunk.
- **Search results** are grouped by file, one match per row, with the file's real line number on the right.
- **File reads** keep real line numbers. A `read` with an `offset`, `start_line` or `symbol=` starts counting at the line the slice starts on. The header names the symbol you read (`file.cpp · foo()`).
- **Terminal output** (`bash`, `test`, `diagnostics`, and `process_start`/`poll`/`stop`) shows the newest lines and the **exit code**, and pulls out a one-line result from test and compiler summaries when it can.
- **Todos** show as a checklist that ticks off as work gets done. [[Ctrl+T]] opens [the whole plan](clip:todo-plan).
- **Tools that answer up front** (`repo_map`, `outline`, `list_dir`, the search tools) preview the top of their output, not the footer.

## Smooth streaming

Text is shown at a steady pace, so a reply that arrives in big chunks still types out evenly. When a turn ends, any leftover text slides out over a moment. This works over SSH too. Where the terminal supports it, frames use DEC 2026 synchronized output to avoid tearing. Set **Motion** to *off* to turn the animation off.

## Appearance

[[Ctrl+K]] → **Appearance** opens a form with fifteen settings. Changes apply right away and are saved to your user config (not the project's), since things like a light terminal are about you, not the repo.

| Setting | Options |
|---|---|
| Language | the interface language |
| Scheme | `native` + 615 built-ins, in a fuzzy browser |
| Colors | auto / truecolor / 256 / 16 / mono |
| Background | auto / dark / light |
| Density | compact / normal / roomy |
| Prose width | 0–200 columns (0 = no cap) |
| Compact turns | on / off |
| Thread preview | on / off: the card above the [[Ctrl+J]] list showing the highlighted thread |
| Codex & Claude sessions | on / off: list [those sessions](/docs/switching) in [[Ctrl+J]] |
| Motion | full / reduced / off |
| Scrollback | auto / tiny / small / big / full: how much history is redrawn on open and resize ([below](#scrollback)) |
| Syntax highlighting | on / off |
| Tool output | collapsed / preview / full |
| Thinking | shown / collapsed / hidden |
| Timestamps | off / relative / absolute |

When Colors or Background is `auto`, the form shows what was detected, so you can check it before overriding.

The theme browser previews live: moving through the list restyles the transcript, [[Enter]] keeps the theme, [[Esc]] goes back to the one you started with. Typing filters by fuzzy match: `gvd` finds Gruvbox Dark.

Colour changes repaint everything. **Layout** changes (density, prose width, compact turns) only apply to new output, because rows already in your scrollback belong to the terminal.

### Scrollback

When you open a thread or resize the window, agentty redraws the conversation into your terminal's scrollback so you can scroll up through it. For a long thread this redraw costs the most time and memory, and anything past your terminal's scrollback limit is thrown away anyway. **Scrollback** sets how much is redrawn:

| Level | Rows |
|---|---|
| **auto** (default) | as many as your terminal keeps |
| tiny | about 1,000 |
| small | about 5,000 |
| big | about 20,000 |
| full | the whole thread |

*auto* reads your terminal's scrollback setting: tmux's `history-limit`, zellij, GNU screen, kitty, WezTerm, Alacritty, foot, Ghostty, Konsole, xfce4-terminal, GNOME Terminal, Windows Terminal, the Windows console, iTerm2, Terminal.app and VS Code. If the setting isn't in the config, it uses that terminal's default. An unknown terminal gets 5,000 rows. The config is read in the background, so startup isn't slowed. `AGENTTY_REPLAY_ROWS` overrides every level (`0` turns the redraw off).

### Why `native` is the default

`native` uses only your terminal's own foreground, background and the 16 ANSI colours **you** set up. It never uses a fixed RGB value. So it looks right on light, dark, 16-colour and monochrome terminals, and your own palette (Catppuccin, Gruvbox, your own) shows through.

The catch: agentty can't read those 16 colours, so a few effects that need to know a colour's brightness are simpler. Filled elements use reverse video, and blends it can't compute are left out. If you'd rather agentty use a full palette, pick one of the 615 in the browser.
