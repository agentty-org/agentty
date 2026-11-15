---
title: Hooks
description: Run your own commands around every tool call. Block a command, rewrite it, or replace its output before the model reads it. Works with hooks written for Claude Code, like RTK.
nav_section: Tools
nav_order: 45
slug: hooks
---

Hooks are shell commands that agentty runs around tool calls. A hook can:

- **block** a call, with a reason the model reads
- **rewrite** a call before it runs, for example `git status` to `rtk git status`
- **allow** a call without asking you, or **ask** you even when your profile wouldn't
- **replace** a tool's output before the model reads it, to save tokens
- **add a note** the model reads next to the result

agentty speaks Claude Code's hook protocol, so hooks written for Claude Code work as they are. agentty's older `pre_tool` / `post_tool` format still works too.

A hooks file never runs until you approve it in a shell. Any change to the file needs approval again.

## Quick start: RTK

[RTK](https://github.com/rtk-ai/rtk) shrinks command output before the model reads it. Its Claude Code hook works in agentty unchanged.

1. Install RTK, so `rtk` is on your `PATH`.
2. Create `.agentty/hooks.json` in your project (or `~/.agentty/hooks.json` for all projects):

   ```json
   {
     "hooks": {
       "PreToolUse": [
         { "matcher": "Bash", "hooks": [ { "type": "command", "command": "rtk hook claude" } ] }
       ],
       "PostToolUse": [
         { "matcher": "Bash", "hooks": [ { "type": "command", "command": "rtk hook claude" } ] }
       ]
     }
   }
   ```

3. Approve it:

   ```bash
   agentty hooks approve
   ```

Now `git status` runs as `rtk git status`, and the shell card shows the command that really ran. The `PostToolUse` entry is only needed for RTK's output filters.

## Quick start: a guard

Block shell commands that touch protected paths. `.agentty/guard.sh`:

```sh
#!/bin/sh
cmd=$(jq -r '.tool_input.command')
case "$cmd" in
  *'/etc/'*|*'.ssh'*)
    echo "refusing: touches a protected path" >&2
    exit 2 ;;   # exit 2 blocks; stderr is what the model reads
esac
exit 0
```

```json
{
  "hooks": {
    "PreToolUse": [
      { "matcher": "Bash", "hooks": [ { "type": "command", "command": "sh .agentty/guard.sh" } ] }
    ]
  }
}
```

## Events

| Event | Runs | Can block | Can change |
|-------|------|-----------|------------|
| `PreToolUse` | before the permission check | yes | the call's arguments, and whether you're asked |
| `PostToolUse` | after a tool succeeds, before the model reads the result | no | the result |
| `PostToolUseFailure` | after a tool fails | no | adds a note |

`PreToolUse` runs before the permission prompt, so a rewritten command is what the prompt shows.

## The file

agentty uses the first file that exists:

1. `.agentty/hooks.json` (this project)
2. `~/.agentty/hooks.json` (all projects)

Files over 64 KiB are ignored.

Each entry under an event has a `matcher` and a list of `hooks`:

| Field | Meaning |
|-------|---------|
| `matcher` | which tools it applies to. Empty or `*` is every tool. Letters, digits, `_ - , \|` and spaces are exact names (`Edit\|Write`). Anything else is a regex (`^mcp__`). |
| `hooks[].type` | `command`. Other types are skipped. |
| `hooks[].command` | the shell command to run |
| `hooks[].timeout` | seconds before it's stopped, 1 to 600. Default 30. |

A matcher can use either Claude Code's tool names or agentty's:

| Claude Code | agentty |
|-------------|---------|
| `Bash` | `shell` |
| `Read`, `Write`, `Edit` | `read`, `write`, `edit` |
| `Grep`, `Glob`, `LS` | `grep`, `glob`, `list_dir` |
| `WebFetch`, `WebSearch` | `web_fetch`, `web_search` |
| `TodoWrite`, `Task` | `todo`, `task` |

Other tools, MCP tools included, keep their agentty names.

## What a hook receives

The event as JSON on **stdin**, in Claude Code's shape:

```json
{
  "hook_event_name": "PreToolUse",
  "session_id": "…",
  "cwd": "/path/to/project",
  "tool_name": "Bash",
  "tool_input": { "command": "git status", "description": "…" },
  "tool_use_id": "…"
}
```

`PostToolUse` adds `tool_response` (for `Bash`: `stdout`, `stderr`, `interrupted`) and `duration_ms`. `PostToolUseFailure` adds `error`.

Arguments are translated to Claude Code's names: `path` becomes `file_path`, a single edit becomes `old_string` / `new_string`, and `Bash`'s `timeout` is in milliseconds. A rewrite is translated back.

Also set: `AGENTTY_HOOK_EVENT`, `AGENTTY_HOOK_TOOL`, `AGENTTY_PROJECT_DIR` and `CLAUDE_PROJECT_DIR`, and `AGENTTY_HOOK_PAYLOAD_FILE`, a mode-`0600` file holding the same JSON.

A hook runs in the **same OS sandbox as the shell tool**, so it is confined to the workspace like any command the model runs.

## What a hook can answer

**Exit codes:**

| Exit | Meaning |
|------|---------|
| `0` | fine. stdout is read as JSON if it is a JSON object. |
| `2` | block. stderr is the reason. For `PostToolUse` the tool already ran, so stderr becomes a note for the model. |
| other | the hook is broken. The call goes ahead and the error is logged. |

**JSON on stdout** (with exit 0), as in Claude Code:

```json
{
  "hookSpecificOutput": {
    "hookEventName": "PreToolUse",
    "permissionDecision": "allow",
    "permissionDecisionReason": "why",
    "updatedInput": { "command": "rtk git status" },
    "additionalContext": "a note the model reads with the result"
  }
}
```

| Field | Event | Effect |
|-------|-------|--------|
| `permissionDecision` | PreToolUse | `deny` blocks with the reason, `ask` always prompts you (with the reason), `allow` skips the prompt |
| `updatedInput` | PreToolUse | replaces the arguments. Include every field, not just the changed ones. |
| `updatedToolOutput` | PostToolUse | replaces what the model reads. For `Bash`, `{"stdout": "…", "stderr": "…"}`. |
| `decision: "block"` + `reason` | PostToolUse | adds the reason next to the result |
| `additionalContext` | all | a note the model reads with the result |
| `continue: false` + `stopReason` | all | stop after this tool: no next turn. You see the reason. |
| `systemMessage` | all | shown to you, not the model |

When several hooks match, they run in file order. Each sees the arguments as the one before left them. If they disagree, `deny` wins over `ask`, and `ask` wins over `allow`.

In `agentty run` and in subagents nobody can answer a prompt, so `ask` blocks the call there.

## agentty's older format

Still supported:

```json
{
  "pre_tool":  [ { "match": "shell", "run": "~/.agentty/guard.sh" } ],
  "post_tool": [ { "run": "logger -t agentty" } ]
}
```

`match` is a POSIX extended regex on agentty's tool name. A `pre_tool` hook blocks on **any** non-zero exit, and its stdout is the reason. Both formats can be in one file.

## Limits

| Setting | Value |
|---------|-------|
| Timeout per hook | 30 seconds by default, `timeout` up to 600 |
| Payload size | results over 4 MiB are cut |
| Text a hook can add | 10,000 characters per field |
| Hooks file size | 64 KiB |
| Disable all hooks | `AGENTTY_NO_HOOKS=1` |

## Approving a hooks file

A hooks file does nothing until you approve it. Approval stores the file's SHA-256 in `~/.agentty/hooks_approved.json`. Change one byte and it needs approval again.

```bash
agentty hooks           # or: agentty hooks list — show hooks and approval state
agentty hooks approve   # prints the file, asks y/N, stores the hash
```

In the app, [[Ctrl+K]] → **Hooks** shows the file and whether it is approved. Pressing Enter there does **not** approve it; it points you to `agentty hooks approve`. Approval happens in a terminal on purpose, so you can't dismiss the y/N prompt by reflex.

:::warn
A hook runs any code it names, with your permissions, inside the workspace sandbox. Only approve a `hooks.json` whose commands you have read. The approval step means a synced dotfile or a cloned repo can't run code before you see it.
:::

## Related

- [Sandboxing & permissions](/docs/sandboxing): the sandbox hooks run in, and the permission profiles.
- [Tools](/docs/tools): the tool names you can match on.
