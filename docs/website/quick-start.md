---
title: Quick Start
description: Install agentty, connect a model, and ask your first question. About a minute.
nav_section: Getting Started
nav_order: 20
slug: quick-start
---

## 1. Install

On Linux or macOS:

```bash
{{installOneLiner}}
```

On Windows (PowerShell):

```powershell
{{installOneLinerWindows}}
```

Run the same line again later to update. Prefer a package manager? See [Installation](/docs/installation).

## 2. Start it in your project

```bash
cd your-project
agentty
```

agentty works inside the folder you start it in. It won't edit files outside it.

## 3. Connect a model

The first time, agentty asks how to connect. Pick whichever you already have:

| You have | Pick |
|---|---|
| Claude Pro or Max | **2) OAuth via claude.ai** |
| ChatGPT Plus or Pro | **3) Sign in with ChatGPT** |
| GitHub Copilot | **5) Sign in with GitHub Copilot** |
| Kimi | **6) Sign in with Kimi** |
| An API key (Anthropic, OpenAI, DeepSeek, Gemini, ...) | **1) Paste an API key** |
| Nothing, and you want it free and local | **4) Custom host**, then type `localhost:11434` (needs [Ollama](https://ollama.com)) |

Sign-ins open your browser. No API key needed for those.

:::tip
Already have a key in your shell, like `OPENAI_API_KEY`? agentty skips this step and opens the provider list instead. Pick that provider and press [[Enter]].
:::

## Ask something

Type what you want and press [[Enter]]:

```text
explain what this project does
```

agentty reads your code, edits files and runs commands to get it done. You see each step as it happens. Press [[Esc]] to stop it.

## Five keys worth knowing

| Key | What it does |
|---|---|
| [[Ctrl+K]] | Everything else: search any command |
| [[Ctrl+/]] | Switch model |
| [[Ctrl+R]] | Review the changes it made, keep or undo each one |
| [[Ctrl+J]] | Your past conversations |
| [[Shift+Tab]] | Ask before it edits or runs anything |

## Need full power? Turn the sandbox off

By default, commands run in a sandbox. Your project is writable, and your SSH keys, cloud credentials and `.env` files are hidden. Some commands won't work there: reaching `localhost`, writing outside your project, or tools that need your credentials.

If you need agentty to run anything you could run yourself, start it like this:

```bash
agentty --sandbox off
```

Commands can now reach every file your user can, and the network. agentty shows a **SANDBOX IS OFF** warning when it starts.

To also let its file tools (read, edit, write) work outside your project folder:

```bash
agentty --sandbox off --workspace /
```

These flags last for that run only. To make it permanent, add an alias to your shell:

```bash
alias agentty='agentty --sandbox off'
```

:::warn
With the sandbox off, and the default Write profile, agentty runs commands without asking. Press [[Shift+Tab]] to switch to Ask if you want to approve each one. Best for a machine or container you're happy to trust it with.
:::

Only need one extra folder? You can add it in [[Ctrl+K]] → Settings → Sandbox (read or write paths) and keep the rest of the sandbox. Reaching `localhost` or your local network always needs `--sandbox off`. See [Sandboxing](/docs/sandboxing).

## Next

- Used Claude Code or Codex before? Press [[Ctrl+J]]: your old conversations are there. [Coming from Codex or Claude Code](/docs/switching)
- [The interface](/docs/interface): what each part of the screen is
- [Providers & models](/docs/providers): every model it works with
