---
title: MCP Server
description: Serve agentty's tools to other MCP clients, and connect agentty to other MCP servers.
nav_section: Advanced
nav_order: 30
slug: mcp
---

This page is the reference for how agentty uses the [Model Context Protocol](https://modelcontextprotocol.io). agentty works both ways: it can **use** tools from other MCP servers, and it can **serve** its own tools to other MCP clients.

:::note
Just want to add a tool, like a browser driver or a database client? Go to **[Plugins](/docs/plugins)**. It covers `mcp.json` and `agentty plugin add` step by step. To write your own server, see **[Build a Plugin](/docs/build-a-plugin)**.
:::

## Using other MCP servers

agentty connects to the MCP servers listed in an `mcp.json`. Each one is a [plugin](/docs/plugins). The smallest config looks like this:

```json
{
  "mcpServers": {
    "playwright": {
      "command": "npx",
      "args": ["-y", "@playwright/mcp"]
    }
  }
}
```

You can set a policy per server:

```json
{
  "mcpServers": {
    "playwright": {
      "command": "npx",
      "args": ["-y", "@playwright/mcp"],
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

| Key | What it does |
|---|---|
| `disabled: true` | Keeps the server off without deleting it. |
| `trustAnnotations` | Default `false`. When `false`, a server's "read-only" hints can't weaken permission checks. Turn it on only for a server you trust. |
| `tools.include` / `tools.exclude` | Filter which of the server's tools agentty offers. |
| `tools.pin` | Always send these tools with every request. |

How agentty handles these servers:

- **Names.** Server tools get namespaced names like `mcp__playwright__browser_click`, so they can't clash with or pose as native tools.
- **Live updates.** When a server sends `tools/list_changed`, agentty updates its tool list right away, including removed tools and changed schemas.
- **Tool loading.** Every enabled tool can be called. Tools that fit the context budget are sent with each request. The model finds the rest with `tool_search`, and a found tool stays loaded. On Anthropic these use deferred loading, so the prompt cache still works. See [how tools load](/docs/plugins#how-tools-load).
- **Resources and prompts.** If a server offers them, the model gets `mcp__resources__read` and `mcp__prompts__get`. The old names `mcp_read_resource` / `mcp_get_prompt` still work.
- **Workspace.** Stdio and HTTP servers get the workspace through MCP roots.
- **Calls.** Progress shows in the live tool card. [[Esc]] cancels the call. A crashed server reconnects on its next call.
- **Concurrency.** Different servers run at the same time. Calls to one server run one after another.

:::note
If you have no `mcp.json`, MCP costs nothing at startup. agentty checks for the file and moves on.
:::

## Importing from other tools

Already set up MCP servers in another tool? Copy them over instead of typing them again:

```bash
agentty mcp import                  # list what each tool on this machine has
agentty mcp import --from codex     # copy them into ~/.agentty/mcp.json
agentty mcp import --from claude --dry-run
```

| `--from` | reads |
|---|---|
| `claude` | `~/.claude.json` (this project's servers) and `./.mcp.json` |
| `codex` | `~/.codex/config.toml` (or `$CODEX_HOME`) and `./.codex/config.toml` |
| `cursor` | `~/.cursor/mcp.json` and `./.cursor/mcp.json` |
| `vscode` | `./.vscode/mcp.json` |
| `junie` | `~/.junie/mcp/mcp.json` and `./.junie/mcp/mcp.json` |

Things to know:

- It's a one-time copy, not a link. agentty never reads another tool's MCP config by itself. Those servers start programs, and an edit in another tool shouldn't change what agentty runs.
- If agentty already has a server with the same name, it's left alone unless you pass `--force`.
- `agentty config mcp` lists the other tools' configs it found, with the import command for each.

JSON configs are copied as they are. Codex's TOML is translated field by field: `command`, `args`, `env`, `cwd`, `url`, `http_headers`, `tool_timeout_sec` / `startup_timeout_sec`, `disabled_tools` and `enabled = false` all carry over. Two things don't, and the import prints a note under the server they belong to:

- **Secrets in environment variables** (`bearer_token_env_var`, `env_http_headers`). agentty won't write a token into a file for you. Add the header by hand, or use `agentty mcp-login` if the server supports OAuth.
- **`enabled_tools`**, because agentty only has `tools.exclude`.

Moving from Codex or Claude Code for good? [Switching](/docs/switching) covers bringing over your conversations and instructions too.

## Authorizing an OAuth-gated server

Some hosted MCP servers need OAuth (MCP spec **2026-07-28** authorization). If you haven't logged in, a call to one returns a clear error instead of hanging. Log in once:

```bash
agentty mcp-login <server>
```

agentty then:

1. Finds the authorization server from the endpoint's protected-resource metadata (RFC 9728).
2. Registers itself as a client (`application_type=native`, so a loopback redirect is allowed).
3. Opens your browser so you can approve access.
4. Catches the redirect on a local loopback port.
5. Checks the response, including the **RFC 9207 `iss` check**, before it exchanges the code. This blocks attacks that mix up authorization servers.

This is OAuth 2.1 with PKCE.

Manage stored tokens:

```bash
agentty mcp-status          # list servers and which are authorized
agentty mcp-logout <server> # forget a stored token
```

About the token:

- It's tied to the issuer, so it's never sent to a different authorization server.
- It's stored encrypted in `~/.agentty/mcp_tokens/<server>.json` with `chmod 600`.
- It's sent with every request to that server and refreshed when it expires.

If you already have a bearer token, you can skip OAuth. A static `Authorization` header in `mcp.json` always wins:

```json
{ "mcpServers": { "acme": { "url": "https://mcp.acme.dev/mcp",
    "headers": { "Authorization": "Bearer sk-..." } } } }
```

### When the server doesn't support Dynamic Client Registration

Some authorization servers have no registration endpoint. Give agentty a `client_id` instead. An `https://` value is used as a **CIMD** (Client ID Metadata Document) URL, where the URL itself is the client identity; this is the preferred path in the 2026-07-28 spec. Any other value is treated as a pre-registered public client.

You can set it in three places. In order of precedence:

1. On the command line:
   ```bash
   agentty mcp-login acme --client-id https://agentty.example/client.json   # CIMD URL
   agentty mcp-login acme --client-id my-registered-client-id               # pre-registered id
   ```
2. Per server in `mcp.json`, as `"client_id"`:
   ```json
   { "mcpServers": { "acme": { "url": "https://mcp.acme.dev/mcp",
       "client_id": "https://agentty.example/client.json" } } }
   ```
3. For all servers, with `AGENTTY_MCP_CLIENT_ID`.

If agentty can't find the metadata URL from the 401 response, pass it with `--metadata <url>`.

## Serving agentty's tools (mcp-serve)

`agentty mcp-serve` runs with no terminal UI and offers agentty's native tools over MCP on stdio. An MCP client (Claude Desktop, an IDE, another agent) sends `tools/list` and `tools/call` on stdin and reads stdout. Logs go to stderr.

```bash
agentty mcp-serve
```

To use it from a client that reads a JSON config (Claude Desktop shown here):

```json
{
  "mcpServers": {
    "agentty": {
      "command": "agentty",
      "args": ["mcp-serve"]
    }
  }
}
```

What it serves:

- The same native tools the TUI uses: file `read`/`write`/`edit`/`move`/`remove`, shell `bash`, long-running `process_*` sessions, focused `test`, code search (`grep`/`glob`/`find_definition`/`search_structural`/`repo_map`), web fetch/search, diagnostics, and the `git_*` tools including `git_show` and `git_blame`.
- File tools stay inside the workspace, and shell/process calls run in the OS sandbox, the same as in the TUI.
- **Native tools only.** It never passes on your external MCP servers. That avoids leaking credentials and avoids MCP proxy loops.
- A native tool you switched off for agentty is not served either. Today that means `web_search` when `Ctrl+K → Settings → Web Search` (or `AGENTTY_WEB_SEARCH`) is `off`. The setting is read at startup, so restart `mcp-serve` after changing it.

## External ACP agents

When you select an external ACP agent, agentty passes your trusted MCP servers to it in `session/new.mcpServers`. That agent makes the calls. agentty shows its ACP tool updates but doesn't run them again. Workspace-local servers are passed on only once trusted: by per-server [content-hash approval](/docs/plugin-trust) or by setting `AGENTTY_MCP_ALLOW_PROJECT=1`.

agentty does not quietly add its own `agentty mcp-serve` to a delegated agent. That would duplicate built-in tools, blur who runs what, and allow recursive `task` loops.

## Searching an MCP server's resources

An MCP server can also offer **resources** (`resources/*`), such as documents or wiki pages. agentty can add these to its [retrieval engine](/docs/retrieval), so `search_docs` searches them along with your local docs, skills and memory, in one ranked list tagged by source. It's off by default. Turn it on with `AGENTTY_RAG_MCP=1` (you need an MCP config for it to do anything).
