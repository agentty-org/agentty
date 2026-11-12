---
title: "Smart Mode: Orchestration"
description: How the lead model hands mechanical work to cheaper subagents, and how to read the routing card.
nav_section: Advanced
nav_order: 63
slug: smart-mode-orchestration
---

With Smart Mode on, the main turn acts as a **lead** that keeps the decisions and hands mechanical work to cheaper **subagents**. This page explains how that delegation works and how to read the 🧠 routing card that shows each decision. For the overview, see [Smart Mode](/docs/smart-mode).

## The routing card

Each orchestrated turn shows its routing decision as a card in the transcript, just before the reply:

```
🧠 Smart Mode                                    14:32
   → claude-opus-4-5  · effort high  · complex   medium → high · session +1
   ● orchestrate   ● subagents
```

| Part | Meaning |
|------|---------|
| `→ claude-opus-4-5` | the model the turn was sent to |
| `effort high` | the reasoning effort it was given |
| `complex` | the [complexity tier](/docs/smart-mode-routing#complexity-scaled-effort) |
| `medium → high` | effort moved from your baseline to this value (shown only when it moved) |
| `correction` / `continuation` / `payload` | why the tier was raised above the prompt text's own score |
| `session +1` | the [session correction](/docs/smart-mode-routing#the-cascade-correction) currently applied |
| `● orchestrate ● subagents` | which parts are active |

The actual delegations appear below the card as ordinary `task` [tool cards](/docs/tools), so you can watch each worker run and read its report. The card is display-only: it is never sent to the model and never saved to the thread.

:::tip
Read the 🧠 card and the `task` cards together: *what* agentty decided, then *how* it ran. Each turn's header also names the model that actually served it.
:::

## How it works

This is the **orchestrator-workers** pattern. It is *not* a planner that writes a fixed list of steps. The lead stays in agentty's normal agent loop and delegates with the `task` tool it already has, deciding as it goes what to keep and what to hand off.

```
        You ── prompt ──▶  Strategic (the lead: thinks, decides, delegates)
                               │
             ┌─────────────────┼─────────────────┐
             ▼                 ▼                 ▼
        task(explorer)    task(coder)       task(tester)
         Utility model    Impl model         Impl model
        (map the code)   (make the edit)   (run the tests)
             │                 │                 │
             └──── one condensed report each ────┘
                               ▼
                     Strategic combines them → replies to you
```

## Subagent routing

Each `task` worker's model comes from its role:

| Agent type | Role | Runs on |
|-----------|------|---------|
| `explorer` | Utility | cheapest capable |
| `reviewer` | Strategic | your best model |
| `coder` | Implementation | mid model |
| `tester` | Implementation | mid model |
| `general` | Implementation | mid model |

Search and reading stay cheap, edits match the lead's quality, and reviews get your best model. With subagent routing turned off (`AGENTTY_SMART_NO_SUBAGENTS=1`), agentty uses its normal tier auto-router instead (read-only roles go to the cheapest capable model).

## The delegation directive

When orchestration is on, agentty adds a `<smart-mode>` instruction to the main turn's system prompt. It sets these rules for the lead:

- **Context is the expensive resource.** Spend a cheaper worker's context on broad mechanical work; keep the lead's for decisions and combining results.
- **Delegate only when it pays.** Don't start a worker for one quick tool call. Delegate breadth and depth.
- **Brief each worker fully.** Give the goal, the exact output format, the tools and paths in scope, and what *not* to touch. Use `file:line` references, not the lead's own reasoning.
- **Parallel or in order.** Independent work runs in parallel. Dependent work (explore, then edit what you found) runs in order.
- **Own the answer.** A worker's report is evidence, not truth. The lead checks claims that matter (a cited `file:line`, a test rerun) and never passes a report on as its conclusion.

The guidance on how many workers to use follows the turn's tier:

| Tier | Guidance |
|------|----------|
| Trivial | answer directly; no subagents |
| Simple | do it yourself, or use at most one explorer |
| Standard | delegate clearly separate mechanical sub-tasks |
| Complex | plan first, then run several workers in parallel (aim for 3+) and combine their reports |

Workers get matching rules: make a reasonable assumption on a minor gap and note it instead of stopping, and return a **short** report (one-line outcome, the details the lead needs with `file:line` evidence, any assumptions).

:::note
The directive is guidance; the lead decides. A strong Strategic model delegates readily. A weaker one may keep more work itself, which is still correct, just less parallel.
:::
