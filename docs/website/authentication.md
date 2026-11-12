---
title: Authentication
description: Each way to sign in, the credential override order, and where credentials are stored.
nav_section: Getting Started
nav_order: 45
slug: authentication
---

This page covers every way to sign in to agentty and where your credentials are kept. The short version is in [Quick Start](/docs/quick-start#3-connect-a-model). Most people pick a sign-in option the first time agentty asks and never come back.

## The sign-in options

On first run (or with `agentty login`) a modal lists these options. Press the number to choose:

| # | Option | Use it when |
|---|--------|-------------|
| 1 | **Paste an API key** | you have a key from any provider (pay as you go) |
| 2 | **OAuth via claude.ai** | you have Claude Pro or Max |
| 3 | **Sign in with ChatGPT** | you have ChatGPT Plus or Pro |
| 4 | **Custom OpenAI-compatible host** | you run your own endpoint ([custom hosts](/docs/providers#custom-hosts)) |
| 5 | **Sign in with GitHub Copilot** | you have a Copilot subscription |
| 6 | **Sign in with Kimi** | you have a Kimi plan |

For a local model with no account or key, use [Ollama](/docs/providers).

## API key

Paste an `sk-ant-…` key (or any provider's key) into the modal, or set the provider's environment variable. This is pay-as-you-go, clearly within each provider's terms, and the same key works headless and over SSH.

The key is saved to `~/.agentty/credentials/credentials.json` at mode `0600`. Set `AGENTTY_CREDENTIALS_DIR` to move that folder.

## Claude Pro/Max (OAuth)

agentty uses the **same OAuth flow and `CLAUDE_CODE_OAUTH_TOKEN` mechanism as Claude Code**.

1. Choose **OAuth via claude.ai**.
2. Your browser opens. Approve the sign-in.
3. Paste the callback code back into agentty.

The token goes into the same credentials file. There is no extra billing; it's the same account. agentty is a third-party client on subscription auth; see the [FAQ](/docs/faq) for what that means. If you want no ambiguity, use an API key or Ollama.

## ChatGPT Plus/Pro (OAuth)

Choose **Sign in with ChatGPT** to use your subscription through agentty's Codex provider.

- **Local terminal:** the browser returns to `http://localhost:1455`.
- **Over SSH:** agentty switches to OpenAI's device-code flow. Open the `auth.openai.com/codex/device` link on any device and enter the one-time code. The server needs no browser or callback port.

Device login must be enabled in your ChatGPT security settings, or by your workspace admin.

| `AGENTTY_CHATGPT_DEVICE_AUTH` | Effect |
|---|---|
| `1` | always use device auth (for unusual headless terminals) |
| `0` | always use loopback login (for example with `ssh -L 1455:localhost:1455`) |

## GitHub Copilot (OAuth)

Choose **Sign in with GitHub Copilot** to use your Copilot models with no API key. This is GitHub's device flow:

1. Run `agentty login` and choose GitHub Copilot.
2. agentty shows a one-time code and opens `github.com/login/device`.
3. Enter the code and approve.

agentty picks the right inference host for your plan (Individual, Business or Enterprise) and refreshes the token during the session.

## Kimi (OAuth)

Choose **Sign in with Kimi** to run Kimi's K2 models with your [Kimi](https://www.kimi.com) plan and **no API key**. This is the OAuth device flow (RFC 8628):

1. Run `agentty login` and choose Kimi.
2. agentty shows a one-time code and opens the Kimi authorization page.
3. Over SSH, press [[c]] to copy the code to your local clipboard (OSC 52) and paste it in any browser.
4. agentty polls and switches over once you approve.

The token is stored encrypted and refreshed automatically. You can hold several Kimi accounts and switch between them in the app. Full walkthrough: [Providers & Models](/docs/providers#sign-in-with-kimi).

## Override order

When more than one credential is present, agentty uses the first one found (highest priority first):

1. `-k <key>` / `--key <key>`: this session only, never written to disk.
2. `ANTHROPIC_API_KEY` environment variable.
3. `CLAUDE_CODE_OAUTH_TOKEN` environment variable.
4. The credentials saved from the modal.

## Other providers

With `--provider`, agentty reads that provider's key from its environment variable (for example `OPENAI_API_KEY`, `DEEPSEEK_API_KEY`, `GEMINI_API_KEY`, `GROQ_API_KEY`). It falls back to `OPENAI_API_KEY`, or to `-k` for the session. Ollama needs no key. A key entered in the app is saved per provider, so you paste it once. See [Providers & Models](/docs/providers).

## Sign in from the command line

```bash
agentty login     # ChatGPT / Copilot / Kimi print a device URL + one-time code
agentty logout    # clear stored credentials
agentty status    # show which auth source will be used
```

## How credentials are stored

:::warn
All credential files are **encrypted at rest** with AES-256-GCM, tied to this machine, and written atomically (temp file, fsync, rename) at mode `0600`:

- `credentials.json`: the active token
- `accounts.json`: every signed-in account
- `provider-keys.json`: one API key per provider and custom host

Since 0.9.1, provider keys live here instead of in plain text in `settings.json`. Upgrades move them automatically on first load.

Encryption is tied to the machine, so it protects a stolen file, not a compromised account: anything running as your user can still ask agentty to decrypt. For stronger protection, add a passphrase (`AGENTTY_ENCRYPT_PASSPHRASE=1`) or use the OS keystore (`AGENTTY_USE_KEYSTORE=1`).
:::
