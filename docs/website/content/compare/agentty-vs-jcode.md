---
title: "agentty vs jcode: two fast native agent harnesses (2026)"
description: "agentty (C++26) vs jcode (Rust), measured: launch time, idle memory and disk on one machine, plus architecture, sandboxing, multi-agent swarms and editors."
competitor: "jcode"
verdict: "Both are fast, native, MIT-licensed harnesses; they launch in a statistical tie. jcode is built for running many agents at once through a background server. agentty is a single process that idles at {{bench.agentty.rss}} instead of {{bench.jcode.rss}}, is {{bench.jcode.diskX}} smaller on disk, and sandboxes every command by default."
updated: "2026-11-09"
---

# agentty vs jcode

jcode and agentty are the two native-code harnesses in our benchmark. jcode is written in Rust and built around a background server that hosts many agents at once. agentty is written in C++26 and runs as one self-contained process.

## Measured

Both installed the official way on the same machine, fresh home directory, no login. jcode's memory includes the background server it starts on first launch and leaves running. Median of 10 launches, measured {{bench.date}} against {{bench.jcode.version}}.

:::bench jcode

Launch times overlap across runs, so treat **ready to type** as a tie. Memory and disk are clear differences.

## At a glance

| | agentty | jcode |
|---|---|---|
| Built with | C++26 | Rust |
| Architecture | One process | TUI client + background server |
| Ready to type | {{bench.agentty.ready}} | {{bench.jcode.ready}} |
| Memory while idle | {{bench.agentty.rss}} | {{bench.jcode.rss}} |
| On disk | {{bench.agentty.disk}} | {{bench.jcode.disk}} |
| Sign in with a plan | Claude Pro/Max, ChatGPT, GitHub Copilot, Kimi | Claude, ChatGPT/Codex, Copilot and more |
| Shell sandbox | On by default, enforced by the OS | Permission prompts |
| Multi-agent | Subagents; Smart Mode routes work to cheaper models | Swarms of agents coordinated by the server |
| Editors | Zed and JetBrains over ACP | ACP |
| License | MIT | MIT |

## Where jcode is ahead

- **Many agents at once.** Run several agents in one repo; the server tells each when another changes files it has read, and agents can spawn their own teammates.
- **Cheap extra sessions.** Each extra session on the shared server costs only a few MB.
- **Extras.** Dictation, a desktop companion, browser automation, and self-development tooling.

## Where agentty is ahead

- **Lean single session.** {{bench.jcode.rssX}} less memory while idle, {{bench.jcode.diskX}} smaller on disk, and nothing left running when you quit.
- **Sandboxed by default.** Every shell command runs behind an OS-enforced wall; `~/.ssh`, `~/.aws` and `.env` are masked.
- **Retrieval.** A local keyword + embedding search engine picks the code that reaches the model.
- **Everywhere.** Linux and macOS on x86_64 and aarch64, Windows, Termux, SSH air-gap, scripts and CI.

## Which should you choose?

- **jcode** if you run swarms of agents in parallel and want them to coordinate.
- **agentty** if you want one fast, light, sandboxed agent that you can drop on any machine.

## Install in one line

```
curl -fsSL https://agentty.org/install.sh | sh
```
