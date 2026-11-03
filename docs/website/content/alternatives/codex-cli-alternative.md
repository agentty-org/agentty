---
title: "An open-source alternative to OpenAI Codex CLI (2026)"
description: "agentty is a native, open-source terminal coding agent and an alternative to the OpenAI Codex CLI — one static binary, multi-provider (not just OpenAI), sandboxed, MIT-licensed."
competitor: "Codex CLI"
verdict: "The Codex CLI ties you to OpenAI. agentty runs against OpenAI too — plus Claude, Groq, OpenRouter, Cerebras, and local Ollama — from a single native binary with a sandbox by default."
updated: "2026-11-01"
---

# An open-source alternative to the OpenAI Codex CLI

If you like the Codex CLI's terminal workflow but want to **use more than one provider** and an **open-source** codebase, agentty is the alternative to try.

## Why look for a Codex CLI alternative

- You don't want to be **locked to a single provider**.
- You want an **open-source** agent you can read and self-host.
- You want a **native binary** with millisecond startup instead of a Node runtime.

## How agentty compares

- **Provider-agnostic** — OpenAI, Claude Pro/Max, Groq, OpenRouter, Cerebras, or local Ollama.
- **Single static binary** — {{sizeMB}}, starts in a few milliseconds.
- **Sandboxed by default**; one-command SSH air-gap.
- **Retrieval built in** so only relevant code is sent to the model.
- **Open source (MIT)**, MCP-capable, runs in Zed over ACP.

## Bring your Codex sessions with you

You don't lose your history by switching. agentty lists your Codex sessions in its thread list (Ctrl+J), and you can open one and keep going, on GPT or any other model. The turns, tool calls and patches show up the way agentty draws its own. Your `~/.codex/AGENTS.md` is read as-is, and `agentty mcp import --from codex` copies the MCP servers from your `config.toml`. Details in [Coming from Codex or Claude Code](/docs/switching).

## Install in one line

```
curl -fsSL https://agentty.org/install.sh | sh
```
