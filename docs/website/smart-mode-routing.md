---
title: "Smart Mode: Roles & Routing"
description: The three model roles, auto-fill, which model each turn gets, and how reasoning effort scales.
nav_section: Advanced
nav_order: 62
slug: smart-mode-routing
---

This page explains how Smart Mode picks a model and a reasoning effort for each piece of work. Most people come here to pin a model to a role, or to understand why a turn got the effort it did. For the overview, see [Smart Mode](/docs/smart-mode).

## Assigning models

1. Open [[Ctrl+S]].
2. The *Strategic*, *Implementation* and *Utility* rows each show the model that would run now, tagged **· pinned** (you chose it) or **· auto** (Smart Mode filled it).
3. Press [[Enter]] on a role row to open the model picker and pin a model.
4. Press [[x]] on a pinned role to reset it to **auto**.

| Role | Answers | Runs |
|------|---------|------|
| **Strategic** | *What should we build?* Architecture, API design, hard bugs, review, planning | Complex main turns, reviewer subagents |
| **Implementation** | *Build what was decided.* Write and edit code, fix compiles, write tests | Standard main turns, coder / tester / general subagents |
| **Utility** | *Gather info, do mechanical work.* Search, read, summarise, commit messages, compaction | Trivial and Simple main turns, explorer subagents, internal engine calls |

### Zero-config auto-fill

Leave a role empty and Smart Mode fills it from your provider's live model list:

- **Strategic**: your current model, at your effort.
- **Implementation**: the strongest **mid-tier** model, one effort step down.
- **Utility**: the **cheapest capable** model, with reasoning effort **off**.

On a single-model or Opus-only account there is nothing cheaper, so every role stays on your model.

:::note
All three roles must be on the **same provider** (see [constraints](/docs/smart-mode-reference#constraints)). The picker only offers models from your active provider.
:::

## Which model the main turn gets

Each turn is rated into a tier (see below), and the tier picks the role:

| Tier | Role |
|------|------|
| Trivial, Simple | Utility |
| Standard | Implementation |
| Complex | Strategic |

Two rows in [[Ctrl+S]] control this:

- **Route the main turn** (default on). Off means every main turn runs on Strategic.
- **Cheapest main-turn role** (default `utility`). Set it to `implementation` to keep simple turns off the cheapest model, or `strategic` to never route down.

If you start agentty with an explicit `--model`, that model is honoured.

## Complexity-scaled effort

Effort has to be chosen *before* the request is sent, so Smart Mode rates each turn up front. This costs no extra model call and no latency. There are four tiers:

| Tier | Looks like | Effort |
|------|-----------|--------|
| **Trivial** | "yes", "commit it", "run it" | none |
| **Simple** | a short, single-clause fix | one step **down** from your baseline |
| **Standard** | the everyday working turn | your baseline (unchanged) |
| **Complex** | "why does this deadlock?", "refactor the auth module", a long or multi-part ask | one step **up** (or **two** when the turn is *deeply* complex) |

You don't set the tier; it comes from your prompt. Ask a design or "why" question, or list a multi-step task, and the turn gets more room to think.

### Your baseline, and why it defaults to `auto`

The table above is **relative to a baseline**: the effort setting on your model, which you change with [[←]] / [[→]] in the model picker. Tiers move up or down from it, so the baseline needs room in both directions.

**`auto`** (the default) means the middle rung of whatever effort levels your current model has:

| Your model's ladder | `auto` means | trivial | simple | standard | complex |
|---|---|---|---|---|---|
| `minimal · low · medium · high` | `low` | off | minimal | low | **medium** |
| `low · medium · high · max` | `medium` | off | low | medium | **high** |
| on/off only | `high` | off | off | high | **high** |
| no reasoning control | — | off | off | off | off |

**Trivial turns never get a reasoning budget**, at any baseline.

To fix the level yourself, pick any rung and it stays there. **`off` is a real choice**: agentty then sends no reasoning parameter on any turn, including complex ones.

:::note
In older versions "never set" and "off" were the same value. If you had chosen **off** on purpose, set it again once; an untouched setting is now read as `auto`.
:::

### How the classifier works

The rating is a small **added-up score**, not a keyword lookup. Three kinds of signal add weight, and the total is cut into tiers:

- **Structure** (most of the weight, works in any language): numbered asks, clause and conjunction count, how much code is in the prompt, question shape, length.
- **Words**: weighted design, debug and architecture keywords in the major languages. They add capped evidence; one stray "design" in *"add a design token"* does not force the top tier.
- **Token shape**: the mix of prose, identifiers and paths.

Because it's a score, Smart Mode knows how far into a tier a turn sits. A turn barely past Complex gets one step up; a turn deep into Complex gets two.

Other rules:

- **Ambiguity rounds up.** Standard is the fallback. Under-thinking a hard turn costs more than a little extra budget on an easy one.
- **Context counts.** A short follow-up to a hard turn (*"now do the same for the other module"*) inherits weight from the turn it continues. A plain *"thanks"* is taken at face value. An attached payload can also lift the tier.
- **Effort stays in range.** A model with no reasoning control gets none.

To make the router more or less eager to escalate, [tune](/docs/configuration#smart-mode-tuning) `AGENTTY_SMART_COMPLEX_THRESHOLD` and `AGENTTY_SMART_DEEP_MARGIN`, or the matching advanced rows in [[Ctrl+S]] ([[Ctrl+E]]).

### The cascade correction

The up-front rating is a first guess. After each turn, agentty looks at what actually happened and adjusts an effort bias for the rest of the session:

- A Simple or Standard turn that started two or more `task` workers was really harder. The bias goes up.
- A failed `shell`, `test`, `diagnostics` or `edit` call is a sign the route was too weak.
- A Complex turn that delegated nothing and answered directly was easier. The bias goes down.
- A message that corrects the last turn ("no", "that's wrong", "actually…", "undo", "revert") pushes the bias up.

The bias moves back toward neutral each turn, so one odd turn doesn't stick. It is capped by `AGENTTY_SMART_BIAS_CLAMP` (the *Self-correction cap* row). The bias is **session-only**: it resets on a new thread and on exit, and is never saved. The routing card shows it as `session +N`.

## Where each role is used

- **Main turn**: by tier, as above.
- **Subagents** (`task`): explorer → Utility, reviewer → Strategic, coder / tester / general → Implementation.
- **Internal engine calls** (the auto-compaction summary): Utility.

All of these go through one resolver that maps `role → (model, effort)`. It never checks a model name such as `if (model == "opus")`, which is why Smart Mode works with any provider and why turning it off changes nothing: with the switch off, every role is your single active model.
