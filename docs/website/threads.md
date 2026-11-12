---
title: Threads & Persistence
description: Where agentty stores conversations and credentials, and how to manage them.
nav_section: User Manual
nav_order: 40
slug: threads
---

Every conversation is a thread, saved as plain files you can read, back up or delete. Press [[Ctrl+J]] to open the thread list and switch between them, and [[Ctrl+N]] to start a new one.

## Managing threads

In the thread list ([[Ctrl+J]]):

| Key | Action |
|---|---|
| type | Filter by title or first message |
| [[↑]] / [[↓]] | Move |
| [[Home]] / [[End]], or [[Shift+↑]] / [[Shift+↓]] | Jump to the top / bottom |
| [[PgUp]] / [[PgDn]] | Move a page |
| [[Tab]] / [[S-Tab]] | Cycle the list: all → this folder → Codex & Claude (empty ones are skipped) |
| [[←]] / [[→]] | Scroll the preview a turn at a time; with Shift, to the first / latest |
| [[Enter]] | Open |
| [[Ctrl+R]] | Rename |
| [[Ctrl+D]] | Delete (press twice) |
| [[Ctrl+N]] | New thread |
| [[Esc]] | Clear the filter, then close |

The panel title says which list you're on, and the hint row says where [[Tab]] goes next. With **Appearance → Thread preview** on, a card above the list shows the highlighted thread's turns.

**Renaming.** [[Ctrl+R]] opens the highlighted title for editing. [[Enter]] saves, [[Esc]] cancels. Saving an empty title goes back to the automatic name. From the composer, `/rename <title>` renames the open thread.

**Automatic names.** A new thread is first named after the first line of its first message. After the first reply, agentty asks the cheap utility model (the one compaction uses) for a 3 to 6 word title and swaps it in. A toast tells you, and the list marks those names `auto`. It asks once per thread, even if that fails, and never replaces a name you set. Forks keep their `Fork:` name, imported Codex or Claude Code threads keep their old name, and older threads keep theirs. Turn it off in Settings → **Chat names**, or with `"auto_title": false` in `settings.json`.

**Deleting.** Deleting a thread removes all its files. Its blobs are removed in the next daily sweep if no other thread uses them.

**Expiring old threads.** Threads are kept until you delete them. To delete old ones automatically:

```sh
export AGENTTY_THREADS_KEEP_DAYS=90
```

Once a day, threads with no activity for that long are deleted. A thread open in a running agentty is never touched. See [Storage](/docs/storage) for everything else agentty keeps on disk.

**Scripts.** Headless runs are threads too. `agentty run --save "…"` keeps the run as a thread (it shows up in [[Ctrl+J]]), and `agentty run --resume <id>` or `--resume last` continues one. See [agentty run](/docs/cli#agentty-run).

## Conversations from Codex and Claude Code

The thread list also shows your Codex and Claude Code sessions, marked `↗` with the tool's name on the right. [[Enter]] on one brings it in as an agentty thread you can keep talking in, on any model. A `→ continued in agentty` line marks where agentty took over. Opening it again later picks up anything you said in the other tool since. See **[Coming from Codex or Claude Code](/docs/switching)** for details.

## Forking a thread

When a conversation fills the context window, press [[Ctrl+K]] and pick **Fork thread**. You get a fresh thread with almost no context. The old transcript is saved to disk and the fork reads it only when it needs to, so forking costs about the same few tokens however long the old thread was. The original thread is left as it was. See **[Forking a Thread](/docs/fork)**.

## Checkpoints & rewind

Inside a git repo, agentty saves a snapshot of your working tree at the start of every message you send, before the agent edits anything. Those messages get a small `· ↺ checkpoint` tag. Nothing is committed to your history, and the snapshot is taken in the background while the request goes out.

To go back to an earlier point:

1. Press [[Ctrl+K]] and pick **Rewind to checkpoint**.
2. Pick a turn. Each row shows the turn number, a preview of the prompt, how long ago it was, and `N files · +A −D`: what has changed in the tree since then. [[↑↓]] / [[j]] / [[k]] move.
3. Press [[Enter]] to rewind, or [[Esc]] to cancel.

A rewind puts back **both** the files and the conversation to just before that message, and puts your original prompt back in the composer so you can edit and resend. It can't be undone. It only works in a git repo with no turn running (a toast explains otherwise). Checkpoints belong to the folder agentty was started in, so they still work with `--workspace /`.

The file restore is exact: edited files get their old contents, deleted files come back, and files created after that point are removed. Files git ignores (build output, caches, `node_modules`) are never in a snapshot and are left alone. Snapshots live under `refs/agentty/checkpoints/…` with no parent commits, so they never touch your branch, index or `HEAD`. The newest 64 are kept per project.

## Where threads live

Threads are saved in `~/.agentty/threads/` (change it with `AGENTTY_THREADS_DIR`). Each thread is an append-only log plus two small side files:

```text
~/.agentty/threads/
├── f24a29c6….jsonl        # the conversation, one event per line
├── f24a29c6….ofs          # line offsets, so a long thread opens fast
├── f24a29c6….meta.json    # title, dates
├── index.json             # picker cache, rebuilt if missing
└── blobs/                 # images and big tool outputs, shared by content hash
```

Threads aren't tied to a project. The folder is flat, so [[Ctrl+J]] lists every conversation no matter where you started agentty.

## Atomic writes

Thread and credential files are written safely: agentty writes a temp file, calls `fsync`/`_commit`, then `rename`s it into place (`MoveFileExW` on Windows). A crash mid-write can't leave a half-written thread.

## Credentials

Logins are stored separately in `~/.agentty/credentials/credentials.json` (mode `0600`). See [Authentication](/docs/authentication).
