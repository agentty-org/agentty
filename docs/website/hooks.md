---
title: Hooks
description: Run your own shell commands around every tool call — block a dangerous command before it runs, or log and react after. Consent-gated by design.
nav_section: Tools
nav_order: 45
slug: hooks
---

Hooks are shell commands that agentty runs around tool calls. A `pre_tool` hook runs before a tool and can block it. A `post_tool` hook runs after a tool returns. Use them for rules the model can't override, such as blocking `bash` commands that touch `/etc`, logging every write, or running a formatter after each edit.

A hooks file never runs until you approve it in a shell. Any change to the file needs approval again.

## Quick start

1. Create `~/.agentty/hooks.json`:

   ```json
   {
     "pre_tool": [
       { "match": "bash", "run": "~/.agentty/guard.sh" }
     ],
     "post_tool": [
       { "run": "logger -t agentty" }
     ]
   }
   ```

2. Create `~/.agentty/guard.sh` and make it executable (`chmod +x`):

   ```sh
   #!/bin/sh
   # pre_tool hook: block commands that touch protected paths
   payload=$(cat "$AGENTTY_HOOK_PAYLOAD_FILE")
   case "$payload" in
     *'/etc/'*|*'~/.ssh'*)
       echo "refusing: touches a protected path"
       exit 1 ;;   # non-zero → blocked, message shown to the model
   esac
   exit 0
   ```

3. Approve it:

   ```bash
   agentty hooks approve
   ```

Now every `bash` call is checked by `guard.sh`, and every tool result is sent to the system log.

## Events

There are only two events. Both apply to individual tool calls. There are no session, turn, or prompt hooks.

| Event | Runs | Can it block? |
|-------|------|---------------|
| `pre_tool` | just before a tool runs | **yes**, exit non-zero to block |
| `post_tool` | just after a tool returns | no, its exit code is ignored |

## Where the file lives

agentty uses the first file that exists:

1. `.agentty/hooks.json` (this project)
2. `~/.agentty/hooks.json` (all projects)

The file is read again on each tool call, so edits apply right away (once approved). Files over 64 KiB are ignored (`src/tool/hooks.cpp:57`).

## Entry fields

| Field | Required | Meaning |
|-------|----------|---------|
| `run` | **yes** | the shell command to run |
| `match` | no | a POSIX extended regex matched against the **tool name**; omit it to match every tool |

`match` checks the tool name only, not arguments or paths. If the regex is malformed, the hook never fires.

## What a hook receives

A hook runs in the **same OS sandbox as the `bash` tool**, so it is confined to the workspace. agentty passes context in environment variables:

| Variable | Value |
|----------|-------|
| `AGENTTY_HOOK_EVENT` | `pre_tool` or `post_tool` |
| `AGENTTY_HOOK_TOOL` | the tool name (`bash`, `edit`, …) |
| `AGENTTY_HOOK_PAYLOAD_FILE` | path to a mode-`0600` temp file holding the JSON payload |

The payload file looks like this:

```json
// pre_tool
{ "event": "pre_tool", "tool": "bash", "args": "<tool args as JSON string>" }

// post_tool
{ "event": "post_tool", "tool": "bash", "args": "…", "result": "<tool output>" }
```

The payload is a file, not stdin or an env var, so a large result can't hit the environment size limit. `result` is cut to 4 MiB.

## What a hook can do

- **`pre_tool` blocks or allows.** Exit non-zero to block. The hook's output becomes the error the model sees. If it printed nothing, the reason is ``blocked by pre_tool hook (`<cmd>` exited N)``. Exit 0 to allow. The first hook that blocks wins.
- **`post_tool` only observes.** It can't change the result or block anything. Use it for logs, notifications, or side effects.

A hook can't add text to the conversation or change a tool's output. Blocking with a reason is the only control it has.

## Limits

| Setting | Value |
|---------|-------|
| Timeout per hook command | 30 seconds |
| Max `result` in payload | 4 MiB |
| Max hooks file size | 64 KiB |
| Disable all hooks | set `AGENTTY_NO_HOOKS=1` |

## Approving a hooks file

A hooks file is inert until you approve it. Approval stores the file's SHA-256 in `~/.agentty/hooks_approved.json`. Change one byte and it needs approval again. Unapproved hooks never run.

```bash
agentty hooks           # or: agentty hooks list — show hooks + approval state
agentty hooks approve   # prints the file, asks y/N, stores the hash
```

In the app, [[Ctrl+K]] → **Hooks** shows the file and whether it is approved. Pressing Enter there does **not** approve it. It points you to `agentty hooks approve`. Approval happens in a terminal on purpose, so you can't dismiss the y/N prompt by reflex.

:::warn
A hook runs any code it names, with your permissions, inside the workspace sandbox. Only approve a `hooks.json` whose `run` commands you have read. The approval step means a synced dotfile or a cloned repo can't run code before you see it.
:::

## Related

- [Sandboxing & permissions](/docs/sandboxing): the OS sandbox hooks run in, and the permission profiles for tools.
- [Tools](/docs/tools): the tool names you can match on.
