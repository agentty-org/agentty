---
title: Performance
description: How agentty stays fast — input-to-photon latency, render caching, connection warmth, and speculative tool execution.
nav_section: Advanced
nav_order: 75
slug: performance
---

This page explains what makes agentty fast and how to measure it yourself. Speed has two parts:

- **The local pipeline:** how fast a keystroke reaches the screen.
- **Turn time:** how long the real work in a turn takes.

## Input to photon

Each keystroke takes the same path: terminal read → event parse → reducer → view build → layout → render → diff → terminal write. On a modern machine the whole trip takes about **1 ms**, so the cursor moves in the same frame you press the key.

Four things keep it that fast:

- **Event-driven loop.** When nothing is animating, agentty renders only on input, a message or a timer. It never redraws on a fixed clock. An idle screen uses no CPU.
- **Visual-hash gate.** Before each render, agentty hashes the visible state. If nothing visible changed, it skips the frame: no layout, no paint, no bytes sent. Animations (the spinner, the streaming cursor, the welcome sigil) put their current step into the hash, so each step still draws.
- **Cross-frame cell cache.** Finished content (old turns, tool cards, the welcome wordmark) is keyed by a content hash. After the first paint it is copied from a cell cache instead of laid out again. A 3000-row transcript costs the same per frame as a short one.
- **No per-frame I/O.** The view function reads no files and does no crypto. Anything that needs a file (a credential check for a sign-in badge, a config file for a picker row) is cached by the file's modification time. The hot path pays for a stat, not a read and parse.

## Smooth streaming

Model output arrives in bursts. agentty shows it at a steady character rate instead of dumping each network chunk, so text appears like typing no matter how the server batches it. The first token has a fast path, so text starts the moment the first byte lands. The reveal runs on its own clock, so a slow frame doesn't make it stutter.

Opening a huge thread is fast too. agentty loads only the last few screens into the live view. Older messages are already in your terminal's own scrollback. A thread with thousands of messages opens as fast as a short one.

## Connection warmth

The handshake is often the slowest part of a request. agentty opens a TCP + TLS + HTTP/2 connection to the active provider *while you type*, so your first message skips it. TLS session tickets let a reconnect skip the full handshake. If the pooled connection expires while you're idle, the next keystroke warms it again, so the first request after a break is as fast as any other.

For Anthropic, agentty marks the stable start of the conversation (system prompt, tool definitions, earlier turns) as cacheable. The model reads it from cache instead of paying for it again each turn. On a long session this gives a faster first token.

## Speculative tool execution

In an agent session, the biggest cost is the number of model round-trips and the time spent waiting on tools. agentty overlaps that work:

- **Parallel batches.** Independent tool calls in one turn run at the same time. Only calls that really conflict wait for each other. See [Tools → parallel & speculative execution](/docs/tools#parallel--speculative-execution).
- **Speculative reads.** A read-only tool starts as soon as its arguments finish streaming, while the model is still writing the rest of the turn. File and search time hides inside the model's own generation.

Neither changes the result. Tool-heavy turns just finish sooner.

## Fast to start

Nothing slow runs before the first frame:

- The TLS context (which reads every system CA certificate) is built in the background on the first connection.
- Tool definitions are built the first time a tool is used, not at load time.
- Account and settings files are read once, not once per provider.
- Inside tmux, agentty asks tmux one question instead of four. Startup queries to the terminal go out together, so over SSH the first frame waits one network round-trip instead of two.

`AGENTTY_STARTUP_TRACE=/tmp/t.txt agentty` writes a timestamp for each startup stage, so you can see where the time goes on your machine.

## Light on memory

The conversation itself is small: a 10 MB thread takes about 16 MB once loaded. The cost is redrawing history into the terminal's scrollback when you open a thread or resize. agentty keeps that bounded:

- **It redraws only what your terminal keeps.** Appearance → **Scrollback** defaults to *auto*, the length of your terminal's own scrollback. Rows past that would be thrown away anyway. See [Scrollback](/docs/interface#scrollback).
- **It keeps nothing it can't reuse.** Each block's render data is freed right after the block is drawn. Scratch buffers last for one redraw, not the whole session.
- **Freed memory goes straight back to the OS**, on every thread, not only the main one.

Opening that 10 MB thread used about 670 MB before these changes and about 120–160 MB after (debug build, depending on the Scrollback level).

## Compiled for the machine

- **Optimized release builds** use link-time optimization and a stripped symbol table. A scripted profile-guided-optimization workload (`scripts/pgo-train.sh`) can train the binary on the real hot paths (boot, typing, picker navigation, redraw). That gives another single-digit-percent gain on streaming and layout.
- **Allocator.** Rendering and parsing allocate a lot. Local builds can use mimalloc (`-DAGENTTY_USE_MIMALLOC=ON`) for a measurable drop in keystroke latency.

## Measuring it yourself

Run with `AGENTTY_LOG=perf=debug`. The numbers go to the normal diagnostic log (`~/.agentty/logs/agentty.log`, or `--log-file <path>`):

| Event | What it tells you |
|-------|-------------------|
| `turn.cache` | **Cache hit ratio:** the share of your prompt prefix the model served from cache. High means fast, cheap turns. A sudden drop mid-session means something invalidated the cached prefix. |
| `turn.ttft` | **Time to first token**, per model: the time from sending a request to the first content byte. |
| `turn.tool_batch` | **Batch width:** how many tool calls the model sent in one round-trip. Wider batches mean fewer round-trips per task. |
| `thread.load` | Thread load and release timings, for when opening a thread felt slow. |

To watch cache behaviour across a session:

```bash
AGENTTY_LOG=perf=debug agentty
grep 'turn.cache\|turn.ttft' ~/.agentty/logs/agentty.log
```

Use `perf=trace` instead to get the per-frame lines (`stream.frame`, `view.shape`). They fire on every streaming frame, so they sit at `trace`, not `debug`. A logged event costs about 1.3 µs; a filtered one costs about 0.5 ns. Turn them on only while you look.

:::note This works on a release build
Nothing is compiled out. Every log site is in the shipped binary. The build type only changes the *default* level (`warn` in release, `trace` otherwise). `AGENTTY_LOG=perf=debug` on the release binary gives the same data as a debug build.
:::

These timings used to come from four separate environment variables (`AGENTTY_CACHE_PROF` and others), each writing its own file under `/tmp`. They now share the one log channel.

The maya render engine also reads `MAYA_FRAME_PROF=/path/to/log`, which records build, layout and paint timings for each frame. Use it to profile the view pipeline directly.
