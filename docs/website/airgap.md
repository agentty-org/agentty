---
title: SSH Air-gap
description: Run agentty on a host with no direct internet, relaying bytes over SSH.
nav_section: Advanced
nav_order: 10
slug: airgap
---

Run this on the laptop that has internet:

```bash
agentty airgap --setup user@airgapped-host    # first time: also copies your credentials
agentty airgap user@airgapped-host            # every time after
```

agentty then runs on the remote box, which can't reach the internet itself. Your laptop relays the bytes. TLS is checked against the real upstream servers, so the network in between can't read or alter the traffic.

## What `--setup` does

`--setup` copies your local `credentials.json` (from `AGENTTY_CREDENTIALS_DIR`, default `~/.agentty/credentials/`) to `~/.agentty/credentials/` on the remote, with mode 0600.

## How it works

`ssh -R 1080` opens a SOCKS5 proxy on the remote at `localhost:1080`. Connections to it travel back over SSH and your laptop dials them. The remote agentty gets `AGENTTY_SOCKS_PROXY=localhost:1080` and sends every TCP connection through it: chat, OAuth refresh, `web_fetch`, `web_search`. You set one env var and don't list hosts.

## Without the wrapper

You can run the same thing with plain `ssh`:

```bash
ssh -t -R 1080 user@airgapped-host \
    'AGENTTY_SOCKS_PROXY=localhost:1080 agentty'
```

Both ends need OpenSSH 7.6 or newer (October 2017; every current distro has it).

Options for `agentty airgap`:

- `AGENTTY_AIRGAP_SSH` passes extra flags to `ssh` (for example `-i`, `-p`, `-J`).
- `--remote-agentty PATH` sets where agentty lives on the remote, if it isn't on the remote `PATH`.

:::warn Trust model
Air-gap mode doesn't trust the network between laptop and remote. It *does* trust the remote with your tokens: `--setup` copies `credentials.json` there at mode `600`. A compromised remote can steal your Anthropic credentials no matter how safe the tunnel is. Use it only on hosts you'd already trust with that secret.
:::

## Run it inside Zed (ACP over airgap)

The same tunnel can run agentty as an editor agent. `agentty airgap user@remote --acp [flags…]` prints a Zed `agent_servers` block you can paste. Its `command` is `ssh` itself. That one process is the SOCKS tunnel, the remote `agentty acp`, and the JSON-RPC transport, and Zed owns it. For roles, prerequisites and troubleshooting, see [Use agentty inside Zed → air-gapped remote](/docs/acp#airgap).

## Pasting images on the remote

If you ran `agentty paste setup` on this laptop, every ssh session already carries your clipboard, air-gap included. [[Ctrl+V]] on the remote pastes screenshots and files.

If you didn't, add `--clipboard-relay` for one session:

```bash
agentty airgap --clipboard-relay user@airgapped-host
```

This starts the clipboard agent on the laptop if it isn't running. It also adds one `-R` forward from a new `/tmp/richpaste-<user>-<random>.sock` on the remote to that agent. Paste on the remote finds the socket by itself. Nothing is installed on the remote, and the laptop doesn't need sshd. See [Clipboard & Images](/docs/clipboard).
