---
title: agentty vs Claude Code
description: How agentty compares with Claude Code on startup, memory, models, sandboxing, air-gap, editor support and platforms.
nav_section: Getting Started
nav_order: 60
slug: vs-claude-code
---

This page compares agentty with [Claude Code](https://github.com/anthropics/claude-code), so you can decide which fits you. Both are coding agents in your terminal with the same basic workflow.

**Short version:** agentty is a {{sizeMB}} native **C++26** binary instead of a {{bench.claude.disk}} bundled JavaScript runtime. It is ready to type in **{{bench.agentty.ready}}** (Claude Code: {{bench.claude.ready}}) and idles at **{{bench.agentty.rss}}** of memory (Claude Code: {{bench.claude.rss}}). It sandboxes every shell call by default, and it works with **any model**: Claude, GPT, DeepSeek, Kimi, Gemini, Grok, Mistral, Groq, OpenRouter, Together, Cerebras, Fireworks or local Ollama. You can use an API key, a subscription sign-in (Claude Pro/Max, ChatGPT, GitHub Copilot or Kimi), or a local model with no key.

**You don't start from scratch.** Your Claude Code conversations show up in agentty's thread list ([[Ctrl+J]]) and you can continue them, on Claude or any other model. Your `CLAUDE.md` files and `.claude/` skills, subagents and slash commands are read from where Claude Code keeps them, and `agentty mcp import --from claude` copies your MCP servers. See [Coming from Codex or Claude Code](/docs/switching).

## Measured

Both installed the official way on the same machine, with a fresh home directory and no login. Median of 10 launches, measured {{bench.date}} against Claude Code {{bench.claude.version}}.

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

### A small native binary

agentty is one statically linked C++26 file ({{sizeMB}}) with **no runtime dependencies**. Install with `curl | sh`; there is no `npm install` and no `node_modules`. It is ready to type in {{bench.agentty.ready}}, prints `--version` in {{bench.agentty.start}}, and the TUI never pauses for garbage collection mid-stream.

### Any model

Use an API key for **OpenAI, DeepSeek, Google Gemini, xAI Grok, Mistral, Groq, OpenRouter, Together, Cerebras, Fireworks or Anthropic**, a subscription sign-in (**Claude Pro/Max, ChatGPT, GitHub Copilot or Kimi**, no key), or a local **Ollama** model (no key, no cloud). Switch in the app with `^P`, or pass `--provider`. See [Providers & Models](/docs/providers).

### Sandboxed by default

Every shell and build command runs in a sandbox the operating system enforces, so it holds even if the command tries to break out. Your project is writable, system libraries are read-only, and your SSH keys, cloud credentials and other projects can't be reached. Claude Code has a sandbox too, but you have to turn it on. See [Sandboxing](/docs/sandboxing).

### Lower background cost

- **Compaction** (the summary made when a long thread nears the context limit) runs on the **cheapest capable model on your provider**, not the model you're chatting with.
- **The compaction trigger** follows the model's real window (75/90/95%, set with the *Compaction depth* command), so a big-window model goes much further before summarising.
- **Read-only subagents** (`explorer`, `reviewer`) also go to a cheaper model by default.
- **1M context** is a model variant you pick on purpose in the picker, never a silent upgrade.

### Fork to escape a full context window

When a conversation fills the window, you can **[fork](/docs/fork)** it into a fresh thread with almost no context. The parent's full transcript is saved to disk and the fork reads it on demand. Forking costs about the same few tokens however large the parent was, and nothing is lost because the transcript is kept word for word, not summarised.

### Runs on air-gapped hosts

`agentty airgap user@host` relays traffic from your laptop to a machine with no direct internet, over SOCKS5-over-SSH with TLS pinned end to end. See the [air-gap guide](/docs/airgap).

### Works inside your editor over ACP

`agentty acp` runs the same engine as the TUI as an [Agent Client Protocol](/docs/acp) agent inside **Zed** or **JetBrains** IDEs: streaming replies, inline diffs, native permission prompts and session reload, over JSON-RPC on stdio. Any ACP client works.

## Where Claude Code is ahead

Claude Code is Anthropic's own product with a large team behind it. It has wider IDE-extension coverage, a longer track record, and day-one support for new Anthropic features. agentty is pre-1.0: the core loop, tools, streaming, auth and persistence all work and are tested daily, but treat it as a capable beta. If you want the officially supported Anthropic experience above all, use Claude Code.

## When to choose agentty

Choose agentty if you want:

- a **single-binary coding agent** with no runtime dependencies;
- **ready to type in {{bench.agentty.ready}}**, with no GC pauses;
- **your choice of model** (Claude, GPT, DeepSeek, Kimi, or local Ollama) behind one client;
- shell calls **sandboxed by default**;
- to run an agent on an **air-gapped host** over SSH;
- an **open-source (MIT)** tool you can read, fork and ship.

## Try it

```bash
curl -fsSL https://agentty.org/install.sh | sh
cd your-project
agentty
```

On first launch, paste an API key (any provider), use a local Ollama model with no key, or sign in with a subscription. See the [Quick Start](/docs/quick-start) or the full [Installation guide](/docs/installation).
