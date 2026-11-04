---
title: "How to run a sandboxed AI coding agent (safe by default)"
description: "AI agents run shell and build commands — that's risky. Learn how agentty sandboxes every command by default on Linux, macOS and Windows, and how to air-gap a session over SSH."
competitor: "sandboxed coding agent"
verdict: "agentty runs shell and build commands inside an OS-level sandbox by default on Linux, macOS and Windows, so an agent can't touch anything you didn't intend — and you can air-gap an entire session over SSH with one command."
updated: "2026-10-20"
---

# How to run a sandboxed AI coding agent

An AI coding agent that runs shell commands is powerful — and risky. A bad command (or a prompt-injected one) can delete files or exfiltrate data. agentty is built to be **safe by default**.

## Sandbox by default

agentty runs shell and build commands inside an OS-level sandbox, on **Linux, macOS and Windows**, using its own policy compiler ([claybin](https://github.com/agentty-org/claybin)). The agent's commands are isolated from the rest of your system unless you explicitly allow more access. You don't have to configure anything — it's on by default.

The same policy compiles to whatever the kernel underneath can actually enforce:

- **Linux** — mount + pid + network namespaces, a landlock ruleset, a seccomp filter, and cgroup v2 resource caps.
- **macOS** — a seatbelt profile (the mechanism Chrome and every App Store app use) plus POSIX rlimits.
- **Windows** — an AppContainer, a restricted token, a job object for the resource caps (as strong as cgroup v2 there), and process mitigation policies in place of a syscall filter.

agentty reports *per capability* which walls the host actually built, rather than claiming "sandbox: active" and leaving you to guess. Where a platform cannot enforce something — macOS and Windows have no equivalent of the Linux syscall filter — it says so instead of pretending. Apple's `sandbox-exec` remains as the macOS fallback; on Linux and Windows there is one engine.

## Control the network, down to the site

A sandboxed command never touches the network directly. It goes through agentty's own proxy, which refuses your local network, services on `localhost` and cloud metadata endpoints — so the internet works for `git push` and `npm install`, but a prompt-injected command can't reach your router or a database on your machine.

You choose how much further to go:

- **Allowed hosts** — list the sites commands may reach (`github.com`, `*.github.com`, `registry.npmjs.org`) and everything else is refused, including raw IP addresses. Paste a URL and it's trimmed to the host. The agent is told the list, so it picks an allowed mirror instead of retrying a blocked one.
- **`airgapped`** — one preset, no network at all, for working on code you don't trust.

Every preset survives a restart, and your own lists never knock it to `custom`.

## Air-gap a whole session

For maximum safety, agentty can air-gap an entire session over SSH with a single command — the agent operates on a remote machine with no path back to your local environment.

## Why this matters

- **Prompt-injection resistance** — a malicious instruction can't run arbitrary commands against your real system.
- **Blast-radius control** — mistakes are contained.
- **Peace of mind** — let the agent work autonomously without babysitting every command.

## Try it

```
curl -fsSL https://agentty.org/install.sh | sh
```

Sandboxing is enabled by default. See [Sandboxing](/docs/sandboxing) for details.
