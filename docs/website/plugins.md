---
title: Plugins (MCP servers)
description: Extend agentty with external tools by adding MCP servers — a browser driver, a database client, a hosted API — from a single mcp.json or the agentty plugin CLI.
nav_section: Advanced
nav_order: 35
slug: plugins
---

This page shows how to add a plugin: an [MCP](https://modelcontextprotocol.io) server that gives agentty new tools. Add a Playwright server and the model can drive a browser. Add a Postgres server and it can query your database. No rebuild needed.

The quickest way is [[Ctrl+K]] → **Plugins** → [[a]], or `agentty plugin add` from a shell. For the protocol, OAuth and serving agentty's own tools, see [MCP Server](/docs/mcp). To write your own server, see [Build a Plugin](/docs/build-a-plugin).

The TUI and the `mcp.json` file edit the same config. Use the **TUI** for common jobs (add, enable, disable, remove, approve). **Edit `mcp.json`** when you need env vars, headers, per-tool filters or timeouts.

## Where plugins live

agentty reads **both** files and uses the servers from each. If both have a server with the same name, the project one wins.

- `~/.agentty/mcp.json` — **user** scope: your servers, on every project.
- `<project>/.agentty/mcp.json` — **project** scope: committed with the repo, shared with your team.

Set `$AGENTTY_MCP_CONFIG` to a file path to use that file instead of both. A project `mcp.json` can come with a cloned repo, so its servers that run commands need your approval first. See [Trust](#config-scope).

:::note
If you have no `mcp.json`, plugins cost nothing at startup. agentty checks for the file and moves on.
:::

## Configure from the TUI

Open the command palette with [[Ctrl+K]] and choose **Plugins**. The picker lists every configured server (from both scopes), its live connection state, and its tools. It reads the same state the agent uses, so it's always current. No restart is needed for anything you do here.

**What each row shows**

- A health badge: **●** connected · **◌** connecting · **⚠** error / needs attention · dim = disabled.
- A scope tag on non-user rows (`project · …`); your own user servers are unbadged.
- The tool count and, expanded beneath a connected server, each tool with its own on/off state (dimmed when the parent is off).

**Keys**

| Key | Action |
|-----|--------|
| [[↑]] [[↓]] | move between rows |
| [[a]] | **add** a plugin inline (spec formats below) |
| [[Enter]] | on a server: **enable / disable** · on a tool sub-row: **toggle that tool** · on an untrusted project server: **approve** it (*trust & enable*) |
| [[→]] / [[←]] | show / hide a server's tools. Servers start collapsed, so a 75-tool server is one row. [[←]] on a tool goes back to its server. |
| [[p]] | on a tool sub-row: **pin** it (always loaded) or unpin it. See [how tools load](#how-tools-load). |
| [[d]] | **remove** the server — press once to arm (row turns ⚠ *press d again to remove*), again to confirm |
| [[esc]] | close |

Every change is saved to the **file the server came from**. Toggling a project server edits the project `mcp.json`. Toggling a user server edits yours. A fresh inline add goes to your user config unless you pass `--project`.

**Adding inline** — press [[a]], type a `name`, then one recipe:

```text
weather --uvx mcp-weather           # PyPI package, via uv
today --python scripts/today.py     # a local Python script
playwright --npx @playwright/mcp    # an npm package (npx -y …)
docs --http https://mcp.acme.dev    # a remote HTTP server (or --sse)
mydb -- /usr/local/bin/db-mcp --port 5432   # any command, verbatim after --
```

Append `--project` to write the repo config. The server connects immediately.

The same actions work from a shell, for scripts, CI or SSH sessions:

```bash
agentty plugin add weather --uvx mcp-weather
agentty plugin add docs --http https://mcp.acme.dev
agentty plugin list [--project]          # ✓ trusted · — pending approval
agentty plugin approve <name> --project  # per-server trust
agentty plugin remove <name>
```

The inline add covers the common shapes. For a server that needs **environment variables, HTTP headers, per-tool filters, or custom timeouts**, edit the file directly, as described next.

## Configure by editing `mcp.json`

Everything the TUI writes is plain JSON you can edit by hand. agentty picks up changes on the next connect: start a new session, or toggle the server in the picker. When agentty writes the file, it keeps everything else exactly as it was: keys it doesn't use, other servers, and your choice of `servers` or `mcpServers`.

The top-level key is `mcpServers` (the `servers` spelling is also read). Each entry is one server, keyed by the name you'll see in tool names (`mcp__<name>__<tool>`).

### A stdio server (a local command)

```json
{
  "mcpServers": {
    "playwright": {
      "type": "stdio",
      "command": "npx",
      "args": ["-y", "@playwright/mcp"],
      "env": { "PLAYWRIGHT_BROWSERS_PATH": "/opt/ms-playwright" },
      "disabled": false,
      "timeoutMs": 30000,
      "connectTimeoutMs": 10000,
      "maxOutputChars": 30000,
      "trustAnnotations": false,
      "tools": {
        "include": ["browser_click", "browser_snapshot", "browser_navigate"],
        "exclude": ["browser_install"],
        "pin": ["browser_snapshot"]
      }
    }
  }
}
```

### An HTTP / SSE server (a remote url, no command)

```json
{
  "mcpServers": {
    "acme": {
      "type": "http",
      "url": "https://mcp.acme.dev/mcp",
      "headers": { "X-Tenant": "acme-prod" },
      "tools": { "exclude": ["delete_everything"] }
    }
  }
}
```

Use `"type": "sse"` for a Server-Sent-Events endpoint. HTTP/SSE servers spawn no local command, so they're never trust-gated. If the server needs OAuth rather than a static header, omit `headers` and authorize once with `agentty mcp-login acme` (see [Authorizing an OAuth-gated server](/docs/mcp#authorizing-an-oauth-gated-server)).

### Every field

| Field | Applies to | Effect |
|-------|-----------|--------|
| `type` | all | `stdio` (default when `command` is set), `http`, or `sse`. |
| `command` | stdio | the executable to spawn. |
| `args` | stdio | argument vector passed to `command`. |
| `env` | stdio | extra environment variables for the spawned process (merged over the inherited env) — the place for an API key or path a local server needs. |
| `url` | http/sse | the server endpoint. |
| `headers` | http/sse | extra HTTP headers on every request — e.g. a static bearer token or tenant id. |
| `disabled` | all | `true` keeps a server configured but off, without deleting it (what the picker's disable toggle writes). |
| `trustAnnotations` | all | default `false` — a server's read-only tool *hints* can't silently weaken your [permission checks](/docs/sandboxing); set `true` only for a server you trust to self-declare effects. |
| `tools.include` | all | allow-list: advertise only these tools (omit to advertise all). |
| `tools.exclude` | all | deny-list: hide these tools (applied after `include`). The picker's per-tool toggle edits this set. |
| `tools.pin` | all | always load these tools, never on demand (see [how tools load](#how-tools-load)). [[p]] in the picker edits this set. |
| `timeoutMs` | all | per-call deadline (default 30 s; override globally with `AGENTTY_MCP_TIMEOUT_MS`). |
| `connectTimeoutMs` | all | handshake / connect deadline. |
| `maxOutputChars` | all | cap a tool's output before it enters context. |

Unknown keys are ignored and kept, so a config shared with Claude Desktop, Cursor or VS Code works as it is. They all read the same `mcpServers` shape. Tools get namespaced names (`mcp__playwright__browser_click`) that can't clash with native tools, and `tools/list_changed` updates apply without a restart.

Already have servers set up in another tool? Copy them with `agentty mcp import`. See [Importing from other tools](/docs/mcp#importing-from-other-tools).

## Trust

A **project** `mcp.json` can come inside a repo you cloned, and a stdio server is a command agentty would run. So project stdio servers don't connect until you approve them. Approval is per server and tied to a hash of its config. If the command changes later, you're asked again.

- In the picker: [[Enter]] on the *trust & enable* row.
- From a shell: `agentty plugin approve <name> --project`.

Your own `~/.agentty` servers and remote HTTP/SSE servers never need approval. Why: [Plugin Trust](/docs/plugin-trust).

`AGENTTY_MCP_ALLOW_PROJECT=1` trusts every server in a project config at once. That's fine for a repo you fully control, not for a fresh clone.

## How tools load

Every enabled plugin tool is always **callable**. What changes is whether its schema is sent with every request (**loaded**) or found by the model when it needs it (**on demand**).

- **Always loaded:** agentty's own tools, pinned tools (`tools.pin`), and any tool the conversation has already used or found.
- **Loaded if it fits:** the other plugin tools, while their schemas take up to 10% of the model's context and the provider's limit allows (OpenAI-style providers take at most 128 tools per request).
- **On demand:** the rest. The model gets a `tool_search` tool, finds what it needs by describing the task (any language, typos are fine, or `select:<name>`), and a found tool stays loaded for the rest of the conversation.

On Anthropic, on-demand tools use the API's own deferred loading, so finding one doesn't break the prompt cache. Elsewhere a found tool is added to the end of the tool list, which keeps the cached part of the request the same.

### The tool budget

Choose how this works in **Settings → General → MCP tool loading**:

| Mode | What loads |
|---|---|
| `auto` (default) | everything that fits, the rest on demand |
| `all` | every plugin tool, up to the provider's limit |
| `on demand` | only pinned and used tools; the model searches for the rest |

The Plugins picker (also `/tools`) shows how many tools are loaded and on demand and what they cost, and each server row says how many of its tools are on demand. `AGENTTY_MCP_TOOL_BUDGET` caps how many tools load per request (`0` = no cap); it never hides a tool.

## Safety

- Plugin tools follow the same [permission profile](/docs/sandboxing) as native tools. A plugin tool that writes still asks for approval on `Ask`/`Minimal`.
- When agentty runs as an MCP server itself (`agentty mcp-serve`), it **never passes on** your plugins. This avoids leaking credentials and proxy loops.
- Different servers run at the same time. A crashed server reconnects on its next call, so the session keeps going.

## Related

- [Plugin Trust](/docs/plugin-trust) — why project servers need approval, and how content-hash trust works.
- [Build a Plugin](/docs/build-a-plugin) — write your own MCP server (Python & C++ walkthroughs + the protocol).
- [MCP Server](/docs/mcp) — the full protocol: serving agentty's tools, resources, OAuth, ACP pass-through.
- [Retrieval](/docs/retrieval) — fold a plugin's MCP **resources** into `search_docs` with `AGENTTY_RAG_MCP=1`.
- [Sandboxing & permissions](/docs/sandboxing) — how tool calls are gated.
