---
title: "What is an MCP terminal client? (and how to use one)"
description: "MCP (Model Context Protocol) lets AI agents use external tools. Learn what an MCP terminal client is and how agentty connects to MCP servers from the command line."
competitor: "MCP terminal client"
verdict: "agentty is an MCP-capable terminal coding agent: connect it to MCP servers to give the model real tools — file systems, databases, APIs — from the command line."
updated: "2026-10-28"
---

# What is an MCP terminal client?

The **Model Context Protocol (MCP)** is an open standard that lets AI models call external tools and data sources through a consistent interface. An **MCP terminal client** is a command-line agent that can connect to MCP servers and use those tools during a session.

agentty is exactly that — a native, terminal-first MCP client.

## Why MCP matters

Without MCP, an agent is limited to whatever its host wired in. With MCP, you can plug in standardized servers that expose:

- File systems and repositories
- Databases and search indexes
- Web APIs and internal services
- Custom company tooling

The model can then call these tools mid-task.

## Using MCP with agentty

1. Install agentty:

```
curl -fsSL https://agentty.org/install.sh | sh
```

2. Add an MCP server: `agentty plugin add`, or in the app press [[Ctrl+K]] → **Plugins** → [[a]]. Or list it in `~/.agentty/mcp.json` the same way Claude Desktop does. See [Plugins](/docs/plugins/).
3. Run `agentty` — the model can now invoke those tools during a conversation, with calls sandboxed by default.

## Servers with lots of tools

Some servers ship dozens of tools (Playwright, GitHub, Binary Ninja). Sending every schema on every request wastes context and makes the model pick worse, so agentty doesn't:

- Every tool stays **callable**, however many there are.
- Tools load while they fit (10% of the model's context by default); the rest are **on demand**. The model finds them with a `tool_search` tool, in any language and with typos, and a found tool stays loaded for the rest of the conversation.
- On Claude, this uses Anthropic's own deferred tool loading, so the prompt cache isn't broken. It works with a Claude Pro/Max subscription too.
- You choose: **auto**, **load all**, or **on demand**, and pin the tools you always want loaded. `/tools` shows what's loaded right now.

See [how tools load](/docs/plugins/#how-tools-load).

## Why a terminal MCP client

- **Native + fast** — a single binary, no runtime.
- **Sandboxed** — tool/shell calls are isolated by default.
- **Composable** — pair MCP tools with agentty's built-in retrieval.

See the [MCP docs](/docs/mcp/) for setup details.
