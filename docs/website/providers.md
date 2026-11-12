---
title: Providers & Models
description: Run agentty on Claude, ChatGPT, GitHub Copilot, Kimi, DeepSeek, Gemini, Grok, Mistral, Groq, Ollama, or any OpenAI-compatible endpoint.
nav_section: Getting Started
nav_order: 40
slug: providers
---

agentty works with Anthropic, local Ollama, and any OpenAI-compatible backend. This page shows how to connect each one. Most people sign in with a subscription they already have, or paste one API key.

## Pick a provider

| What you have | What to do |
|---|---|
| Claude Pro/Max | `agentty login` → **OAuth via claude.ai** |
| ChatGPT Plus/Pro | `agentty login` → **Sign in with ChatGPT** |
| GitHub Copilot | `agentty login` → **Sign in with GitHub Copilot**, then `agentty --provider copilot` |
| A Kimi plan | `agentty login` → **Sign in with Kimi**, then `agentty --provider kimi` |
| An API key (Anthropic, OpenAI, DeepSeek, Gemini, Grok, Mistral, Groq, OpenRouter, …) | set the key's env variable, then `agentty --provider <id>` ([table](#supported-providers)) |
| Your own server (llama.cpp, vLLM, LM Studio, a gateway) | `agentty --provider host:port` ([custom hosts](#custom-hosts)) |
| Nothing, and you want to stay local | install Ollama, then `agentty --provider ollama` ([local models](#local-models-ollama)) |

```bash
agentty login                                     # sign in with a subscription
agentty --provider kimi                           # Kimi K2 via your Kimi plan
agentty --provider deepseek -m deepseek-v4-pro    # DeepSeek (DEEPSEEK_API_KEY)
agentty --provider gemini -m gemini-2.5-pro       # Google Gemini (GEMINI_API_KEY)
agentty --provider xai -m grok-4.6                # xAI Grok (XAI_API_KEY)
agentty --provider groq -m llama-3.3-70b          # Groq
agentty --provider openrouter                     # any model via OpenRouter
agentty --provider ollama -m qwen2.5-coder        # local model, no key
agentty -m claude-opus-4-5                        # Claude (API key or Pro/Max OAuth)
```

`--provider` and `-m` are saved between runs. Pass them only when you want to change.

### Switching inside a thread

Press `^P` to switch provider and `^/` to switch model. There is no restart and no new sign-in; the next turn uses the new choice. Both are also in the command palette (`^K`). Both pickers have a live search filter: start typing (`kimi`, `grok`, `deepseek`…) to jump to a row.

## Supported providers

| ID | Backend | Key |
|---|---|---|
| `anthropic` | Claude: API key or Pro/Max OAuth | `agentty login` → Claude |
| `chatgpt` | Codex models: Sign in with ChatGPT (Plus/Pro) | `agentty login` → ChatGPT |
| `copilot` | GitHub Copilot models: Sign in with GitHub | `agentty login` → GitHub Copilot |
| `kimi` | Kimi K2 models: Sign in with Kimi | `agentty login` → Kimi |
| `openai` | GPT / o-series on `api.openai.com` | `OPENAI_API_KEY` |
| `deepseek` | DeepSeek V4 (chat + reasoner) on `api.deepseek.com` | `DEEPSEEK_API_KEY` |
| `gemini` | Google Gemini via the OpenAI-compatible API | `GEMINI_API_KEY` |
| `xai` | xAI Grok models on `api.x.ai` | `XAI_API_KEY` |
| `mistral` | Mistral / Codestral / Magistral on `api.mistral.ai` | `MISTRAL_API_KEY` |
| `groq` | Llama / Mixtral / Kimi on Groq LPUs (very fast) | `GROQ_API_KEY` |
| `cerebras` | wafer-scale inference (very fast) | `CEREBRAS_API_KEY` |
| `together` | open models on `together.ai` | `TOGETHER_API_KEY` |
| `fireworks` | open models on `fireworks.ai` | `FIREWORKS_API_KEY` |
| `openrouter` | any model via `openrouter.ai` | `OPENROUTER_API_KEY` |
| `ollama` | local models at `localhost:11434` | none |
| `host:port` | any raw OpenAI-compatible endpoint | `OPENAI_API_KEY` |
| `https://host[:port]/path` | any OpenAI-compatible endpoint with a custom path prefix (for example a gateway on `/api` instead of `/v1`) | `OPENAI_API_KEY` |

## API keys

Hosted OpenAI-compatible providers read their key from their own env variable (for example `GROQ_API_KEY`). If that is unset they fall back to `OPENAI_API_KEY`, or to `-k <key>` for the session. Ollama needs no key.

```bash
export GROQ_API_KEY=gsk_…
agentty --provider groq -m llama-3.3-70b

# or a one-off, never written to disk:
agentty --provider openai -k sk-… -m gpt-4o
```

See [Authentication](/docs/authentication) for every sign-in option and where credentials are stored.

## Sign in with GitHub Copilot

With a GitHub Copilot subscription (Individual, Business or Enterprise) you can use its models (GPT-4o, o-series, Claude, Gemini and more) with **no API key**.

```bash
agentty login          # choose "Sign in with GitHub Copilot"
agentty --provider copilot
```

1. agentty shows a one-time code and opens `github.com/login/device`.
2. Enter the code in any browser (this works over SSH too).
3. The model list is fetched live from your account, so you see exactly what your plan offers.

agentty stores a long-lived GitHub token, encrypted, at `~/.agentty/credentials/copilot_credentials.json`. It swaps it for the short-lived Copilot token and refreshes during the session, so long runs don't drop. `agentty status` shows your plan, entitlement and inference host. `agentty logout` → GitHub Copilot signs out.

:::note
Each Copilot plan uses a different host. agentty picks the right one; there is nothing to configure. On the free Copilot tier, agentty shows a clear "chat quota exhausted" message instead of a raw error.
:::

## Sign in with Kimi

With a [Kimi](https://www.kimi.com) plan you can run agentty on Kimi's K2 models with **no API key**.

```bash
agentty login          # choose "Sign in with Kimi"
agentty --provider kimi
```

Sign-in uses Kimi's OAuth device flow (RFC 8628):

1. agentty shows a one-time code and opens the Kimi authorization page.
2. Over SSH, press [[c]] to copy the **code** and [[u]] to copy the **URL**. Both go through OSC 52, so they land on your local clipboard. Paste them in any browser. [[o]] opens the browser again.
3. agentty polls in the background and switches over once you approve.

The token is stored encrypted at `~/.agentty/credentials/kimi_credentials.json` and refreshed automatically. The picker row shows your sign-in state (`⚠ sign in with Kimi` / `✓ signed in`). Press [[Enter]] on the active Kimi row to open the **account manager**, where you can switch, add or remove Kimi accounts. `agentty status` shows the active account; `agentty logout` → Kimi signs out.

:::tip
Kimi uses an OpenAI-compatible endpoint, so tool calls, streaming and reasoning work as on any other provider. To use a raw platform API key instead of the subscription, use a custom host: `agentty --provider https://api.moonshot.ai -k <key> -m kimi-k2-0905-preview`.
:::

## DeepSeek

[DeepSeek](https://platform.deepseek.com) is built in. It uses a **static API key** (DeepSeek has no developer OAuth). Set `DEEPSEEK_API_KEY`, pass `-k`, or paste the key into the in-app prompt once.

```bash
export DEEPSEEK_API_KEY=sk-…
agentty --provider deepseek -m deepseek-v4-pro      # general / coding model
agentty --provider deepseek -m deepseek-reasoner    # reasoning model (thinking)
```

- DeepSeek's endpoints have no `/v1` prefix; agentty handles that.
- Both chat and reasoner models call tools natively.
- On a reasoning model, the chain of thought streams live, shown like Claude's thinking. The effort control in the model picker maps straight to the API.
- Before you set a key, `^P` still shows a small bundled model list so you can pick one. The live list replaces it once the key is set.

## Google Gemini

Gemini uses an **API key** from [Google AI Studio](https://aistudio.google.com/apikey). Set `GEMINI_API_KEY` (or `GOOGLE_API_KEY`), pass `-k`, or paste it into the in-app prompt. Pasted keys starting with `AIzaSy` or `AQ.` are recognised as Gemini keys.

```bash
export GEMINI_API_KEY=AIza…
agentty --provider gemini -m gemini-2.5-pro
```

agentty uses Google's OpenAI-compatible endpoint (`generativelanguage.googleapis.com/v1beta/openai`).

:::note
There is **no Google sign-in**, so a Google AI Pro subscription can't be used here. As far as we know that plan doesn't add Gemini API quota either. The API key gets the API's own quota: the free tier, or pay-as-you-go once billing is on for the key's Google Cloud project.
:::

### Quota errors

On the free tier some models have a quota of zero, and the rest have small per-minute and per-day limits. When you hit one, Google answers `429` and says how long to wait. agentty reads that wait:

- **A short wait** (a per-minute limit) is retried automatically after the time Google asked for.
- **A long wait** (over 15 minutes, usually the daily quota) is shown straight away, because retrying can't succeed until the quota resets.

The message is Google's own, so it names the model and the quota. `limit: 0` means the free tier gives that model no requests at all: turn on billing for the key's project, or pick another model with `^/`.

## Local models (Ollama)

Run a model with Ollama on `localhost:11434`. No key, no cloud, and no data leaves your machine.

```bash
ollama pull qwen2.5-coder
agentty --provider ollama -m qwen2.5-coder
```

agentty uses Ollama's native `/api/chat` protocol. It also recovers tool calls that weaker local models print as raw JSON, so smaller models can still use all the tools.

:::note
`--provider` and `-m` are saved between sessions. Run `agentty --provider anthropic` to go back to Claude, or press `^P` in the app.
:::

## Your own server

### Custom hosts

Any server that speaks the OpenAI chat API works: llama.cpp, vLLM, LM Studio, a gateway, or a remote machine. You don't need a built-in row for it.

**From the TUI:** open `^P` and type the endpoint.

```
^P  →  yolo-auto.com

  Use yolo-auto.com  as a custom OpenAI-compatible host      ⏎ connect
  ──────────────────────────────────────────────────────────────────
  ⏎ connects to yolo-auto.com · agentty probes it and adopts the dialect it finds
```

As soon as the text looks like an endpoint (it has a dot, a `host:port`, or `https://`), the picker offers it as the **first** row. Press `Enter` to open the connect modal with your text already filled in. You can also pick `^P` → **Custom host…**. Searching also finds hosts you've already saved, so `gw.internal` jumps straight to that row.

**From the CLI:**

```bash
agentty --provider localhost:8080 -m my-model                       # local, no key
agentty --provider https://chat.example.org/api -k sk-… -m GLM-5.2
```

Saved hosts reappear in `^P` every session. A local host added without a key is saved too. To remove one, press `Del` on its row twice.

#### How agentty connects

- **It checks before switching.** agentty asks the server for its model list (your configured path, then `/v1/models`, then Ollama's `/api/tags`) and only switches if it answers. You get a toast such as `✓ 12 models · openai-compatible · 45ms · /v1/models`. If nothing answers, you go back to the input with the reason ("nothing listening — is the server running?") and your text kept. A dead endpoint never becomes your active provider.
- **It detects the dialect.** A bare Ollama server is recognised by `/api/tags`, so you don't need to know what your server speaks.
- **`/v1` is the default.** A bare host, `host:port`, or bare URL like `http://localhost:8080/` uses the `/v1` prefix: chat at `…/v1/chat/completions`, models at `…/v1/models`. A missing `/v1` is the most common local-server mistake.
- **Explicit paths are kept exactly.** If your gateway uses another prefix, put it in the URL:

  ```bash
  agentty --provider https://chat.example.org/api -k sk-…    # → /api/chat/completions, /api/models
  ```

- **Scheme and port.** `https://` (or a bare host on 443) uses TLS and asks for an API key. `http://` or any non-443 port uses plain HTTP and connects without a key.
- **Gateways keep a thread on one backend.** Every request to an OpenAI-compatible host sends an `x-session-id` header with the thread's id. Routing gateways (Bifrost, OpenRouter, LiteLLM) use it to send each turn where the last one went, so a fallback chain isn't walked every turn and the backend's prompt cache stays warm. Titles, compaction and subagents send their thread's id too. Hosts that don't know the header ignore it.

#### Several accounts on one endpoint

Add `#name` to keep several logins to the *same* server apart. Each keeps its own API key, saved model and picker row:

```bash
agentty --provider https://ollama.com/v1#work
agentty --provider https://ollama.com/v1#personal
```

The `#name` is a local label only and is never sent to the server. Claude, ChatGPT, Copilot and Kimi have their own in-app account manager instead; `#name` is for API-key custom hosts.

:::tip Debugging a custom host
Set `AGENTTY_LOG=wire=trace` to see the exact request path, status and response your server returned. See **[Logging & diagnostics](/docs/logging)**. The usual problems are a missing `/v1` (404) and a model id that doesn't match what `/v1/models` reports.
:::

### Known working endpoints

These hosted gateways work through the custom-host path as is. Type the spec into `^P` (or pass it to `--provider`), then paste the key when asked, pass `-k`, or set `OPENAI_API_KEY`.

| Endpoint | Spec | Key | Example model |
|---|---|---|---|
| [Requesty](https://docs.requesty.ai) | `https://router.requesty.ai/v1` (EU: `https://router.eu.requesty.ai/v1`) | [app.requesty.ai/api-keys](https://app.requesty.ai/api-keys) | `openai/gpt-4o-mini` |

```bash
agentty --provider https://router.requesty.ai/v1 -k <key> -m openai/gpt-4o-mini
```

## 1M-context models

Signed in with Claude Pro/Max OAuth, the model picker shows a **"(1M context)"** row under each Sonnet, Opus and Haiku 4+ model, for example `Claude Opus 4.8` then `Claude Opus 4.8 (1M context)`.

Picking the 1M row raises the context window agentty tracks for that model from 200K to 1M tokens. The status bar's context gauge and auto-compaction both use the larger limit, so a long session can grow much further before agentty summarises it. agentty sends Anthropic's extended-context beta for you; nothing else changes.

The 1M row is not offered with a raw API key. The beta depends on account tier, and a lower tier rejects requests over 200K with a 400.

When even 1M fills up, you can [fork](/docs/fork) the thread into a fresh one with almost no context (the parent transcript is read on demand) instead of compacting in place.

## Local model details

### Slow first token is normal

A large local model (20-30B on consumer hardware) reads the prompt **silently** before its first token. On a long prompt that can take minutes. On a local provider:

- the phase chip reads **"processing…"** (not "Streaming") until the first byte arrives;
- the idle timeout is 10 minutes for plain-HTTP endpoints (90 s for hosted APIs), so a slow model isn't mistaken for a hang;
- a reply can stream for as long as it needs (hosted APIs stop at 30 minutes).

`Esc` still cancels at once.

### When a local model won't respond

If prompts fail at once (not after a long think), agentty tells you why instead of retrying forever:

- **404 on every request**: the spec is missing its `/v1` prefix, or the model isn't loaded. The error names both. A custom host added without `/v1` is checked against `/v1/models` automatically.
- **Errors inside the stream**: llama.cpp reports failures as a mid-stream `event: error` under HTTP 200. agentty shows the server's actual message (a chat-template rejection, a bad alias) instead of looping.
- **Retries that get nowhere**: after 6 attempts with no model output, agentty stops with *"giving up after 6 failed attempts with no model output — check the server/model"*.
- **A stale saved model**: if the saved model id isn't in the host's live `/v1/models` list, agentty stops with *"model X isn't served by this host — pick one (^/)"* instead of failing every prompt with a 400.

When in doubt, `AGENTTY_LOG=wire=trace agentty` shows the exact request and response. See **[Logging & diagnostics](/docs/logging)**.

### The prompt local models get

A model on a server you run (loopback or your LAN, over plain HTTP) gets a short system prompt instead of the long hosted one. Small models follow a few firm rules better than pages of text. It tells the model to:

- do the work with tools: write files with `write`, change them with `edit`, run commands with the shell tool
- never paste code into its reply instead of calling a tool
- treat a big task as normal: work in many steps, one tool call at a time, and never refuse or apologise that a task is too large
- only say a file changed when a tool reported it

The main turn, [subagents](/docs/subagents) and [`agentty run`](/docs/cli#agentty-run) all get the same prompt. Even so, a small model (around 20B parameters or less) can still give up on a very large request, such as a long spec it must implement in full. Split the request into smaller steps, or use a bigger model.
