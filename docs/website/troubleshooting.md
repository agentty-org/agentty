---
title: Troubleshooting
description: Common issues and how to resolve them.
nav_section: Help
nav_order: 10
slug: troubleshooting
---

Find the symptom you see, then follow the fix under it. If nothing here matches, [get a log](#reporting-a-bug) and open an issue.

## Connection and network

### A reply stops with `h2 socket hangup (server accepted request; not replayed automatically)`

The connection dropped after the provider had started answering. agentty does not retry at that point, because a replay could duplicate the reply or re-run tool calls.

The cause is almost always the network. Corporate proxies, VPNs and CDN edges often close idle HTTP/2 connections after 30-60 seconds. agentty keeps idle connections for 90 seconds, so it can reuse one the network already closed.

Check the log:

```bash
grep http.stream_hangup ~/.agentty/logs/agentty.log
```

```
W net  http.stream_hangup: host=api.anthropic.com:443 reused=1 idle_ms=61402 status=200 …
```

- **`reused=1` and `idle_ms` near a round number** (30000, 60000): a network idle timeout. Set `AGENTTY_POOL_IDLE_TTL` (seconds) below it:

  ```bash
  AGENTTY_POOL_IDLE_TTL=45 agentty      # evict before the network does
  AGENTTY_POOL_IDLE_TTL=0  agentty      # no pooling at all (safest, slower)
  ```

- **`idle_ms` small or random**: ordinary packet loss or a flaky link. Connection reuse is not the cause.

### I get certificate or TLS verification errors

You are probably behind a proxy that re-encrypts traffic. Install the proxy's CA into the system trust store. See [Corporate Proxies](/docs/proxies). As a last resort, `AGENTTY_INSECURE=1` skips verification. Don't use it on shared setups.

### Air-gap mode can't connect

- Check that *both* ends have OpenSSH 7.6 or newer.
- Put agentty on the remote `PATH`, or pass `--remote-agentty PATH`.
- Run `--setup` once so your credentials are copied to the remote.

See [SSH Air-gap](/docs/airgap).

## Sign-in and models

### agentty ignores my API key or login

Run `agentty status` to see which source agentty uses. The order is `--key` > `ANTHROPIC_API_KEY` > `CLAUDE_CODE_OAUTH_TOKEN` > on-disk credentials. An env var hides the credentials file.

### The ChatGPT model list is empty

ChatGPT sends fewer models, or none, to Codex CLI versions it considers old. Set `AGENTTY_CODEX_CLIENT_VERSION` to a newer Codex CLI version in plain `x.y.z` form, until agentty is updated. See [Environment](/docs/environment).

### I get `HTTP 429` or “quota exceeded”

agentty looks at how long the provider says to wait:

- **Short wait** (burst or per-minute limit): agentty waits and retries by itself. The status bar counts the attempts.
- **More than 15 minutes** (daily or plan quota): agentty shows the error at once, since retrying can't work before the reset.

The wait comes from the `Retry-After` header, or from the error body. Google Gemini uses the body (`"retryDelay": "6043s"`, “Please retry in 1h40m”).

To continue, wait for the reset, add billing or credit, or switch model (`^/`) or provider (`^P`). On Gemini's free tier, `limit: 0` means that model gets no free requests. See [Providers › Google Gemini](/docs/providers#google-gemini).

### I see “this model can't read images — retried without them”

The provider rejected an image: the model has no vision, the format isn't supported, there were too many images, or your organisation turned image input off. agentty removes the images and sends the turn again, so you still get an answer about the text. This covers pasted images and images from tools, such as `read` on a `.png`. Your transcript keeps the images. If you need the image read, switch to a vision model (`^/`).

## Local models

### A local model (llama.cpp / vLLM) doesn't respond

- **The address is missing `/v1`.** Most local servers only serve under `/v1`. A bare `localhost:8080` gets `/v1` added. A partial path like `localhost:8080/api` is used as written and will 404. Use the picker (`^P` → Custom host) so the toast shows the path, or check with `AGENTTY_LOG=wire=trace`.
- **The model id doesn't match.** `llama-server` serves exactly what its `/v1/models` lists (often the GGUF filename). If the id isn't listed, agentty says *“model X isn't served by this host — pick one (^/)”*. Open `^/` and pick a listed model.
- **The chat template rejects the request.** Some models (gemma, gpt-oss) reject agentty's system prompt or tools. agentty shows the server's message. Run `AGENTTY_LOG=wire=trace` to see the request and the rejection.
- **It is slow, not stuck.** A big model can read the prompt silently for minutes. The phase chip says *“processing…”*, and the idle timeout for local endpoints is 10 minutes. Press `Esc` to cancel.

See [Providers › Custom hosts](/docs/providers#custom-hosts).

### A local model says “I can't do all of that in one reply”

Small models sometimes refuse a big task. agentty's local prompt tells them to work step by step (see [Providers › The prompt local models get](/docs/providers#the-prompt-local-models-get)), but a ~20B model can still give up on a long spec. Ask for one part at a time, or use a bigger model.

## Tools and commands

### A command says Permission denied, or a tool is blocked

The sandbox may be blocking it. Change the sandbox in Settings → Sandbox, or start one run with `agentty --sandbox off`. See [Sandboxing](/docs/sandboxing).

### Search (search_code / search_docs) finds the wrong thing or nothing

Run `agentty diagnostics` and read the `retrieval (embeddings)` section.

- `probe: ok`: search by meaning is on. If a result is still wrong, describe what the code does, or let the model `grep` for an exact name.
- `probe: FAILED`: search is keyword-only. Start Ollama and pull a model: `ollama pull qwen3-embedding:0.6b`.
- `invalid: host …`: a typo in the Retrieval settings (for example `127.0.0`). Fix it in `/settings` → *Proactive retrieval (RAG)*.

`search_docs` finds nothing when there is no docs folder. Put docs in `./docs`, or set `AGENTTY_DOCS_DIR`. The first search in a repo is slower because it builds the index. See [Retrieval](/docs/retrieval#if-results-look-wrong).

### Ctrl+V pastes text instead of my image

Run `agentty paste doctor` in the same pane. It shows the settings in effect, every route it can see, and what a paste returns now.

Over **ssh**, the clipboard is on your laptop, not the server. Run this **once on the laptop**, then reconnect. It works in every terminal, tmux included:

```bash
agentty paste setup
```

Without it, agentty asks the terminal. Only **kitty** sends images back (OSC 5522). Other terminals send text only (OSC 52), so the paste arrives as text. On kitty, allow clipboard reads on the laptop and restart kitty:

```conf
# ~/.config/kitty/kitty.conf   (on the machine kitty runs on)
clipboard_control write-clipboard write-primary read-clipboard read-primary
```

Inside tmux, also run `tmux set -g allow-passthrough on`. Check Settings → Paste too: a route turned off there (or by an `AGENTTY_PASTE_*` variable) is skipped. See [Clipboard & Images](/docs/clipboard).

## The app itself

### agentty seems stuck after Esc

This is fixed in current builds (a cancelled worker could clear a new turn's cancel token). Update to the latest release. If it still happens, restart agentty and file a bug with your `git rev-parse HEAD` or release version.

Starting a new thread (`^N`) or switching threads during a reply stops that reply and its tools. Nothing from it lands in the thread you switched to.

### I see “wait for the thread to finish loading”

You pressed Enter while a thread was loading or a checkpoint rewind was running. agentty holds the message so it doesn't go into the thread you are leaving. Your draft stays in the box. Press Enter again when loading finishes.

### agentty uses a lot of disk, or settings and history won't save

- `agentty config` lists every folder agentty uses and its size.
- `agentty config doctor` checks each one is writable and private.
- Conversations are usually the largest. Move them with `agentty config move threads /some/big/disk` (then add the `export` line it prints), or expire old ones with `AGENTTY_THREADS_KEEP_DAYS=90`.
- `agentty config clean` lists files older versions left behind.

See [Storage](/docs/storage).

### The screen looks garbled

Some terminals lag on DEC 2026 synchronized output. File a bug with your `$TERM`, the terminal emulator name, and a screenshot.

## Getting a log for a bug report {#reporting-a-bug}

```bash
AGENTTY_LOG=debug agentty --log-file /tmp/agentty.log
# reproduce, then attach /tmp/agentty.log
```

- For a provider or network problem, add `AGENTTY_LOG=wire=trace` to capture the raw request and response bytes.
- `agentty diagnostics` writes a redacted bundle (build info, provider, log tail) you can attach.
- If agentty crashed, stderr already has a backtrace and the last ~256 events (the flight recorder).

See [Logging & diagnostics](/docs/logging#reporting-a-bug).

:::note
Something not covered here? [Open an issue](https://github.com/agentty-org/agentty/issues) with `$TERM`, your emulator, the version, and a screenshot or paste of the relevant part.
:::
