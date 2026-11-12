---
title: Architecture
description: The update loop, the view function, the provider, and the subprocess model.
nav_section: Advanced
nav_order: 70
slug: architecture
---

This page explains how agentty's code fits together. There are three core pieces: a pure update loop, one render function, and a fixed set of effects.

:::tip
Wondering why it's C++26 and not Rust? See [Why modern C++ (not Rust)](/docs/why-not-rust) for the reasons, and [The Rust critique, answered](/docs/rust-critique) for the arguments against.
:::

## The update loop

All state changes go through one pure function: `(Model, Msg) → (Model, Cmd)`. Every message is handled, and you can inspect each transition. IDs have their own types (`ToolCallId`, `ThreadId`, `OAuthCode`, `PkceVerifier`), so passing two arguments in the wrong order is a compile error.

```text
Msg  →  reducer (one std::visit over a closed event sum)  →  (Model, Cmd)
Cmd  →  runtime executes side effects  →  new Msg events
```

## The view

Rendering is one function, `Model → Element`. agentty fills in widget Configs from the `Model`. [maya](https://github.com/agentty-org/maya), a sister TUI engine that is mostly headers, draws them: every glyph, layout choice and animation. agentty never builds Elements itself.

## The provider

The Anthropic provider talks HTTP/2 + SSE directly, using its own `nghttp2` + OpenSSL stack. OAuth (PKCE) and API keys both go through `auth::cmd_login`. Incoming text is shown at ⅛ of the buffer per tick, so server batching doesn't make it jumpy.

There are four transports in total:

- **Anthropic** (above).
- **OpenAI Chat Completions** covers every OpenAI-compatible backend: OpenAI, DeepSeek, Kimi, Google Gemini, xAI Grok, Mistral, Groq, OpenRouter, Together, Cerebras, Fireworks, GitHub Copilot and local Ollama. The differences are just config (base URL, auth header, model id).
- **Ollama's native `/api/chat`** also rescues tool calls that weaker local models leak as raw JSON, so they can still use every tool.
- **OpenAI's Responses API** is used for ChatGPT/Codex sign-in.

Code the transports share lives in one helper each, so a fix can't drift between copies. That covers stream framing, the end-of-stream event, Retry-After backoff, UTF-8 cleanup, the leaked-tool-call detector, token-usage parsing, OpenAI-family auth headers, the tool-schema encoder, the SSE anti-buffering headers and the reasoning-effort tier.

Reasoning works the same everywhere. Each wire format spells effort its own way (`thinking.budget_tokens` / `reasoning.effort` / `reasoning_effort`), but the value comes from one place. Streamed thinking (Anthropic thinking, DeepSeek/Grok/Gemini `reasoning_content`) arrives as the same event, so it looks the same on every provider. You pick the provider per session and can switch it live. See [Providers & Models](/docs/providers).

Background summaries and read-only `task` roles (explorer, reviewer) run on the cheapest capable model of the active provider, not the model you chat with. Auto-compaction triggers based on the model's real context window, not a fixed token margin. See [Providers & Models](/docs/providers#1m-context-models) and the *Compaction depth* command.

## The subprocess model

On POSIX, subprocesses use `posix_spawn` + `poll(2)`, and agentty enforces deadlines itself (`SIGTERM → SIGKILL`). On Windows they use `CreateProcessW` + a reader thread. agentty doesn't need GNU `timeout` and doesn't use `popen`, so there are no quoting problems. File writes are atomic: `write` + `fsync`/`_commit` + `rename`/`MoveFileExW`.

## The permission matrix

The permission policy is a `constexpr` matrix checked by `static_assert`s. Each tool declares its effects at compile time. If you change a cell in a way that weakens a guarantee, the build fails.

:::tip
For more detail, the repo's `docs/RENDERING.md` walks through the view pipeline step by step, and `docs/UI.md` is the Config reference for each widget. For how it stays fast (input-to-photon latency, render caching, connection warmth, speculative tool execution), see **[Performance](/docs/performance)**.
:::
