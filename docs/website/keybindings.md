---
title: Keybindings
description: The complete agentty keymap.
nav_section: User Manual
nav_order: 20
slug: keybindings
---

Every key agentty understands. The first table has the keys you'll use most; the rest are grouped by where they work.

## Most used

| Key | Action |
|---|---|
| [[Enter]] | Send the message (while a turn runs, it's queued) |
| [[Shift+Enter]] | New line in the composer |
| [[Esc]] | Cancel the current turn, reject a permission, or close a panel. It never quits |
| [[Ctrl+C]] | Quit (the only quit key) |
| [[Ctrl+K]] | Command palette |
| [[Ctrl+/]] | Model picker |
| [[Ctrl+J]] | Thread list |
| [[Ctrl+N]] | New thread |
| [[S-Tab]] | Cycle permission profile (Write → Ask → Minimal) |
| [[Ctrl+R]] | [Review pending diffs](clip:diff-review) |
| [[Ctrl+V]] / [[Alt+V]] | Paste from the clipboard, including images |

## Global

These work from the main screen.

| Key | Action |
|---|---|
| [[Ctrl+K]] | Command palette |
| [[Ctrl+J]] | Thread list (opens at the current thread). Includes your [Codex and Claude Code sessions](/docs/switching) |
| [[Ctrl+N]] | New thread |
| [[Alt+←/→]] | Switch to the next thread (← newer, → older) |
| [[Ctrl+←/→]] | Same, but only on an empty composer with no turn running |
| [[Ctrl+/]] | Model picker: one fuzzy list of every model from every provider you're signed into. [[Enter]] switches provider and model together |
| [[Ctrl+Tab]] | Swap back to the previous model you used (works across providers) |
| [[Ctrl+P]] | Provider picker (add or switch backends, hosts, accounts) |
| [[Ctrl+S]] | Smart Mode overlay (routing + learning) |
| [[Ctrl+T]] | Todo / plan view |
| [[Ctrl+G]] | Run a code block from the newest reply on your real terminal |
| [[Ctrl+R]] | [Review pending diffs](clip:diff-review) |
| [[Ctrl+O]] | Tool output viewer: browse the full output of past tool calls |
| [[Ctrl+U]] | On an empty composer: read thinking blocks in full, newest first. If a retrieved-context card is live, it expands or collapses that card instead |
| [[Ctrl+L]] | Stats: tokens, cache, tools, latency for this thread |
| [[Ctrl+Y]] | Redraw the screen (inside the composer [[Ctrl+Y]] is redo) |
| [[Ctrl+E]] | Toggle the taller composer |
| [[Ctrl+B]] | Loop: send this message, then send it again after every finished turn until you press it again |
| [[S-Tab]] | Cycle permission profile (Write → Ask → Minimal) |
| [[Ctrl+C]] | Quit |

**Open keys also close.** Press a panel's own key again to close it: [[Ctrl+P]] closes the provider picker, [[Ctrl+K]] the palette, [[Ctrl+S]] Smart Mode, and so on. The two pickers link to each other: [[Ctrl+P]] in the model picker jumps to the provider picker, and [[Ctrl+/]] in the provider picker jumps back. Inside the model picker, [[Ctrl+/]] narrows the list to the highlighted row's provider (press again to clear) and [[Ctrl+F]] marks a favorite.

**The model picker.** Type `son` for every Sonnet or `gpt` for every GPT. A **recent** section at the top holds the models you switch between. Providers you aren't signed into show as dim **sign in to …** rows that open login. Use [[Ctrl+P]] when you want to manage backends rather than pick a model.

**Esc steps back one level.** In a multi-step flow (provider picker → **Custom host…** → host input → API-key prompt), [[Esc]] goes back one step and keeps what you typed. The footer shows `Esc back` when there's a step to go back to, `Esc cancel` when there isn't.

:::note
**If [[Ctrl+/]] or [[Ctrl+Tab]] do nothing, your terminal can't send them.** Older terminals can't encode these chords (on most, `Ctrl`+`Tab` is the same byte as `Tab`). agentty turns on the **kitty keyboard protocol** at startup, so they work on kitty, Ghostty, WezTerm, foot, Konsole, recent xterm/Alacritty, iTerm2 3.5+ and **Blink Shell** on iOS. Where they don't (notably **mosh**, some minimal iOS SSH apps), type **`/model`** (or `/swap`) in the composer, or pick **"Switch model"** / **"Swap to previous model"** from [[Ctrl+K]]. Set `MAYA_NO_KITTY_KEYBOARD=1` to turn the protocol off.
:::

## Composer

The composer is a readline-style editor.

### Submit & compose

| Key | Action |
|---|---|
| [[Enter]] | Send the message (or, while streaming, queue it) |
| [[Shift+Enter]] | Insert a newline |
| [[Alt+Enter]] | Insert a newline (for terminals that don't send Shift+Enter) |
| [[Ctrl+E]] | Toggle the expanded (taller) composer |
| [[/]] | At the start of a line: open the `/` command menu. [[↑]]/[[↓]] or [[Ctrl+P]]/[[Ctrl+N]] move, [[Tab]] completes, [[Enter]] completes a partial name or runs an exact one, [[Esc]] hides it |
| [[@]] | At a word boundary: open the file picker; inserts an `@file` chip |
| [[#]] | At a word boundary: open the symbol picker; inserts a `#symbol` chip |

### Cursor motion

| Key | Action |
|---|---|
| [[←]] / [[→]] | Move one character (an attachment chip counts as one) |
| [[Ctrl+←]] / [[Ctrl+→]] | Move one word (a run of punctuation counts as one). On an empty composer with no turn running, they switch threads instead |
| [[Home]] / [[End]] | Jump to start / end of the buffer |

### Editing

| Key | Action |
|---|---|
| [[Backspace]] | Delete the character (or whole chip) before the cursor |
| [[Ctrl+W]] | Delete the word before the cursor |
| [[Alt+D]] | Delete the word after the cursor |
| [[Ctrl+U]] | Delete to start of line (on an empty composer it opens the thinking reader, see Global) |
| [[Alt+K]] | Delete to end of line |
| [[Ctrl+Z]] | Undo, a word at a time |
| [[Ctrl+Y]] / [[Ctrl+Shift+Z]] | Redo |
| [[Ctrl+V]] / [[Alt+V]] | Paste from the clipboard: an image becomes an image chip, a copied file becomes its text or saved path, text stays text |

Undo groups a run of typing into one step, so one [[Ctrl+Z]] after a paste gets you back to before the paste.

Delete-to-end is [[Alt+K]], not the usual readline [[Ctrl+K]], because [[Ctrl+K]] always opens the palette.

Some terminals take [[Ctrl+V]] for their own paste (Windows Terminal does). [[Alt+V]] works in every terminal. A terminal's own paste (an empty bracketed paste, or a pasted `richpaste:1:` line) also lands as the image or file. Over **ssh**, run `agentty paste setup` once on your laptop and [[Ctrl+V]] reads the laptop's clipboard. See [Clipboard & Images](/docs/clipboard); each route can be turned on or off in Settings → Paste.

### History & queue

| Key | Action |
|---|---|
| [[↑]] | On an empty composer: bring back queued messages (if any), else step back through your earlier messages |
| [[↓]] | Step forward through history toward your draft |
| [[Alt+↑]] | Edit queued messages one at a time (newest first) |
| [[Alt+↓]] | Step back out of the queue editor toward your draft |
| [[Alt+Backspace]] | Drop the most recently queued message (empty composer, not editing the queue) |

## Queue behavior

Typing while a turn runs **queues** your message instead of interrupting. Two ways to change the queue:

- **[[↑]] on an empty composer** pulls *every* queued message back into the box at once (one per line). This empties the queue, so send again to re-queue.
- **[[Alt+↑]] / [[Alt+↓]]** step through the queue **one message at a time**. [[Enter]] puts the edited message back in its place. This works even with text in the composer.

The composer placeholder shows `press ↑ to edit queued — type to queue another…` when it applies.

## Thread list ([[Ctrl+J]])

| Key | Action |
|---|---|
| type | Filter by title or first message |
| [[↑]] / [[↓]] | Move |
| [[Home]] / [[End]], or [[Shift+↑]] / [[Shift+↓]] | Jump to the top / bottom |
| [[PgUp]] / [[PgDn]] | Move a page |
| [[Tab]] / [[S-Tab]] | Cycle the list: all → this folder → Codex & Claude. Empty lists are skipped |
| [[←]] / [[→]] | Scroll the preview one turn; [[Shift+←]] / [[Shift+→]] go to the first / latest |
| [[Enter]] | Open. On a Codex or Claude Code session, import it and continue here |
| [[Ctrl+R]] | Rename the highlighted thread |
| [[Ctrl+D]] | Delete it (press twice; any other key cancels) |
| [[Ctrl+N]] | Start a new thread |
| [[Esc]] | Clear the filter; press again to close |

## Review pane ([[Ctrl+R]])

When the agent has edited files, [[Ctrl+R]] opens the review pane. The edits are already on disk; the pane lets you keep or undo each one.

| Key | Action |
|-----|--------|
| [[↑]] / [[↓]] | Move between hunks |
| [[←]] / [[→]] or [[S-Tab]] / [[Tab]] | Previous / next file |
| [[Enter]], [[y]] or [[Ctrl+Y]] | Accept the current hunk (keep it) |
| [[n]] or [[Ctrl+N]] | Reject the current hunk (revert it) |
| [[PgDn]] / [[PgUp]] or [[Ctrl+D]] / [[Ctrl+U]] | Scroll inside a hunk taller than the view |
| [[Ctrl+A]] | Accept all: keep every change |
| [[Ctrl+X]] | Reject all: revert every file (press twice to confirm) |
| [[Esc]] / [[Ctrl+R]] | Close: keeps accepted and undecided hunks, reverts rejected ones |

:::warn
**[[Esc]] keeps, it does not cancel.** Closing keeps every hunk you didn't decide on (the edit is already live). The footer says `Esc keep all` / `keep rest` / `apply` so you know what will happen. To throw away *everything*, use [[Ctrl+X]].
:::

## Tool output viewer ([[Ctrl+O]])

| Key | Action |
|---|---|
| [[↑]] / [[↓]] (or [[j]] / [[k]]) | Move in the list, or scroll the open output |
| [[PgUp]] / [[PgDn]], [[Space]] | Scroll a page |
| [[Enter]] | Open the highlighted output |
| [[←]] / [[→]] | Previous / next entry |
| [[/]] or [[Ctrl+F]] | Search; [[n]] / [[N]] next / previous match |
| [[t]] | Jump to the newest |
| [[y]] / [[Ctrl+Y]] | Copy |
| [[Esc]] | Back to the list, or close |
| [[Ctrl+O]] | Close |

The thinking reader ([[Ctrl+U]]) uses the same keys; [[Ctrl+U]] closes it.

## Code blocks ([[Ctrl+G]])

| Key | Action |
|---|---|
| [[1]]–[[9]] | Pick a block |
| [[Ctrl+E]] / [[e]] | Edit the block before running |
| [[Ctrl+Y]] / [[y]] | Copy the block |
| [[Ctrl+G]] | Close |

On the result screen, [[Ctrl+A]] / [[a]] attaches the output to your next message and [[Ctrl+Y]] / [[y]] copies it.

## Palette-only actions

Some actions have no key. Open them from the command palette ([[Ctrl+K]]):

- **Rewind to checkpoint** opens a picker with a diff preview over every checkpointed turn (git repo, no turn running). See [Checkpoints & rewind](/docs/threads#checkpoints--rewind).
- **Fork thread** starts a fresh thread from the current one with almost no context; the old transcript is read only when needed. Use it when the context window is full. See [Forking a Thread](/docs/fork).
- **Compaction depth** sets how full the context window gets before agentty compacts it in the background (75/90/95%). See [Providers & Models](/docs/providers#1m-context-models).
