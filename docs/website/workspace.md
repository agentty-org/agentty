---
title: Workspace Boundary
description: Why filesystem tools refuse paths outside your project, and how to opt out.
nav_section: Tools
nav_order: 30
slug: workspace
---

agentty's filesystem tools only touch paths inside the workspace, which is the directory you launch in. Anything outside it (your home directory, system files, other projects) is refused. To widen it, pass `-w` / `--workspace DIR`.

## What's inside and outside {#the-workspace-root}

By default the workspace root is the current directory. Tools such as `read`, `write`, `edit`, `glob`, `list_dir`, `grep`, `find_definition`, the git tools, and `bash`'s `cd` check every path against it. A path that escapes is rejected before the tool runs (`src/runtime/main.cpp:1568`).

```bash
cd ~/code/my-app
agentty                          # root = ~/code/my-app
# read ../other-project/secret  → refused
```

`repo_map` adds one more rule. Its walk stops at any nested repository: a subdirectory with its own `.git`, `.hg`, `.svn`, or `.jj` (a submodule's gitlink *file* counts too). So a submodule or vendored checkout inside your root is skipped, and the map never mixes in another project's source.

## Widening the workspace {#pointing-at-another-workspace}

Point the boundary at another directory:

```bash
agentty --workspace ~/code/other-project
```

The path must be an existing directory, or agentty exits with an error.

To remove the boundary entirely, use the filesystem root:

```bash
agentty --workspace /
```

:::warn
`--workspace /` lets the agent touch any path your user can. With the [Write profile](/docs/profiles), that is a lot of trust. Use it on purpose.
:::

## Access boundary vs. active project

agentty tracks two roots. They are the same folder unless you widen the boundary.

- The **access boundary** is the gate above: the outermost path the tools may touch. `--workspace` moves it. `--workspace /` removes it.
- The **active project** is the directory you launched in. agentty never changes directory away from it.

When you widen the boundary, **relative paths still resolve against the project, not the boundary**:

```bash
cd ~/code/my-app
agentty --workspace /            # boundary = /, project = ~/code/my-app
# read src/main.rs   → ~/code/my-app/src/main.rs   (not /src/main.rs)
# repo_map           → maps ~/code/my-app           (not the whole disk)
# grep / find_definition / diagnostics / test / git → all scoped to the project
```

These are anchored on the project:

- relative-path resolution
- the default scope of `repo_map`, `grep`, `glob`, and `list_dir`
- `find_definition`
- `diagnostics` and `test` (build directory and manifest)
- the git tools and checkpoints
- the `@`-file picker and the symbol index
- project-scoped memory

The boundary is used only to *reject* paths. Absolute paths still work anywhere inside it, so with `--workspace /` you can reach other locations by naming them in full.

## Boundary vs. sandbox

The workspace boundary and the [sandbox](/docs/sandboxing) are separate layers, and both apply at once. The boundary governs agentty's own filesystem tools. The sandbox governs what spawned shell commands can reach.
