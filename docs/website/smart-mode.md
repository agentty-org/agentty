---
title: Smart Mode
description: Route each turn to a model and reasoning effort that fit how hard it is.
nav_section: User Manual
nav_order: 52
slug: smart-mode
---

Smart Mode sends each turn to a model that fits it. Hard thinking goes to your best model, routine work goes to cheaper ones, and reasoning effort scales with how hard the turn looks. This page covers what it does, how to turn it on, and what it costs.

It is **off by default**. When off, nothing about your turns changes.

## Turn it on

1. Press [[Ctrl+S]] (or [[Ctrl+K]] → *Smart Mode*).
2. Toggle **Smart Mode** on.
3. Leave the three model rows on **auto**, or press [[Enter]] on a row to pin a model.
4. Press [[Esc]] to close. The choice is saved for next time.

:::tip
The defaults are fine. With nothing pinned, Smart Mode fills the roles from your provider's live model list.
:::

## The three roles

| Role | Does | Auto-fill picks |
|------|------|-----------------|
| **Strategic** | planning, architecture, hard debugging, review | your current model |
| **Implementation** | writing and editing code, tests | the strongest mid-tier model |
| **Utility** | search, reading, summaries, titles, compaction | the cheapest capable model |

agentty never checks model names to decide behaviour. Each piece of work has a role, and the role picks the model. With Smart Mode off, every role is your single active model.

## What it does

Smart Mode is one switch. Turning it on enables all of this together:

- **Main-turn routing.** Each turn is rated Trivial, Simple, Standard or Complex. Trivial and Simple turns run on Utility, Standard on Implementation, Complex on Strategic. You can raise the floor or turn this off in the overlay's advanced rows ([[Ctrl+E]]).
- **Internal routing.** Engine calls such as the auto-compaction summary run on Utility.
- **Orchestration.** The main turn is told to keep decisions for itself and hand mechanical work to `task` subagents.
- **Subagent routing.** Each subagent's model follows its role: an explorer runs cheap, a reviewer runs strong.
- **Complexity-scaled effort.** Reasoning effort goes down for easy turns and up for hard ones.
- **Session correction.** If you correct a turn ("no, that's wrong", "undo"), later turns in this session think harder. This resets when you exit.

Smart Mode stores nothing per repository. It does not learn your codebase across sessions.

:::note
Smart Mode works within **one provider**. All three roles must come from the same provider, because sign-in and prompt caching don't carry across providers. On an account with only one model, every role stays on that model.
:::

## What it costs

Savings come from running routine work on cheaper models. Fast and flagship models in the same family often differ about 5× in price. Orchestration can use *more* tokens on a hard turn, because the lead starts workers. That only happens on turns rated complex enough, and the 🧠 routing card shows each decision.

## More detail

- **[Roles & Routing](/docs/smart-mode-routing)**: auto-fill, effort tiers, the classifier, session correction.
- **[Orchestration](/docs/smart-mode-orchestration)**: how the lead delegates, and the 🧠 routing card.
- **[Reference](/docs/smart-mode-reference)**: overlay rows, `settings.json` keys, env variables, constraints, [design](/docs/smart-mode-reference#design).
