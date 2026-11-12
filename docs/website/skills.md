---
title: Agent Skills
description: Teach agentty your codebase once with a SKILL.md. It's live the next turn. Compatible with Claude Code's skills format.
nav_section: Advanced
nav_order: 40
slug: skills
---

A skill is a folder with a `SKILL.md` that teaches agentty your conventions, DSLs and team knowledge. To add one, drop a folder into `.agentty/skills/`. It's live the next turn, with no rebuild and no restart.

This page covers writing and placing skills. If a skill runs programs or reaches the network, install it with `agentty skill add` instead. See [Installing skills](/docs/skill-install).

## Writing a skill

1. Make a folder under `.agentty/skills/` (this repo) or `~/.agentty/skills/` (every project). The folder name is the skill's name.
2. Put a `SKILL.md` in it: Markdown with a small YAML frontmatter block.

```text
---
name: house-style
description: This project's commit + code conventions. Load before committing or refactoring.
---

# House style

- Commit messages are single-line, imperative, no AI attribution.
- Prefer `edit` over rewriting whole files.
- Run `cmake --build build -j` to build; tests live in tests/.
```

The model sees only each skill's name and `description` in its catalog, and loads the full body when a task needs it. So write the `description` to make it obvious *when* to load the skill. A large library costs very little per turn.

Skills are also **searchable**. By default they're indexed into agentty's [retrieval engine](/docs/retrieval), so `search_docs` and the automatic pre-turn retrieval can find a relevant skill before the model thinks to load it. Turn this off with `AGENTTY_RAG_SKILLS=0`.

## Where skills live

agentty looks for `<name>/SKILL.md` in these folders. If two skills have the same name, the one higher in the table wins (project beats personal; `.agentty` beats the other formats):

| Location | Scope |
|---|---|
| `<project>/.agentty/skills/` | This repo (native) |
| `<project>/.agents/skills/` | This repo (shared agents format) |
| `<project>/.claude/skills/` | This repo (Claude Code compat) |
| `~/.agentty/skills/` | Every project (native, personal) |
| `~/.agents/skills/` | Every project (shared agents format) |
| `~/.claude/skills/` | Every project (Claude Code compat) |

The catalog lists up to **64** skills. Set `AGENTTY_MAX_SKILLS` to change that (allowed range 8 to 4096). Lower it for small-context models, raise it for a big library.

:::note
Skills use the same format as Claude Code's `.claude/skills/`, so an existing skill library works in agentty unchanged.
:::

## Nesting skills (group folders)

You can put a skill up to **four levels** below one of those folders. The folders above it are group folders. They organize your library and the model never sees them. The skill's name is its path, lowercased, with every character outside `a-z0-9` turned into `-`:

```text
.agentty/skills/
  pdf-extract/SKILL.md          → name: pdf-extract
  embedded/
    startup/SKILL.md            → name: embedded-startup
  perf/
    alloc/
      pools/SKILL.md            → name: perf-alloc-pools
  My Group/
    Sub_Dir/SKILL.md            → name: my-group-sub-dir
```

Naming rules:

- The name is what you type as `/name`, so spaces, underscores and capitals are folded, and repeated separators collapse to one.
- Names longer than 64 characters are cut to 64.
- A group folder with no `a-z0-9` characters in its name is skipped.
- Flat, single-level skills keep their names.
- If two skills in the same folder end up with the same name, the **shallower** one wins. So a flat `a-b/` keeps its name even if an `a/b/` appears next to it. Project skills still beat user skills.
- Hidden folders (starting with `.`, like `.git` or `.obsidian`) are never searched.
- Keep the frontmatter `name:` equal to the path name. `agentty skills` warns if they differ.

The depth and name limits stop a stray large tree (a checkout, a `node_modules`) from turning skill discovery into a huge filesystem walk.

## Bundled resources

A skill folder can include supporting files next to `SKILL.md`: scripts, reference docs, templates. agentty lists them (a shallow walk) and lets the model read the skill folder, even when it's outside the workspace. That access is read-only. It never lets the model write there.

## Linting your skills

`agentty skills` lists every skill it finds, with lint warnings, and exits with code 1 if there are any. Add it to CI to catch a broken or misnamed skill before it ships.

```bash
agentty skills   # list + validate; exit 1 on warnings
```
