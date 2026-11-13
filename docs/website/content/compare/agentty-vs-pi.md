---
title: "agentty vs Pi: two minimal agent harnesses, measured (2026)"
description: "agentty (C++26) vs Pi (TypeScript), measured on one machine: launch time, idle memory and disk, plus extensibility, sandboxing, providers and editors, and when to pick each."
competitor: "Pi"
verdict: "Both are minimal, MIT-licensed harnesses that work with many providers. Pi is built to be reshaped: extensions, packages and an SDK, and it can rewrite itself on request. agentty is ready to type in {{bench.agentty.ready}} instead of {{bench.pi.ready}}, idles at {{bench.agentty.rss}} instead of {{bench.pi.rss}}, and sandboxes every command by default."
updated: "2026-11-13"
---

# agentty vs Pi

[Pi](https://pi.dev/) and agentty both call themselves agent harnesses and both keep their core small. Pi is written in TypeScript, runs on Node.js, and is designed to be extended: you change the harness itself with extensions, skills and packages, or ask Pi to write them. agentty is written in C++26 and ships as one binary with sandboxing, retrieval and subagents built in.

## Measured

Both installed the official way on the same machine, fresh home directory, no login. For Pi that is `pi.dev/install.sh`, which installs its npm package; Node.js and the ripgrep and fd binaries Pi downloads on first launch count toward its disk. Median of 10 launches, measured {{bench.date}} against Pi {{bench.pi.version}}.

:::bench pi

Pi also publishes a standalone binary on its GitHub releases. On the same machine it used less memory and disk than the npm install (about 95 MB idle, 134 MB on disk) but took slightly longer to be ready to type (about 0.7 s).

## At a glance

| | agentty | Pi |
|---|---|---|
| Built with | C++26, one static binary | TypeScript on Node.js 22.19+ |
| Ready to type | {{bench.agentty.ready}} | {{bench.pi.ready}} |
| Memory while idle | {{bench.agentty.rss}} | {{bench.pi.rss}} |
| On disk | {{bench.agentty.disk}} | {{bench.pi.disk}} |
| Sign in with a plan | Claude Pro/Max, ChatGPT, GitHub Copilot, Kimi | Claude Pro/Max, ChatGPT Plus/Pro, GitHub Copilot |
| Models | Claude, GPT, Gemini, DeepSeek, Grok, Mistral, Groq, OpenRouter, local Ollama, any OpenAI-compatible endpoint | 15+ providers, hundreds of models |
| Shell commands | Sandboxed by default, enforced by the OS | Run with your account's permissions, no approval step; sandbox it yourself (containers, Docker Sandboxes) |
| Extending it | Plugins, skills, hooks, MCP | Extensions, skills, prompt templates, themes, packages via npm or git; TypeScript SDK and RPC |
| Subagents and plan mode | Built in | Left out on purpose; add them with packages |
| Editors | Zed and JetBrains over ACP | Integrates through its RPC mode and SDK |
| License | MIT | MIT |

## Where Pi is ahead

- **Extensibility.** Almost everything is replaceable: tools, commands, providers, the UI. Extensions are TypeScript, and Pi can write and reload its own changes mid-session.
- **Packages.** Share and install extensions, skills and themes as Pi packages over npm or git.
- **Embedding.** A TypeScript SDK and an RPC mode make it easy to build Pi into other programs.

## Where agentty is ahead

- **Speed and footprint.** {{bench.pi.readyX}} faster to a usable prompt, {{bench.pi.rssX}} less memory while idle, {{bench.pi.diskX}} smaller on disk, with no Node.js to install.
- **Sandboxed by default.** Every shell command runs behind an OS-enforced wall, and `~/.ssh`, `~/.aws` and `.env` are masked. Pi's own docs say it runs commands with your permissions and leaves the boundary to you.
- **Built in, not bolted on.** Subagents, plan mode, local code retrieval and Smart Mode routing ship in the binary.
- **Editors.** Zed and JetBrains connect over ACP with no extra setup.

## Which should you choose?

- **Pi** if you want a harness to build on: you'd rather write the features you need, or install a package for them, than take someone else's defaults.
- **agentty** if you want a fast, light agent that is safe out of the box and already does subagents, planning and retrieval.

## Install in one line

```
curl -fsSL https://agentty.org/install.sh | sh
```
