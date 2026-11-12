---
title: Permission Profiles
description: Ask, Write, and Minimal — how agentty gates writes, shell, and network.
nav_section: User Manual
nav_order: 30
slug: profiles
---

A profile decides which tools run on their own and which ask you first. New installs start in **Write**. Press [[S-Tab]] to cycle Write → Ask → Minimal → Write. Your choice is saved across sessions.

| Profile | Pure reads | Writes / edits | Shell / build | Network |
|---|---|---|---|---|
| **Write** (default) | auto | auto | auto | auto |
| **Ask** | auto | prompt | prompt | prompt |
| **Minimal** | prompt | prompt | prompt | prompt |

The [ACP bridge](/docs/acp) starts in **Ask** instead.

Switching profiles clears your "always allow" grants, both for this session and saved ones. So moving to a stricter profile really brings the prompts back.

## Write (autonomous, default)

Every tool runs without a prompt. This is still contained: each shell call runs in an OS sandbox, and the file tools refuse paths outside your workspace. An autonomous run can't leave your project directory or read your secrets.

## Ask

Read-only tools run on their own. Writes, shell calls, and network calls [prompt before running](clip:permission-prompt). Use it when you want to check each change, for example in an unfamiliar repo.

## Minimal

**Every** tool prompts first, including reads, search, and definition lookup. Use it when you want to approve each step. In ACP mode, this is the profile that makes Zed prompt on reads too.

:::tip
The permission policy is a compile-time `constexpr` table checked by `static_assert`s. Changing a cell breaks the build, so the rules can't drift silently.
:::
