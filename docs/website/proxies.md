---
title: Corporate Proxies
description: Make agentty work behind TLS-terminating forward proxies.
nav_section: Advanced
nav_order: 50
slug: proxies
---

Behind a SOCKS proxy, start agentty like this:

```bash
AGENTTY_SOCKS_PROXY=127.0.0.1:1080 agentty
```

Behind a proxy that re-encrypts traffic (Zscaler, Bluecoat, mitmproxy), install the proxy's CA certificate into the system trust store. The sections below explain both.

## SOCKS proxies

SOCKS does not terminate TLS. agentty checks certificates exactly as it does on a direct connection. This is also why [air-gap mode](/docs/airgap) is safe over an untrusted network.

:::note
agentty only uses its own `AGENTTY_SOCKS_PROXY=host:port` variable. It does **not** read `HTTP_PROXY`, `HTTPS_PROXY` or `NO_PROXY`, so a stray shell export can't reroute your API traffic. Air-gap mode sets the variable for you.
:::

## TLS-terminating proxies (Zscaler, Bluecoat, mitmproxy)

These proxies re-encrypt traffic with their own CA. Install that CA into the system trust store. agentty loads system roots at startup.

```bash
# Debian / Ubuntu
sudo cp corp-proxy-ca.crt /usr/local/share/ca-certificates/
sudo update-ca-certificates

# Fedora / RHEL
sudo cp corp-proxy-ca.crt /etc/pki/ca-trust/source/anchors/
sudo update-ca-trust
```

## Streaming through corporate VPNs

Some endpoint-security gateways keep the connection open but hold back model events. Short replies then arrive in one burst. Older agentty builds stopped larger tasks with `stream stalled — no events for 120s`. Current builds:

- ask for `no-cache, no-transform` and uncompressed event streams,
- count HTTP/2 control traffic as a sign the connection is alive, with a hard deadline still in place,
- fall back to incremental TLS HTTP/1.1 streaming when a gateway removes `h2` from ALPN.

If you see that exact 120-second error, update agentty. Replies may still arrive in bursts when company policy forces buffering. agentty can't make a gateway release bytes it holds back. The request keeps going instead of being cancelled, as long as the connection stays up.

## Last resort: turn off verification

If you can't install the CA, you can disable peer verification:

```bash
AGENTTY_INSECURE=1 agentty
```

:::warn
`AGENTTY_INSECURE=1` skips peer verification entirely. Anyone on the network path can pretend to be the API. Use it only as a temporary local workaround, and never hand it to other people.
:::
