---
title: "The best open-source Claude Code alternative (2026)"
description: "Looking for an open-source alternative to Claude Code? agentty is a native C++26 terminal coding agent — one static binary, millisecond startup, multi-provider, sandboxed, and MIT-licensed."
competitor: "Claude Code"
verdict: "agentty is a drop-in, open-source alternative to Claude Code: sign in with the same Claude Pro/Max plan, or point it at OpenAI, Groq, OpenRouter, Cerebras, or a local Ollama model — all from a single {{sizeMB}} native binary."
updated: "2026-11-01"
---

# The best open-source Claude Code alternative

If you like Claude Code's terminal-first workflow but want something **open source**, **faster**, and **not locked to a single provider**, agentty is built for exactly that.

## Why people look for a Claude Code alternative

- They want an **open-source** agent they can read, self-host, and contribute to.
- They want to **use providers other than Anthropic** — OpenAI, Groq, OpenRouter, Cerebras, or a fully local Ollama model.
- They don't want a **Node.js runtime** and `npm install` on every machine.
- They want **stronger isolation** — a sandbox around shell and build commands by default.

## How agentty compares

agentty keeps the part you like — a fast, keyboard-driven terminal agent — and changes the parts that matter for power users:

- **Single static binary.** {{sizeMB}}, starts in a few milliseconds, no Node, no Python, no Electron.
- **Bring your own model.** Sign in with your existing Claude Pro/Max, or use API keys for OpenAI, Groq, OpenRouter, Cerebras, or point it at local Ollama.
- **Sandboxed by default.** Shell and build calls run isolated; air-gap a whole session over SSH with one command.
- **Runs in Zed** over ACP, and speaks MCP.
- **Open source (MIT).**

## Install in one line

```
curl -fsSL https://agentty.org/install.sh | sh
```

No Node, no Python — just a single static binary.

## Is it really a drop-in replacement?

For the core loop — chat, read/edit files, run commands, retrieve from your repo — yes. If you depend on a Claude-Code-only feature the day it ships, keep Claude Code for that. For everyone who wants an open, fast, multi-provider agent, agentty is the alternative to try first.

## Keep your history

Switching doesn't mean starting over. agentty lists your Claude Code conversations in its thread list (Ctrl+J), and you can open one and continue it. Your `CLAUDE.md`, `.claude/` skills, subagents and slash commands are read from the same places, and `agentty mcp import --from claude` brings your MCP servers. See [Coming from Codex or Claude Code](/docs/switching).
