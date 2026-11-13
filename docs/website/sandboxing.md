---
title: Sandboxing
description: Every shell command runs inside a sandbox the kernel enforces. What it allows, how to loosen it, and what it doesn't cover.
nav_section: Tools
nav_order: 20
slug: sandboxing
---

Every shell command the agent runs goes into a sandbox, enforced by your operating system's kernel. Your project folder stays writable. Your keys, cloud credentials and `.env` files are hidden from the command.

You don't have to configure anything. This page explains what the sandbox does and how to change it.

## What it allows and blocks by default

The default preset is `balanced`. Under it, a command:

- **can read and write your project folder**, plus a private `/tmp`.
- **can read your toolchains and system files**: `/usr`, `/bin`, `/lib`, `~/.cargo/bin`, `~/.rustup`, `~/.nvm`, `~/.pyenv` and similar, plus your git config.
- **sees credentials as empty files**: `~/.ssh`, `~/.aws`, `.env` in your repo, and many more (full list [below](#what-the-command-can-reach)). The file opens but reads as empty. This is on purpose: a tool that gets "permission denied" often crashes, while one that reads an empty config usually carries on.
- **reaches the internet through agentty's proxy**. The proxy refuses `localhost`, your local network, and cloud metadata addresses.
- **can't write outside your project**. Those writes are refused.
- **can't write files your editor or shell will later run** (git hooks, `.vscode/tasks.json`, `.envrc`). See [the escape a sandbox can't see](#the-escape-a-sandbox-cant-see).

## Something got blocked?

At startup agentty prints one line about the sandbox:

```
agentty: sandbox: active (claybin) · turn it off entirely with --sandbox off
```

agentty prints `active` only after it has actually built a test sandbox and the kernel allowed it. If the sandbox can't run, the line says so.

Approving a command doesn't give it more access. The approval profile you pick with [[Shift+Tab]] (Write, Ask, Minimal) only decides whether agentty asks you first. Every command still runs inside the sandbox.

On macOS, opening apps, Finder or URLs (`open .`, `osascript`) needs system services the sandbox blocks. It fails with errors like `No application knows how to open URL` or `Unable to find application named 'Finder'`. Run it in your own terminal, or restart with `--sandbox off`.

To loosen the sandbox, open **Settings → Sandbox**. There you can:

- pick a looser preset, such as `permissive` (see [presets](#pick-a-posture-not-28-switches))
- add folders the command may read or write ([adding paths](#adding-paths))
- set **Allowed hosts** to limit the network to named sites ([below](#only-the-sites-you-name))

Changes save as you type but **apply on the next launch**, so restart agentty afterwards ([why](#why-changes-need-a-restart)).

To turn the sandbox off entirely:

```sh
agentty --sandbox off
```

:::tip
A program that ignores `https_proxy` gets *no* network inside the sandbox, because the proxy is the only thing it can reach. If something "can't connect" inside the sandbox but works outside, that is usually why.
:::

The pane also shows every wall, what enforces it, and anything it *can't* enforce on your machine.

:::caution
On **Windows** the pane previews through claybin's Windows compiler, so the footer and per-row walls match what commands get (filesystem `partial` via appcontainer+DACL, memory and process caps `strong` via the job object).
:::

## Pick a posture, not 28 switches

The pane has many settings. Most people only need the first one: **posture**. A posture is a preset that fills in every other setting.

| Posture | Pick it when | Files | Network | What you give up |
|---|---|---|---|---|
| `permissive` | a build breaks and you suspect the sandbox | toolchain dirs readable | full, via proxy | syscall filter and resource caps (except the process cap); masks and file walls stay |
| `balanced` | **the default**, everyday work | toolchain dirs readable | full, via proxy | the network is open, so a leaked secret could be sent out |
| `hardened` | running an agent you haven't reviewed | minimal | TCP ports 443, 80, 22, 53 only | an unusual build might need something it can't do |
| `airgapped` | reading or editing code you don't trust | minimal | none | `git push`, `npm install` and `curl` stop working |

You can still see and edit every setting afterwards. If you change one row, the pane relabels itself `custom`. Your own lists (extra paths, ports, **Allowed hosts**) never turn a preset into `custom`.

:::tip
In every mode with network, a command that can read your files can also send them somewhere. `airgapped` turns the network off, and costs nothing if you only read and edit code. If you need *some* network, use [Allowed hosts](#only-the-sites-you-name).
:::

## What the command can reach

**Read and write**

- your project folder
- a private, empty `/tmp` that vanishes when the command ends (on macOS, your per-user temp and cache folders under `/var/folders`, where `$TMPDIR` points)

**Read only**

- system libraries and binaries (`/usr`, `/bin`, `/lib`) so builds work
- your language toolchains: `~/.cargo/bin`, `~/.rustup`, `~/go/bin`, `~/.nvm`, `~/.pyenv`, `~/.rbenv`, `~/.asdf`, `~/.bun/bin`, `~/.deno/bin`, `~/.dotnet`, `~/.sdkman`, `~/.local/bin`
- your git config: `~/.gitconfig` and `~/.config/git` (git won't run without them). Your git *credentials* stay masked.
- a short allow-list of `/etc`: DNS config, CA certificates, your git identity. The rest of `/etc` isn't visible. On macOS, also the Xcode / Command Line Tools folders and the timezone database.

**Masked (reads as empty)**

- **Credentials:** `~/.ssh`, `~/.aws`, `~/.gnupg`, `~/.kube`, `~/.docker/config.json`, `~/.config/gh`, `~/.config/gcloud`, `~/.azure`, `~/.netrc`, `~/.npmrc`, `~/.pypirc`, `~/.cargo/credentials`, `~/.git-credentials`, `~/.password-store`, `~/.config/op`, `~/.config/rclone/rclone.conf`, `~/.terraform.d/credentials.tfrc.json`, `~/.m2/settings.xml`, `~/.gradle/gradle.properties`.
- **Other agents' sign-ins:** `~/.codex/auth.json`, `~/.claude/.credentials.json`, `~/.claude.json`, `~/.config/github-copilot`.
- **Keyrings and browser profiles:** `~/.local/share/keyrings`, `~/.config/google-chrome`, `~/.config/chromium`, `~/.config/BraveSoftware`, `~/.mozilla`. These are encrypted, but a key and the data it unlocks shouldn't leave together.
- **Shell and REPL history:** `~/.bash_history`, `~/.zsh_history`, fish history, and the Python, psql, MySQL and Node REPL histories. Pasted tokens end up there.
- **agentty's own state:** its credential store and its memory file.

A mask always wins. Nothing else the sandbox mounts can make a masked path readable again.

**Not there at all:** everything else in your home directory, such as other projects and documents.

**The network:** with Network set to `full` (the default), a command reaches the internet only through agentty's proxy. The proxy refuses your machine's own services, your local network and cloud metadata addresses. So a command can't reach your router, a database on `localhost`, or a cloud credentials endpoint. agentty sets `https_proxy` and friends, so `curl`, `git`, `pip`, `npm`, Python and Node work as usual.

### Only the sites you name

**Settings → Sandbox → Network → Allowed hosts** limits commands to *only* the hosts on the list.

| You write | It allows | It doesn't allow |
|---|---|---|
| `github.com` | `github.com` | `api.github.com` |
| `*.github.com` | `api.github.com`, `codeload.github.com` | `github.com` itself, `githubusercontent.com` |

A typical list for a JavaScript project that pushes to GitHub:

```
github.com
*.github.com
registry.npmjs.org
```

- **Empty list:** nothing changes. Any public site is reachable (the default). The row says *any public host*.
- **`*.example.com` means subdomains only.** Add `example.com` too if you want the site itself. The editor reminds you, and the pane warns if every entry is a wildcard.
- **Paste a URL** and it's trimmed to the host. `https://GitHub.com/me/repo` is saved as `github.com`.
- **Raw IP addresses are refused** once there's a list. `curl https://140.82.112.3` fails, because a list of names can't vouch for an address.
- **Typos are flagged, not guessed.** `*`, `*.com`, `git*hub.com`, an IP, or anything that isn't a host name is marked with the reason and dropped when you close the editor.
- **The agent is told.** Its instructions list the allowed hosts. When a command is refused, the agent gets a note naming them, so it picks a mirror you allowed.

:::note
The proxy checks the name the command *asked for*, then looks up that name itself. So a fake DNS answer, `curl --resolve`, or a forged `Host:` header won't get past it. An allowed name that points at your local network is still refused.
:::

Allowed hosts only applies while Network is `full`. Under `ports` or `none` it's kept but unused, and the row says so.

## Secrets inside your own repo

Credentials also end up inside projects: committed, copied into examples, or made by tooling. So agentty walks your project and masks these files by name:

`.env` and its variants, `.npmrc`, `.netrc`, `.git-credentials`, `.pgpass`, SSH keys (`id_rsa`, `id_ed25519`, `id_ecdsa`, `id_dsa`), `credentials.json`, `service-account.json`, and anything ending in `.pem` or `.tfvars`.

The walk goes three levels deep by default (configurable). It skips `.git`, `node_modules`, `target`, `build` and `.venv`, which are large and rarely hold credentials.

:::tip
A monorepo's `services/api/.env` is masked too, not just the one at the repo root.
:::

## Adding paths

Need a folder outside your project to be readable? In **Settings → Sandbox**, the **Also readable** row opens a list editor with path completion. Type a few characters and [[Tab]] accepts. It completes folders and files.

- clear a line (or press [[Ctrl+X]]) to remove an entry
- **Masked** hides something *inside* a folder you granted ("this whole folder, except that one file")
- **Allowed ports** switches Network to per-port mode when you add the first one
- **Allowed hosts** limits the network to named sites (see [above](#only-the-sites-you-name)); you can paste URLs

## Why changes need a restart

This is the one place in agentty where saving doesn't apply right away.

If the walls could change while commands run, they could only usefully get looser, and loosening can't be undone: tightening again doesn't un-read a key that already left. It would also make the sandbox hard to audit, with two commands in one session getting different walls.

So edits save as you type and take effect on the next launch. The pane checks this. If the next launch can't deliver what the settings describe (the policy won't compile, the engine can't start, a feature degrades), the footer says which and why.

## The escape a sandbox can't see

One attack gets past any process sandbox, and it shows up in real incidents.

The agent writes an ordinary file in your project, such as `.vscode/tasks.json`, a git hook, a `.envrc` or a virtualenv interpreter. That's a normal write inside the folder it may write. Later **you** open the project in your editor, run `git commit`, or activate the virtualenv, and that file runs outside any sandbox.

agentty refuses those writes by default and tells the model to write the content somewhere inert and ask you to wire it up. You can relax this to warn-and-allow in the pane, but `refuse` is the default.

The files it guards:

- **Editor and agent config:** `.vscode`, `.idea`, `.cursor`, `.claude`, `.agentty`, `.codex`, `.gemini`, `hooks.json`
- **git:** `.git/config`, `.git/hooks`, `.git/info`
- **Shell start-up:** `.bashrc`, `.zshrc`, `.profile`, `.bash_profile`, `.zprofile`, `.zshenv`, `.xprofile`, `.xinitrc`, `.envrc`, `.config/fish`, `.config/environment.d`, virtualenv `activate`
- **Login session:** `.config/autostart`, `.config/systemd`, and window-manager config (Hyprland, Sway, i3, Plasma)
- **Executables found on `PATH`:** `.local/bin`, `node_modules/.bin`, virtualenv interpreters
- **Build scripts:** `Makefile`, `package.json`, `build.rs`, and similar

Where it can, the sandbox also mounts the existing ones read-only, so the write fails with `EROFS`. Where it can't (a host without user namespaces), the write is still detected and reported.

## What it doesn't stop

- **Sending data over a network you allowed.** If a command can read your files and the network is on, it can upload them: to any public site, or with **Allowed hosts** set, to the sites on your list (and anywhere they forward it). Blocking the network breaks `git push` and `npm install`, which is why `airgapped` and Allowed hosts are opt-in. For one headless run, `agentty run --no-network` removes the network without changing your saved setting.
- **Tools that ignore the proxy settings.** They get *no* network, not an unfiltered one.
- **UDP in ports mode.** With Network set to `ports`, the allowed-ports list limits TCP only. UDP and ICMP aren't filtered, so DNS-based exfiltration still works there. Under `full` the ports list is ignored, and the pane says so.
- **Map, write, then make executable.** W^X refuses a mapping that is writable and executable at once. Writing a page and then making it executable still works, because JITs need it.
- **Secrets deeper than the scan depth,** or inside a skipped folder like `node_modules`.
- **Credential files with unusual names.** A secret in `config.local.yaml` doesn't look like a secret.
- **Kernel bugs.** The command shares your kernel. A kernel flaw defeats any sandbox of this kind. That is what the pane means by kernel isolation `none`.
- **A host that can't build a wall.** On Linux, a kernel without Landlock (older than 5.13) leaves no engine. On Windows, a failed AppContainer probe (policy software, token shape) leaves none either. `--sandbox on` fails loudly; `--sandbox auto` prints what you actually got.

## The model knows it's sandboxed

A model that doesn't know it's confined misreads every wall it hits. A permission error becomes "your corporate proxy is blocking this". Then it sends you after a problem that doesn't exist, or retries a command that will never be allowed.

So agentty tells it twice: in the system prompt (what's *allowed*, so it works in the right place first time), and on the output of any command that was denied. Both tell it that approval doesn't widen the sandbox, to say the sandbox is the likely cause rather than certain, and to offer you a next step: run the command yourself, add the folder in Settings → Sandbox, or restart with `--sandbox off`.

## Locking down a single run

`agentty run` takes two flags for running on code you don't trust, like a pull request in CI:

- `--read-only` gives the model only tools that read. Write, shell and network tools aren't offered, and if the model asks for one anyway the call is refused.
- `--no-network` runs every command with no network at all (an empty network namespace, so not even DNS works) and refuses network tools. It needs the claybin sandbox. With the sandbox off or unavailable, the run refuses to start.

```sh
agentty run --read-only --no-network "review this branch for anything that leaks secrets"
```

Both apply to that run only. Your saved sandbox setting is untouched.

## Modes

| Mode | Behaviour |
|---|---|
| `auto` (default) | Use a sandbox if one is available; otherwise run without and say so. |
| `on` | Require one. Refuse to start if the machine can't provide it. |
| `off` | No sandbox, including the write gate above. "Off" means off. |

```sh
agentty --sandbox on
```

If your machine blocks a feature agentty needs, `--sandbox auto` runs unsandboxed and prints why, and `--sandbox on` refuses to start. On Linux, on a host that blocks unprivileged user namespaces, you can allow them: an AppArmor profile for `/usr/bin/bwrap` containing `userns,`, or `sudo sysctl -w kernel.unprivileged_userns_clone=1`. (claybin's default path doesn't need user namespaces, which is why it's tried first.) On macOS there's nothing to enable: seatbelt is always present.

:::warn
Running with `--workspace /` makes your whole filesystem the project folder, so there's nothing left to contain. agentty reports the sandbox as degraded. Keep the workspace scoped to your project.
:::

:::warn Don't use your home directory as the workspace
The workspace is the one place commands may write. Start agentty in `~` and every dotfile becomes writable: shell start-up files, `~/.local/bin`, autostart entries, other tools' config. The masks and read-only binds still hold for the files they cover, but anything they don't name is open, and the trust-handoff check hits its size cap on a whole home directory. Settings → Sandbox warns when this is the case. Start agentty in a project folder instead.
:::

## See it work

```bash
# inside the sandbox
$ cmake --build build -j     # works: project + system libraries reachable
$ cat ~/.ssh/id_rsa          # empty: the key never reaches the command
$ cat .env                   # empty: masked even inside your own project
```

:::warn
A sandbox limits the damage. It doesn't replace reading what the agent proposes. With the network on, an approved command can still send your code somewhere.
:::

## What the walls actually are

The sandbox is several mechanisms, each closing a different way out. Linux has all of them:

- **A private view of the filesystem.** The command gets its own *mount namespace*. Your project is there, system libraries are there read-only, and `~/.ssh` is an empty file. It isn't hidden by convention; it isn't in that view.
- **A list of allowed syscalls.** A program asks the kernel to do things (open a file, start a process, send a packet) through **syscalls**. A *seccomp filter* lists which are allowed. The default profile blocks the ones used to escape a sandbox (`ptrace`, `mount`, `unshare`) and allows what a compiler needs. Sockets are limited to Unix, IPv4 and IPv6; netlink, vsock, raw and packet sockets are refused.
- **File rules the kernel checks.** *Landlock* (Linux 5.13+) lets a program permanently drop its own file access. It applies to that process and everything it starts, so a build script that starts a shell that starts Python is still confined.
- **Resource ceilings.** *cgroups* cap memory, CPU and process count. A runaway build hits a limit instead of taking your laptop down.
- **No new privileges.** The `no_new_privs` flag stops a program from gaining permissions through a setuid binary.
- **A detached terminal session.** Without it, a command could push fake keystrokes into your terminal (the `TIOCSTI` trick) and make your shell run them after agentty exits.

:::note
Each platform compiles the same policy into what it has. On Linux: namespaces, Landlock, seccomp and cgroup2 at full strength. On macOS: seatbelt plus rlimits. A seatbelt profile can't be dropped by `exec`, which matches `no_new_privs`. Resource ceilings become POSIX rlimits, which are weaker than cgroups (they count *per process*, not per process tree). There is **no syscall filter** on macOS, so agentty reports it as `none`. On Windows: an AppContainer, a restricted token, a job object and process mitigation policies. Memory, CPU and process caps are as strong as cgroup2. Filesystem and exec walls are `partial`, because Windows enforces ACLs per object, not per path prefix. There is no syscall filter either (mitigation policies are a fixed menu of hardening switches, not a programmable filter).
:::

## Which backend, and why

| Platform | Engine | What it puts up | Fallback |
|---|---|---|---|
| Linux | **claybin** | namespaces, Landlock, seccomp, cgroup2 | none (one Linux engine) |
| macOS | **claybin** | seatbelt from your compiled policy, plus rlimits | `sandbox-exec`, one fixed profile |
| Windows | **claybin** | AppContainer, restricted token, job object, mitigation policies | **none, by design** |

**Linux.** claybin replaced bwrap. Both build file read/write walls, but only claybin adds a syscall filter, memory/CPU/process limits, runtime `ptrace` and `kill` decisions, and the filtering proxy. `--sandbox-backend bwrap` still parses for old scripts, says what it did, and runs claybin.

**macOS.** Both engines use seatbelt. claybin calls `sandbox_init` with a profile compiled from *your* policy; `sandbox-exec` runs one fixed profile.

| Capability | claybin | sandbox-exec |
|---|---|---|
| file read/write/exec walls | yes | yes, but fixed |
| network through the filtering proxy | **yes** | **no** (open) |
| your **Settings → Sandbox** choices apply | **yes** | **no** |
| extra readable / masked paths | **yes** | **no** |
| per-capability guarantee report | **yes** | **no** |
| memory / CPU / process limits | **partial** (rlimits) | **no** |
| syscall filter | no (platform has none) | no |

The rlimits count *per process*, not per process tree: four children under a 1 GB cap can use 4 GB together. So memory and CPU report `partial`, and the process limit reports `advisory`, because macOS counts it per *user account*, not per sandbox.

What claybin's macOS profile closes that a simple seatbelt profile leaves open (each has a test that runs on every build):

- **Your clipboard and keychains.** macOS system services run outside any sandbox. A profile that allows all of them lets a command read the clipboard (`pbpaste`) and list your keychains. agentty allows only the few real tools need.
- **The map of your disk.** A file's *metadata* (does `~/.ssh/id_ed25519` exist, how big is it) leaks even when its contents are walled off. A command can only look up the folders on the way to what it was granted.
- **The network.** A command can reach one place: agentty's proxy, on a private port on `127.0.0.1`. No LAN, no other local services, no DNS of its own.
- **Apple's own tools still work.** `/usr/bin/git`, `cc` and `python3` are launchers that load the real tool from Xcode and write a cache to your per-user temp folder. Both are allowed.

Two things no seatbelt profile can do: `launchctl print` (read-only) always works, because it uses a channel every process is born with; and setuid programs like `/bin/ps` refuse to run inside any sandbox on macOS.

On macOS the fallback works both ways, so a Mac missing `sandbox-exec` still gets claybin, which talks to the kernel directly.

**Windows.** There is no weaker engine and no `bwrap`. The probe either proves it can build a wall (an AppContainer or a job object) and claybin runs, or the startup line says `sandbox: unavailable, running unsandboxed` and commands run unconfined. Windows Sandbox (the Hyper-V VM) wasn't chosen: it is a whole-OS VM with seconds of boot time, no piped stdout and no per-command exit codes.

```sh
agentty --sandbox-backend claybin   # default everywhere
agentty --sandbox-backend bwrap     # accepted for old scripts; bwrap is gone
```

Technical detail, the full threat model, and how to verify all of this on your machine: [`docs/SANDBOX.md`](https://github.com/agentty-org/agentty/blob/master/docs/SANDBOX.md).
