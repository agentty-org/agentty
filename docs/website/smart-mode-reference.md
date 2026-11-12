---
title: "Smart Mode: Reference"
description: Overlay rows, settings.json keys, environment variables, constraints and design of Smart Mode.
nav_section: Advanced
nav_order: 64
slug: smart-mode-reference
---

Every Smart Mode setting in one place: the [[Ctrl+S]] overlay, the `settings.json` keys, the environment variables, and the rules Smart Mode always follows. For a plain introduction, start with [Smart Mode](/docs/smart-mode).

## The overlay ([[Ctrl+S]])

Open it with [[Ctrl+S]] or [[Ctrl+K]] → *Smart Mode*.

| Key | Action |
|-----|--------|
| [[↑]] / [[↓]] | move between rows |
| [[Enter]] | toggle the switch, or pick a model for a role |
| [[x]] | reset a role to auto |
| [[Ctrl+E]] | show or hide the advanced rows |
| [[Esc]] | close |

| Row | What it controls | Default |
|-----|------------------|---------|
| **Smart Mode** | the master switch | off |
| **Strategic / Implementation / Utility** | the model for each role (pinned, or auto) | auto |
| **Route the main turn** | let a turn's complexity choose its model, not only its effort | on |
| **Cheapest main-turn role** | how far down a simple turn may go: `utility`, `implementation`, or `strategic` (never route down) | `utility` |
| **Complexity cut** *(advanced)* | score at which a turn counts as Complex; lower spends more | shipped default |
| **Deep-band margin** *(advanced)* | how far into a tier a turn must be to get an extra effort step | shipped default |
| **Self-correction cap** *(advanced)* | how far this session's corrections may move effort from baseline | shipped default |

The role rows are locked while Smart Mode is off. Turning Smart Mode on enables internal routing, orchestration and subagent routing together. They are not separate toggles because nobody gains from having them apart.

## Persisted config

The overlay saves your choices to the `smart` object in `settings.json` (`~/.agentty/settings.json`, or the platform equivalent). You never need to edit it by hand.

| Key | Meaning |
|-----|---------|
| `enabled` | master switch |
| `strategic_model` / `impl_model` / `utility_model` | pinned model id per role; absent means auto |
| `strategic_effort` / `impl_effort` / `utility_effort` | effort for a pinned role |
| `strategic_provider` / `impl_provider` / `utility_provider` | provider the pin was made under |
| `route_main_turn`, `main_turn_floor`, `complex_threshold`, `deep_margin`, `bias_clamp` | the policy rows above; written only when changed from the default |

Older files may still contain `route_internal`, `orchestrate`, `route_subagents`, `learn_routing`, `outcome_feedback`, `speculative` and `recall_plans`. These keys are ignored. No migration is needed.

## Nothing is stored per workspace

Smart Mode keeps **no on-disk state**. The only feedback it uses is the session correction (see [Roles & Routing](/docs/smart-mode-routing#the-cascade-correction)). It lives in memory, is capped by `AGENTTY_SMART_BIAS_CLAMP`, and resets when you start a new thread or exit.

Older builds wrote `.agentty/routing_memory.tsv` and `.agentty/decompositions.jsonl`. Nothing reads them now. `agentty config clean` lists and removes leftover `routing_memory.tsv` files, or you can delete them yourself.

## Environment variables

### Policy tuning

These override the advanced rows. Values are clamped to a safe range; unset uses the saved or shipped value. Full detail: [Configuration › Smart Mode tuning](/docs/configuration#smart-mode-tuning).

| Variable | Controls |
|----------|----------|
| `AGENTTY_SMART_COMPLEX_THRESHOLD` | how readily a turn counts as Complex (the main cost/quality dial) |
| `AGENTTY_SMART_DEEP_MARGIN` | how deep into a tier a turn must be for an extra effort step |
| `AGENTTY_SMART_BIAS_CLAMP` | how far session correction can move effort from baseline |
| `AGENTTY_SMART_ROUTE_MAIN` | the *Route the main turn* row |
| `AGENTTY_SMART_MAIN_FLOOR` | the *Cheapest main-turn role* row |

Individual classifier weights are not exposed. The threshold is the control to use.

### Force the switch for one run

`AGENTTY_SMART_MODE=1` forces Smart Mode on for that process; `=0` forces it off. `1`/`true`/`yes`/`on` mean on, `0`/`false`/`no`/`off` mean off. Unset means your saved setting applies.

```bash
AGENTTY_SMART_MODE=1 agentty   # predictable routing for a scripted run
AGENTTY_SMART_MODE=0 agentty   # force it off without touching your config
```

The override is never saved, so a benchmark or CI run can't change your normal preference. While it is set, the overlay's Smart Mode row is locked and shows `env: AGENTTY_SMART_MODE`. Unset the variable to toggle it again.

### Debug overrides

Each part of Smart Mode can be turned off alone, for tracking down a routing bug:

```bash
AGENTTY_SMART_NO_INTERNAL=1     # compaction/titles stay on the main model
AGENTTY_SMART_NO_ORCHESTRATE=1  # main turn stays on the selected model
AGENTTY_SMART_NO_SUBAGENTS=1    # workers use the tier auto-router
```

These exist only as env variables, on purpose: the default is on, and they are for debugging.

## Constraints

- **Off changes nothing.** With the switch off, Smart Mode adds no tokens, no latency and no routing decisions.
- **One provider.** All three roles must come from your active provider. Sign-in and prompt caching don't carry across providers, so the picker only offers models from that provider.
- **Roles pick models; model names never pick behaviour.** The resolver maps a role to `(model, effort)` and never inspects a model id, so any model is safe in any slot.
- **Effort stays within what the model supports.** Effort scaling and session correction stay inside the model's own range. A model with no reasoning control gets none.
- **No hidden learning.** Session correction is capped, decays each turn, and is gone when the session ends.

## Design rationale {#design}

Smart Mode follows the **orchestrator-workers** pattern: a strong model owns the plan and hands well-scoped subtasks to cheaper workers. Three ideas make that work here:

1. **Roles, not model names.** Keeping behaviour separate from model identity means you can pin models or switch providers without touching anything else.
2. **Complexity-scaled effort plus correction.** Most turns are simple, and flagship effort on them is waste. The classifier scales effort to the turn, and session correction raises effort when a cheap attempt falls short.
3. **One reactive loop.** The lead stays in agentty's normal agent loop and delegates with the `task` tool it already has. There is no static plan to fall out of date and no second agent framework.

Earlier versions also persisted learned routing across sessions. That was removed because it was never shown to beat the fixed policy and it could raise cost from one week to the next without you noticing.

See the [design note](https://github.com/agentty-org/agentty/blob/master/docs/design/smart-mode.md) for the full write-up.
