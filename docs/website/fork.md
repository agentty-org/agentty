---
title: Forking a Thread
description: Fork starts a fresh thread with almost no context. The old transcript stays on disk and is read only when needed.
nav_section: User Manual
nav_order: 45
slug: fork
---

Use **Fork thread** when a long conversation has filled the context window but you want to keep going. You get a fresh thread with almost no context. The old conversation is saved to a file, and the model reads parts of it only when it needs them.

Other options lose more: compaction summarizes old turns and drops detail, and a new thread starts with nothing. A fork costs about the same few tokens however long the old thread was.

:::tip
The fork becomes your new main thread. If the model needs a detail from the old conversation, it `read`s or `grep`s the transcript itself.
:::

## Forking

1. Press [[Ctrl+K]] and choose **Fork thread**.
2. Choose how [retrieval (RAG)](/docs/retrieval) should work in the new thread:

   | Choice | The fork's retrieval behaviour |
   |--------|--------------------------------|
   | **RAG per turn** | retrieve context before every turn |
   | **First-turn RAG** | retrieve once, up front, then stay quiet |
   | **RAG off** | no automatic retrieval (search tools still work) |

   [[↑↓]] / [[j]] [[k]] choose, [[Esc]] cancels.
3. Press [[Enter]]. The fork opens right away with a **⑃ Forked** card at the top, and a toast confirms it.

You can't fork an empty thread, or while the agent is mid-turn. A toast tells you why.

## What the fork carries

One message: the **fork note**. It shows as the **⑃ Forked** card and tells the model it's a fork and where the old transcript is:

```text
This conversation is a fork of an earlier one. Its full transcript is
saved at:
  ~/.agentty/threads/<parentId>.transcript.md
Read it with the `read` tool (or grep it) ONLY if you need earlier
context — don't read it pre-emptively.
```

The model always sees this note, so it knows the transcript exists. It only pays for the contents if it opens the file.

## The transcript file

When you fork, agentty writes a readable Markdown copy of the old thread to `~/.agentty/threads/<parentId>.transcript.md`, next to the thread files. It has `## user` / `## assistant` headers and the text, with each tool call shortened to one line. **Tool output is left out**, which keeps the file small and easy to grep.

The file has size limits, so even a huge thread gives a useful file:

- **512 KB total, newest first.** If the whole thread doesn't fit, the newest turns are kept and the oldest are replaced by a marker. The header still shows the real message count.
- **16 KB per message, start and end kept.** A huge pasted block is cut in the middle with a `… bytes elided …` marker.

The file is written atomically (temp file → `fsync` → `rename`), so a crash mid-fork can't leave it cut short.

The transcript is outside your workspace, so agentty lets the fork read **that one file** and nothing else in `~/.agentty/threads`. That permission is restored whenever the fork is opened again: in a new session, from the thread list, over ACP or with `agentty run --resume`.

:::note
A large transcript is fine. It's on disk, not in the context window. The model greps out the 2 KB it needs from a 500 KB file.
:::

## Fork vs. compaction

Both deal with a full context window, and they work together: you can fork a compacted thread, or compact a fork.

| | [Compaction](/docs/providers#1m-context-models) | Fork |
|---|---|---|
| Thread | **same** conversation | **new** thread (records its parent) |
| Old context | summary, kept in the window | full text on disk, read when needed |
| Up-front token cost | pays to summarize | almost none (just the note) |
| Detail kept | shortened to prose | **all of it**, the transcript is word for word |
| Use it to | keep *one* long thread going | start clean without losing the past |

## Provenance

Forking doesn't change the original thread. The new thread records which thread it came from. Both are ordinary thread files under `~/.agentty/threads/` (see [Threads & Persistence](/docs/threads)), and you can back up or delete either one on its own.
