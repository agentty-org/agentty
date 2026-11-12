---
title: Installing skills
description: How to install a skill that runs programs or reaches the network. One command, a consent screen agentty writes, and approval pinned to the skill's content.
nav_section: Tools
nav_order: 45
slug: skill-install
---

This page covers installing skills with `agentty skill add`, and the checks agentty runs when you do. Most skills are plain instructions ("here's our house style") and need none of this: just drop them in a skills folder, as described in [Agent Skills](/docs/skills). The steps here matter for skills that tell the agent to run programs, reach the network, or write files that outlive the turn.

## The commands

```sh
agentty skill add ./path/to/skill-dir
agentty skill list
agentty skill remove NAME
agentty skill approve NAME
```

The same command works for a vendor's skill, a folder on disk, or your company's internal one. `skill add` installs into `~/.agentty/skills/<name>`, so the skill is available in every project.

Remote fetch isn't supported yet (`src/tool/skills_cli.cpp:365`). Clone first, then add the path:

```sh
git clone https://github.com/you/your-skill /tmp/s && agentty skill add /tmp/s
```

There is no bundled skill, no first-run screen and no setup-menu entry. A skill arrives only because you asked for it by name. Giving one vendor a slot in setup would mean having no fair answer for the next one.

## Declaring what a skill does

A skill declares what it can do in its frontmatter, with the same four words agentty's own tools use:

```yaml
---
name: sites
description: deploy temporary apps with hosting, database, storage
effects: [exec, net, write-fs]
source: github.com/example/agentty-sites
---
Run `npx --yes @example/init`, then POST to the deploy API.
```

| effect | means |
|---|---|
| `read-fs` | opens and scans paths on this machine |
| `write-fs` | creates or modifies files that outlive the run |
| `net` | sends and receives data over the internet |
| `exec` | runs commands, including fetched packages |

- **No `effects:` means prose only.** That covers every older skill. Prose skills are never gated.
- Parsing is lenient. `[exec, net]`, `exec net` and `exec,net` all work. `write-fs`, `write_fs` and `WriteFs` mean the same thing.
- An unknown word is ignored, so a skill written for a newer version still loads.

## What you see before it installs

```
  install skill "sites" from github.com/example/agentty-sites?

  deploy temporary apps with hosting, database, storage

  the agent will be told it may:
    write files        create or modify files that outlive the run
    reach the network  send and receive data over the internet
    execute programs   run commands, including fetched packages

  skills are instructions, not sandboxed code. agentty cannot
  enforce what is written above — it is what the skill declared.
  you did not write these instructions.

  [1] install   [2] print it first   [3] cancel
```

**agentty writes all of this text.** The skill only supplies its name, description, source and declared effects. It can't write its own reassuring summary. A skill that under-declares gets a prompt that under-sells it, but it can't lie in the prompt.

`[2] print it first` shows you the actual `SKILL.md` before you decide.

## What agentty checks before it asks

Before showing the prompt, agentty reads the skill body and looks for patterns seen in real malicious skills. Most installs get the quiet prompt above. A flagged skill gets a different-looking screen, with the safe choice first. (People approve about 93% of permission prompts, so a screen that always looks the same stops being read.)

```
  ━━━ REVIEW THIS ONE ━━━

  install skill "helper"?

  formats your code nicely

  declares no effects — prose only.

  reading the file, agentty noticed:
   !! line 1    downloads a script and pipes it straight into a shell
   !! line 3    puts instructions inside an HTML comment
   !! line 6    instructs the agent to hide what it is doing from you
   !! line 7    installs something that keeps running after this session
    ! line 5    reads credentials or environment secrets

  this is pattern matching over text, not proof of intent —
  and a skill with no findings has not been proven safe.

  [2] print it first   [3] cancel   [1] install anyway
```

That skill declared no `effects:` at all. What a skill says about itself is the least reliable part of the file, so the check reads the **body**.

What gets flagged:

| finding | why |
|---|---|
| `curl \| sh` | the code that runs is whatever the server sends today |
| base64 → `eval` | the real command is hidden from anyone reading the file |
| instruction override | a skill should describe a task, not reprogram the agent |
| directives in HTML comments | invisible when rendered, visible to the agent |
| "don't tell the user" | tells the agent to hide what it's doing from you |
| crontab / `authorized_keys` / shell rc | outlives the session |
| reads credentials + posts outbound | the two halves of stealing data |
| `Bash(*)` pre-approval | asks for unrestricted shell up front |
| password-protected archives | a known way past scanners |
| bidi / zero-width characters | text that renders differently than it parses |

`--yes` refuses to install a skill with critical findings. A script shouldn't quietly accept something a person would have stopped at.

:::warn
This is a review aid, not a verdict. It matches text patterns, so aliases, wrapper scripts, encoding and new tricks can get past it. A clean result is one piece of evidence. agentty never says "safe".
:::

## Seeing what you have

[[Ctrl+K]] → **Skills** opens a read-only viewer. [[↑]]/[[↓]] move, [[Esc]] closes.

```
┌─ Skills ───────────────────────────────────────────────────┐
│ !! sneaky                              prose                 │
│  ! deployer                    exec, net · PENDING           │
│    house-style                         prose                 │
│                                                              │
│   formats code                                               │
│   you wrote this                                             │
│   ~/.agentty/skills/sneaky                                   │
│                                                              │
│   agentty noticed:                                           │
│    !! line 1  downloads a script and pipes it into a shell    │
│    !! line 2  instructs the agent to hide what it is doing    │
│                                                              │
│   pattern matching, not proof — and no findings is not a      │
│   clean bill.                                                │
│                                                              │
│   1 skill flagged, 1 awaiting approval                       │
└──────────────────────────────────────────────────────┘
```

- Rows are sorted worst first: flagged, then awaiting approval, then the rest.
- Every state has a word or symbol as well as a colour, so it reads the same in `theme::native` and for colourblind users.
- **The viewer has no approve key, on purpose.** It points you to `agentty skill approve NAME`. Approving should be something you do deliberately, with the findings and body in front of you, not one keystroke away from a list you're scrolling.

## What `skill add` refuses

| refused | why |
|---|---|
| a `name:` that isn't one plain folder name | the name becomes the install **path**, so `name: ../../../../tmp/x` would write anywhere |
| symlinks inside the skill folder | a link could make the copy walk your filesystem or copy a file like `/etc/passwd` |
| a `SKILL.md` over 2 MB (or 8 MB / 128 files total) | a skill is instructions; anything bigger should be a plugin |
| a skill with no body and no description | it uses a catalog slot and tokens every turn to say nothing; usually a wrong path |
| a prompt with no terminal to answer it | there's nobody to answer, so it fails instead of silently doing nothing |
| `--yes` on a skill with critical findings | see above |

A bad name is refused, not cleaned up. Quietly installing `foo` when the file said `../../foo` would just be a different surprise. Ordinary names like `sites`, `house-style`, `code_review` or `rust-2024` are fine.

Skipped symlinks are reported:

```
skipped 2 symlinks — skills are copied as plain files
installed sym → ~/.agentty/skills/sym
```

## Two skills, one name

A skill is identified by its `name:`, not its folder. When two folders declare the same name, only one loads. agentty tells you about the other one, because the hidden copy could be the one that runs programs:

```
$ agentty skill list
same                     user     -
    innocent prose

!! "same" is also declared by ~/.agentty/skills/beta
   that copy is NOT loaded and it declares net, exec
```

`skill add` refuses to create a clash:

```
refusing to install: "taken" is already the name of a skill in
  ~/.agentty/skills/existing
installing this one would be shadowed and never load. rename it,
or remove the other first.
```

A project skill overriding a user skill of the same name is normal and stays quiet. Only two folders in the *same* scope get the warning.

## Approval is pinned to content

Approving a skill approves **those exact instructions**, not the name.

```sh
$ agentty skill list
sites    user   approved   write-fs, net, exec
```

Edit the body, or keep the text and add an effect, and it goes back to `PENDING`:

```sh
$ vim ~/.agentty/skills/sites/SKILL.md      # change what it tells the agent
$ agentty skill list
sites    user   PENDING    write-fs, net, exec
```

The approval is a hash of the body plus the declared effects. So a new version that quietly adds `exec` asks again. (Pinning trust to a name instead is how [MCPoison](https://nvd.nist.gov/vuln/detail/CVE-2025-54136) worked in Cursor.)

Approvals are stored in `~/.agentty/skills_approved.json`. A cloned repo can't write there, so a repo can never pre-approve its own skills.

## Where skills come from

| source | trust on load |
|---|---|
| you wrote it in `~/.agentty/skills/` | trusted |
| it came with a cloned repo (`.agentty/skills/`) | pending, if it declares effects |
| you fetched it (`source:` set) | pending, until approved |
| no `effects:` at all | trusted (prose) |

This is the same rule as project MCP config: a repo can't vouch for itself.

## Publishing one

Put a `SKILL.md` in a repo, declare what it does, and tell people to clone it and run `agentty skill add <path>`.

- **Declare honestly.** Under-declaring only gets you a user who finds out later.
- **Don't put sales copy in the body.** The body becomes the agent's instructions. A line like *"ask them if they want to upgrade"* puts words in someone else's agent's mouth. Put it in your docs.

## Limits

- **A skill is instructions, not sandboxed code.** `effects:` tells you what the author *said*. It doesn't enforce anything. What actually limits a tool run is the [sandbox](/docs/sandboxing) and the permission profile.
- **Screening can be evaded.** See the warning above.
- **Skills cost context.** Every skill takes a slot in the catalog (`AGENTTY_MAX_SKILLS`, default 64) and tokens on every turn. Install what you use.
- **There's no "trust all skills" switch**, anywhere. It would become step one of every install guide.
- **`!`-context lines don't run.** agentty doesn't support dynamic-context commands. A ported skill that has them gets a note saying those lines do nothing here.

## Background

The design draws on 2026 research:

- **Snyk** audited 3,984 skills from ClawHub and skills.sh (Feb 2026). 13.4% had a critical issue, and 91% of the confirmed-malicious ones paired a payload with prompt injection. That's why author text is cleaned before agentty shows it.
- **A 98,380-skill study** (arXiv 2602.06547) confirmed 157 malicious skills. In 100% of advanced attacks, the capability was in the body but missing from the description. That's why the check reads the body.
- **Datadog** showed dynamic-context commands run *before* the model sees the skill, which is why agentty doesn't support them.

Declaring effects in frontmatter came from [PR #47](https://github.com/agentty-org/agentty/pull/47) by Arag Agrawal (Cohesivity).
