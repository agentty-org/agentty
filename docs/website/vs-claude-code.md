---
title: agentty vs Claude Code
description: An honest comparison of agentty and Claude Code — the native C++26, single-binary, model-agnostic claude-code alternative. Startup speed, cost, sandboxing, air-gap, editor integration, and platform support side by side.
nav_section: Getting Started
nav_order: 15
slug: vs-claude-code
---

**Short version:** agentty is a drop-in [claude-code](https://github.com/anthropics/claude-code) alternative that targets the *same workflow*, a coding agent in your terminal, but ships as a {{sizeMB}} native **C++26** binary instead of a {{bench.claude.disk}} bundled JavaScript runtime. It is ready to type in **{{bench.agentty.ready}}** (Claude Code: {{bench.claude.ready}}), idles at **{{bench.agentty.rss}}** of memory (Claude Code: {{bench.claude.rss}}), sandboxes every shell call by default, and is **bring-your-own-model**: run it with an API key, a subscription sign-in (Claude Pro/Max, ChatGPT, GitHub Copilot, or Kimi), or a local Ollama model. It works with **any model** (Claude, GPT, DeepSeek, Kimi, Gemini, Grok, Mistral, Groq, OpenRouter, Together, Cerebras, Fireworks, or local Ollama), not Claude only.

If you already like Claude Code's workflow but want it faster, dependency-free, model-agnostic, and sandboxed, agentty is built for you.

**You don't start from scratch.** Your Claude Code conversations show up in agentty's thread list ([[Ctrl+J]]), and you can open one and keep going, on Claude or any other model. Your `CLAUDE.md` files, `.claude/` skills, subagents and slash commands are read from where Claude Code keeps them, and `agentty mcp import --from claude` copies your MCP servers. See [Coming from Codex or Claude Code](/docs/switching).

## Measured

Both installed the official way on the same machine, fresh home directory, no login. Median of 10 launches, measured {{bench.date}} against Claude Code {{bench.claude.version}}.

:::bench claude

## At a glance

| | agentty | Claude Code |
|---|---|---|
| **Runtime** | Single static C++26 binary | JavaScript app compiled into a native executable with its own runtime |
| **Launch → ready to type** | {{bench.agentty.ready}} | {{bench.claude.ready}} |
| **Memory while idle** | {{bench.agentty.rss}} | {{bench.claude.rss}} |
| **On disk** | {{bench.agentty.disk}}, one file | {{bench.claude.disk}} |
| **Models** | Claude, OpenAI, DeepSeek, Kimi, Gemini, Grok, Mistral, Groq, OpenRouter, Together, Cerebras, Fireworks, local Ollama, any OpenAI-compatible host | Claude only |
| **Auth** | Any provider API key, local Ollama (no key), *or* subscription OAuth (Claude Pro/Max, ChatGPT, GitHub Copilot, Kimi) | Claude Pro/Max OAuth or API key |
| **Shell sandbox** | On by default, enforced by the operating system | Permission prompts; an OS sandbox you can switch on |
| **Editor integration** | Runs inside Zed and JetBrains over [ACP](/docs/acp) | Terminal + IDE extensions |
| **Air-gapped hosts** | One-command [SSH air-gap](/docs/airgap) | — |
| **Platforms** | Linux and macOS (x86_64 + aarch64), Windows x86_64, Termux/Android | macOS, Linux, Windows |
| **License** | MIT (open source) | Proprietary |
| **MCP tools** | Yes, client and server | Yes |
| **Your Claude Code history** | Open any past Claude Code (or Codex) session and continue it, on any model | — |
| **Compaction cost** | Background summarization runs on the cheapest capable model on your provider | Not publicly documented |

## Where agentty is different

### It's a small native binary

Claude Code ships as a {{bench.claude.disk}} executable that carries its own JavaScript runtime. agentty is a single statically-linked C++26 executable ({{sizeMB}}): `curl | sh` and you have one file with **no runtime dependencies**. It is ready to type in {{bench.agentty.ready}}, prints `--version` in {{bench.agentty.start}}, and the TUI never pauses for garbage collection mid-stream. No version drift between machines, no `npm install`, no `node_modules`.

### Any model, bring your own key

agentty is model-agnostic — run it with an API key for **OpenAI, DeepSeek, Google Gemini, xAI Grok, Mistral, Groq, OpenRouter, Together, Cerebras, Fireworks, or Anthropic**, a subscription sign-in (**Claude Pro/Max, ChatGPT, GitHub Copilot, or Kimi** — no key), or a fully local **Ollama** model (no key, no cloud). Switch backends live in-app with `^P`, or pass `--provider`. See [Providers & Models](/docs/providers). If you want Claude Code's ergonomics with a local, zero-API-cost model, that's a one-flag change.

### Sandboxed by default, not as an afterthought

Every shell and build command agentty runs is wrapped in a sandbox the operating system enforces, so it holds even if the command tries to break out. Your project is writable, system libraries are read-only, and your SSH keys, cloud credentials and other projects aren't reachable at all. It's the default, not an opt-in; Claude Code has a sandbox too, but you have to turn it on. See [Sandboxing](/docs/sandboxing).

### Cost-aware by design

Background context compaction — the summarization pass that fires when a long thread nears the context window — runs on the **cheapest capable model on your active provider**, not the flagship model you're chatting with, so a compaction costs a fraction of a normal turn. The trigger itself scales with the model's real window (tunable 75/90/95% via the *Compaction depth* command) instead of a fixed token margin, so a big-window model rides much further before it needs to summarize at all. Read-only subagent fan-out (`explorer`, `reviewer`) gets the same treatment. The 1M-context window is an explicit, entitlement-gated model variant you opt into from the picker, never a silent auto-upgrade.

### Fork a thread to escape a full context window

When a conversation fills the window, agentty can **[fork](/docs/fork)** it into a *fresh* thread that carries **near-zero context**: the parent's full transcript is written to disk and the fork reads it on demand, so forking costs O(1) tokens no matter how large the parent grew — and nothing is lost, because the transcript is verbatim, not a lossy summary. Most agents that offer forking copy the whole history into the new session (inheriting its token cost); agentty inverts that into a read-on-demand pointer, turning fork from *"branch with the same context"* into *"reclaim the window and keep going."*

### Runs on air-gapped hosts

`agentty airgap user@host` relays traffic from your laptop to a machine with no direct internet, over SOCKS5-over-SSH with TLS pinned end-to-end. One command. See the [air-gap guide](/docs/airgap).

### Lives inside your editor over ACP

`agentty acp` runs the exact same engine as the TUI as an [Agent Client Protocol](/docs/acp) agent inside **Zed** or **JetBrains** IDEs: streaming responses, inline diffs, native permission prompts, session reload, over JSON-RPC on stdio. Any ACP client works.

## Where Claude Code is still ahead

Being honest: Claude Code is a mature, first-party Anthropic product with a large team behind it. It has broader IDE-extension coverage, a longer track record, and first-day support for new Anthropic features. agentty is pre-1.0 and moving fast — the core loop, tools, streaming, auth, and persistence all work and get daily testing, but treat it as a capable beta. If you want the officially-supported Anthropic experience above all else, use Claude Code.

## When to choose agentty

Choose agentty if you want:

- A **single-binary coding agent** with zero runtime dependencies.
- **Ready to type in {{bench.agentty.ready}}** and a TUI that never GC-pauses.
- **Your choice of model** — Claude, GPT, DeepSeek, Kimi, or a local Ollama model — behind one client.
- Shell calls **sandboxed by default**.
- To drive an agent on an **air-gapped host** over SSH.
- An **open-source (MIT)** tool you can read, fork, and ship.

## Try it in one line

```bash
curl -fsSL https://agentty.org/install.sh | sh
cd your-project
agentty
```

First launch opens auth — paste an API key (any provider), use a local Ollama model with no key, or sign in with your Claude Pro/Max subscription. See the [Quick Start](/docs/quick-start) or the full [Installation guide](/docs/installation).
