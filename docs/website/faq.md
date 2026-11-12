---
title: FAQ
description: Frequently asked questions about agentty — the C++26 claude-code alternative: auth, providers, platforms, sandboxing, air-gap, storage, and licensing.
nav_section: Help
nav_order: 20
slug: faq
---

Quick answers to the questions people ask most.

## Does it work with my Claude Pro/Max subscription?

Yes. You can sign in with OAuth on your Pro/Max plan, with no extra billing. Most people use an API key or a local Ollama model instead, since agentty works with any model. See [Authentication](/docs/authentication).

## Can I bring my Codex or Claude Code conversations?

Yes. They appear in the thread list ([[Ctrl+J]]) and you can continue them on any model. agentty also reads your `CLAUDE.md`, `AGENTS.md`, skills and slash commands where those tools keep them. `agentty mcp import` copies your MCP servers. Nothing in the other tool changes. See [Coming from Codex or Claude Code](/docs/switching).

## Will using my Claude subscription with agentty get my account banned?

We can't promise either way. agentty is a third-party client, with the same standing as any other. It uses the **same OAuth flow** and **`CLAUDE_CODE_OAUTH_TOKEN`** mechanism as Claude Code, sends normal Messages-API requests over HTTPS, and does not spoof, scrape or get around rate limits. Credentials stay in `~/.agentty/credentials/credentials.json` (mode `0600`) and go only to Anthropic.

We're not affiliated with Anthropic, and their terms can change. For **zero doubt**, use an `ANTHROPIC_API_KEY` (pay-as-you-go) or pick [another provider](/docs/providers) with `--provider`. See [Authentication](/docs/authentication).

## Is it really a drop-in for claude-code?

Mostly. It is a coding agent in your terminal, shipped as one native binary, and it can use the same Claude sign-in. Unlike Claude Code, it also works with other models. Besides Claude it runs against OpenAI, DeepSeek, Kimi, Google Gemini, xAI Grok, Mistral, Groq, OpenRouter, Together, Cerebras, Fireworks, and local Ollama models. See the full [agentty vs Claude Code](/docs/vs-claude-code) comparison and [Providers & Models](/docs/providers).

## Can I use models other than Claude?

Yes. Pass `--provider` to point agentty at OpenAI, DeepSeek, Kimi, Google Gemini, xAI Grok, Mistral, Groq, OpenRouter, Together, Cerebras, Fireworks, a local Ollama model, or any raw `host:port`. It persists like `-m`, and you can switch backends live in-app with `^P`. See [Providers & Models](/docs/providers).

### Can I use Kimi or DeepSeek?

Yes, both are fully supported. **Kimi**: run `agentty login`, pick Kimi and sign in with your Kimi plan (no API key), then run `agentty --provider kimi`. **DeepSeek**: set `DEEPSEEK_API_KEY` and run `agentty --provider deepseek -m deepseek-v4-pro`. Use `deepseek-reasoner` for the thinking model; its reasoning streams live. See [Providers & Models](/docs/providers#sign-in-with-kimi).

## Do I need Node or Python?

No. agentty is a single static C++26 binary. No Node runtime, no `npm install`, no Python, no Electron.

## What platforms are supported?

Linux, macOS and Windows, all built and tested daily. Prebuilt binaries ship for Linux (x86_64, aarch64) and Windows (x86_64); macOS builds from source in seconds. It also builds natively on **Termux/Android** (no root, no proot) — see [Installation](/docs/installation#termux--android).

## How is it sandboxed?

By the OS kernel. Every shell and build command runs in a kernel-enforced sandbox: the workspace is read-write, system libraries are read-only, and `~/.ssh`, `/etc` and other projects are blocked.

- Linux: namespaces, Landlock, seccomp and cgroup2.
- macOS: seatbelt and rlimits.
- Windows (since 0.9.29): AppContainer, restricted token, job object and mitigation policies. If the host can't build a sandbox, agentty says `sandbox: unavailable, running unsandboxed`. There is no weaker fallback.
- Unrooted Android/Termux runs unsandboxed.

See [Sandboxing](/docs/sandboxing).

## Can I limit which websites commands can reach?

Yes. List them in **Settings → Sandbox → Network → Allowed hosts**, e.g. `github.com` or `*.npmjs.org`. `*.x` matches only subdomains, so list `x` too for the site itself. Leave it empty to allow any public site, or pick the `airgapped` preset for no network. Commands always go through agentty's proxy, which blocks your local network, `localhost` and cloud metadata addresses. See [Sandboxing](/docs/sandboxing).

## Can I run it on a machine with no internet?

Yes. `agentty airgap user@host` relays traffic from your laptop over SOCKS5-over-SSH, with TLS pinned end-to-end. See the [air-gap guide](/docs/airgap).

## Can I use it inside my editor?

Yes. `agentty acp` runs agentty as an [Agent Client Protocol](/docs/acp) agent in Zed or any other ACP client. You get streaming answers, inline diffs, native permission prompts and session reload. It is the same engine as the TUI, over JSON-RPC on stdio.

## Does code search send my code anywhere?

No. `search_code` and `search_docs` run on your machine. Keyword search needs nothing; search by meaning uses a local [Ollama](https://ollama.com) model (`ollama pull qwen3-embedding:0.6b`), which agentty finds by itself. The index is stored in your project's `.agentty/cache/`. See [Retrieval](/docs/retrieval).

## Where are my conversations stored?

In plain files under `~/.agentty/threads/`: one append-only `.jsonl` log per thread, plus small index files. You can read, back up or delete them. Move them with `AGENTTY_THREADS_DIR`, expire old ones with `AGENTTY_THREADS_KEEP_DAYS`. `agentty config` shows every folder agentty uses and its size; [Storage](/docs/storage) has the full map.

## What happens when a conversation runs out of context?

agentty summarizes it automatically, or you can fork it.

- **Auto-compact**: as a thread nears the limit, agentty summarizes older turns on a cheap model and keeps going in the same thread. Some detail is lost.
- **[Fork](/docs/fork)** ([[Ctrl+K]] → *Fork thread*): starts a fresh thread with almost no context. The full old transcript is saved to disk, and the model can read it when needed.

To stop auto-compaction, turn off Settings → **Auto-compact** (or set `"auto_compact": false` in `settings.json`). Then only `/compact` summarizes. Requests are still trimmed to fit the window, so the oldest turns drop out of what the model sees instead of failing.

## Is it stable / production ready?

Not yet: it's pre-1.0 and changes fast. The core loop, tools, streaming, sign-in and saved threads all work and are smoke-tested daily on Linux. Treat it as a capable beta.

## What license is it under?

MIT. Use it, fork it, ship it. See the [license page](/license).

## How do I fix or improve these docs?

Open a PR. Every page is a Markdown file under `docs/website/` in the agentty repo. Click **Edit this page** at the bottom of any doc to get there. The site rebuilds on merge.
