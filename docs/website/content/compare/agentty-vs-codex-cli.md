---
title: "agentty vs Codex CLI: an honest comparison (2026)"
description: "agentty vs OpenAI's Codex CLI, measured: launch time, idle memory and disk on one machine, plus models, sandboxing, editors and switching, and when to pick each."
competitor: "Codex CLI"
verdict: "Codex CLI is OpenAI's open-source agent with a strong sandbox and first-party GPT support. agentty signs in with the same ChatGPT plan, also runs Claude, Copilot, Kimi and local models, and is ready to type in {{bench.agentty.ready}} instead of {{bench.codex.ready}} from a {{sizeMB}} binary."
updated: "2026-11-09"
---

# agentty vs Codex CLI

Both are open-source terminal coding agents with an OS-enforced sandbox. Codex CLI is OpenAI's agent, written in Rust and distributed through npm. agentty is a C++26 agent harness shipped as one static binary that works with any provider.

## Measured

Both installed the official way on the same machine, fresh home directory, no login. Codex's disk figure includes the Node.js runtime its npm install needs. Median of 10 launches, measured {{bench.date}} against {{bench.codex.version}}.

:::bench codex

## At a glance

| | agentty | Codex CLI |
|---|---|---|
| Built with | C++26, one static binary | Rust, installed through npm |
| Ready to type | {{bench.agentty.ready}} | {{bench.codex.ready}} |
| Memory while idle | {{bench.agentty.rss}} | {{bench.codex.rss}} |
| On disk | {{bench.agentty.disk}} | {{bench.codex.disk}} |
| Models | Claude, GPT, Copilot, Kimi, DeepSeek, Gemini, Grok, Mistral, Groq, OpenRouter, local Ollama | OpenAI models; local through Ollama or LM Studio |
| Sign in with a plan | ChatGPT, Claude Pro/Max, GitHub Copilot, Kimi | ChatGPT |
| Shell sandbox | On by default, enforced by the OS | On by default, enforced by the OS |
| Editors | Zed and JetBrains over ACP | VS Code extension; app server |
| License | MIT | Apache-2.0 |

## Where Codex CLI is ahead

- **First-party GPT.** Built by OpenAI alongside its models, with Codex Cloud tasks you can pull down and apply locally.
- **Sandbox modes.** Clear read-only, workspace-write and full-access modes, with approvals to match.

## Where agentty is ahead

- **Not tied to one vendor.** Keep your ChatGPT plan and also use Claude, Copilot, Kimi or a local model; switch mid-thread with `^P` and `^/`.
- **Footprint.** {{bench.codex.readyX}} faster to a usable prompt, {{bench.codex.rssX}} less memory while idle, {{bench.codex.diskX}} smaller on disk, with no npm or Node.js.
- **Retrieval.** A local search engine finds the code that matters, so turns stay small.
- **Smart Mode.** A flagship model plans; cheaper models do the routine work.

## Switching is painless

Your Codex sessions appear in agentty's thread list (Ctrl+J) and you can continue any of them, on GPT or any other model. `~/.codex/AGENTS.md` is read as-is, and `agentty mcp import --from codex` copies the MCP servers from your `config.toml`. Details in [Coming from Codex or Claude Code](/docs/switching).

## Which should you choose?

- **Codex CLI** if you only use OpenAI models and want Codex Cloud.
- **agentty** if you want the same kind of sandboxed agent with any model, in a lighter package.

## Install in one line

```
curl -fsSL https://agentty.org/install.sh | sh
```
