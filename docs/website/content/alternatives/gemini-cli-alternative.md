---
title: "An open-source Gemini CLI alternative (2026)"
description: "agentty is a native, open-source terminal coding agent and a Gemini CLI alternative — multi-provider (not just Gemini), one static binary, sandboxed, MIT-licensed."
competitor: "Gemini CLI"
verdict: "The Gemini CLI is built around Google's models. agentty runs Gemini too, plus Claude, GPT, Copilot, Kimi and local Ollama, from a single native binary with a sandbox by default."
updated: "2026-11-09"
---

# An open-source Gemini CLI alternative

Like the idea of a terminal coding agent but want **provider freedom** and an **open-source** codebase? agentty runs against many providers, not just one.

## How agentty compares

:::bench gemini

- **Provider-agnostic** — sign in with the Claude Pro/Max, ChatGPT, GitHub Copilot or Kimi plan you have, use an API key for OpenAI, DeepSeek, Gemini, Grok, Mistral, Groq or OpenRouter, or run a local model through Ollama.
- **Single native binary** — {{sizeMB}}, ready to type in {{bench.agentty.ready}}, idles at {{bench.agentty.rss}}.
- **Sandboxed by default**, SSH air-gap in one command.
- **Built-in retrieval** so only relevant code reaches the model.
- **Open source (MIT)**, MCP-capable, runs in Zed or JetBrains over ACP.

Full side-by-side: [agentty vs Gemini CLI](/compare/agentty-vs-gemini-cli).

## Install in one line

```
curl -fsSL https://agentty.org/install.sh | sh
```
