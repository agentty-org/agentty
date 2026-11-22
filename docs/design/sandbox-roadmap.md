# Sandbox roadmap — making claybin robust, and agentty's use of it simpler

Status: proposal. Companion to [`sandbox-boundary.md`](sandbox-boundary.md)
(the design record) and [`../SANDBOX.md`](../SANDBOX.md) (the reference).

This document does three things: compares claybin with the 2026 state of the
art, lists what is broken or missing (with evidence), and proposes an ordered
plan. Every claim about current behaviour is either measured or cites code.

---

## 1. Where the field is (late 2026)

Every serious local coding agent has converged on the same shape: **an OS
process sandbox around spawned commands, plus a network proxy that enforces a
domain allowlist.** No containers, no VM, on the developer's machine.

| | Claude Code (`sandbox-runtime`) | OpenAI Codex CLI | agentty + claybin |
|---|---|---|---|
| Linux mechanism | bubblewrap (needs userns) + optional seccomp | Landlock + seccomp (no userns needed) | mount-ns tree **or** Landlock, + seccomp-bpf allowlist, cgroup2, seccomp-notify broker |
| macOS | Seatbelt (`sandbox-exec`) | Seatbelt | Seatbelt via `sandbox_init` |
| Windows | — | restricted token / AppContainer | AppContainer + restricted token + job |
| Network | proxy, domain allowlist, deny unknown | off by default; proxy allowlist, DNS-rebinding check, private ranges blocked | own netns + SOCKS/HTTP proxy, private ranges refused, `Allowed hosts` |
| Applies to | bash + its children (and MCP servers via `srt`) | every spawned command | shell tool, diagnostics, git, hooks, ACP terminals |
| Missing-backend behaviour | falls back unsandboxed unless `failIfUnavailable` | refuses | Auto: unsandboxed + banner; On: refuse |

**Lessons the field learned the hard way:**

1. **Path-based rules lose to an agent that reasons** (Ona, March 2026: Claude
   Code ran a denied `npx` as `/proc/self/root/usr/bin/npx`, then disabled its
   own sandbox). Denylists of *commands* are not a boundary. Only kernel-level
   resource walls (files, sockets, syscalls) hold, and the agent must not be
   able to change the policy it runs under.
2. **"Retry outside the sandbox" is the escape hatch.** Claude Code's
   `allowUnsandboxedCommands: false` exists to close it.
3. **Fail-open on a missing backend is the default failure mode** (Claude
   Code's `failIfUnavailable`). A sandbox that silently isn't there is worse
   than none, because people trust it.
4. **Network allowlists need DNS-rebinding and private-range checks**, and
   CONNECT-by-name through the proxy, not raw IPs (Codex).
5. **Landlock ABI 6+ scoping** (`LANDLOCK_SCOPE_SIGNAL`,
   `LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET`) composes with network access where a
   netns can't; ABI 4+ gives per-port TCP rules without namespaces.

claybin is already ahead of both reference implementations on several axes:
an allowlist seccomp profile (not a denylist), cgroup2 caps, a syscall broker,
an honest per-capability guarantee report, Landlock scoping at ABI ≥ 6,
io_uring / userfaultfd / keyctl / bpf / clone3 / TIOCSTI all handled, and a
real escape-attack corpus. The problems below are mostly about the **edges**:
the hosts that lack user namespaces, and the processes agentty spawns that
never reach claybin.

---

## 2. Findings

### F1 — On hosts without user namespaces, the shipped posture fails, and secret masks do not hold  *(critical, measured)*

Ubuntu 24.04's AppArmor default denies unprivileged user namespaces. This is
the configuration the design record (§3a) says claybin exists to serve.

Reproduced on this machine by running the existing tools inside
`bwrap --unshare-user --disable-userns` (a process that cannot create a nested
user namespace — the same position as an unconfined binary on Ubuntu 24.04):

- **`sandbox_audit`**: every preset — shipped default, permissive, balanced,
  hardened, airgapped — reports
  `compile refused: creating a symlink would mutate the host filesystem`.
  The merged-`/usr` symlinks agentty adds (`/bin -> usr/bin`, …,
  `sandbox.cpp` `system_links`) are `Fidelity::impossible` without a mount
  namespace (`claybin/include/claybin/policy/mounts.hpp`, `MountKind::symlink`).
  agentty fails closed (`run_shell_command`'s spawner returns `start_error`),
  so **every shell command errors** — the sandbox is not weak there, it is
  unusable. `sandbox_parity_test` claims the opposite and passes only because
  it builds its own policy without `system_links`.
- **`sandbox_live_check`**: 23 checks fail. The worst:
  `workspace .env is masked: child said SECRET=canary — FAIL`, plus nested
  `.env` / `*.pem` masks, `host-readable still masks .env and ~/.ssh`, and
  the proxy's RFC1918/loopback refusals.
- **Root cause in claybin.** Without a mount namespace, a mask under a granted
  directory becomes an `FsAuthority::deny` under a grant. Landlock has no
  negative rules, so the deny **cannot be expressed** — compile.cpp
  L1097–1111 says so in a comment ("grant $HOME, deny $HOME/.aws is not
  expressible here at all") and then silently skips it
  (`if (allowed == 0) continue;`). The guarantee report still says
  `filesystem.read strong (landlock)`. A minimal policy
  (`bind ws`, `mask ws/.env`) reproduces it: `fs_read=strong`, `cat .env` →
  exit 0, `SECRET=leaked`.

This is the "boundary that reports itself as active while enforcing nothing"
the design record names as the one failure mode this subsystem exists to
prevent.

### F2 — Long-lived children are never sandboxed  *(high)*

`util::ChildProcess` (`include/agentty/util/child_process.hpp:282`) spawns via
`maya::platform::spawn` with no sandbox, and it is used for:

- **MCP stdio servers** — `src/mcp/bridge.cpp:488`, `src/mcp/connection.cpp:171`
- **ACP agents** (Claude Code, Codex, Gemini, Devin, OpenCode) — `src/provider/external_acp_backend.cpp:670`
- **RAG peers** — `src/rag/rag_host.cpp:53`

An MCP server is third-party code (`npx some-mcp-server`) that runs with the
user's full authority: `~/.ssh`, `~/.aws`, every repo, the network. This is
the supply-chain path the Shai-Hulud variant used (rogue MCP servers injected
into developer tools). `docs/SANDBOX.md` does not mention MCP at all, so users
reasonably assume "every process agentty runs" means these too. Claude's
`srt` explicitly targets "local MCP servers"; agentty does not.

Also unsandboxed, deliberately: `cmd_code_block_run.cpp:209` (the user runs a
code block on the real tty) and `airgap.cpp` (ssh). Those are the user's own
actions, so that is fine — but it should be written down.

### F3 — Policy is assembled per call site  *(medium, design)*

`sandbox.cpp` has five near-identical spawner lambdas (L818, L881, L1098,
L1136, L1521) that each call `build_claybin_posture()`, apply
`relax_wx_for_jit`, sometimes patch `cwd` / `write_paths`, and copy the same
`SpawnResult` fields into `SpawnedChild` — with drift already visible: the
Linux argv path copies `supervisor_fd`, `service`, `keepalive`, `kill_tree`;
the Windows argv path copies only `keepalive` and `kill_tree`. Every new kind
of child (F2) would add a sixth.

### F4 — No user-visible distinction between "sandbox blocked it" and "it failed"  *(medium)*

There is a blocked-activity feed and a model-facing stanza (`prompt.cpp`
`sandbox_stanza`), which is good. But on a host where F1 bites, the model just
sees an opaque spawn error per command, and the banner says
`sandbox: active (claybin)` because `available()` only checks that *some* wall
can be built (`sandbox_claybin.cpp:1041`), not that *this* posture compiles.

### F5 — Smaller claybin gaps

- **Landlock audit logging (ABI 7, kernel 6.15+).**
  `landlock_restrict_self` is called with flags `0` (`apply.cpp:1539`), so
  denials are invisible to the kernel audit log. Logging same-exec denials
  would give agentty a free, kernel-sourced "blocked" feed.
- **macOS**: Seatbelt is deprecated and undocumented; the profile is our
  only wall there and has no syscall filter. Worth an explicit test matrix
  per macOS release, since Apple changes it silently.
- **DNS rebinding in the proxy is handled**: `dial_filtered`
  (`sandbox_netproxy.cpp` L461–486) resolves once, checks each address, and
  connects to that same `sockaddr` — no second lookup to rebind. Keep it
  that way; a test that pins "connect uses the checked address" is cheap.

---

## 3. Plan

Ordered by risk removed per unit of work. Each step ships on its own, with a
test that fails before and passes after.

### Step 1 — Make the no-userns path honest and working (fixes F1)  ✅ *done*

Shipped. What it took, measured under `bwrap --unshare-user --disable-userns`:

- claybin expands every **narrowing** (a mask, or a read-only path under a
  writable one) into per-entry landlock rules down the chain of directories,
  siblings granted `O_NOFOLLOW` so a symlinked sibling cannot widen into its
  target; a directory on the chain drops only the rights its narrowed entries
  must not inherit. Unlistable directories make compile refuse. Missing
  components stop the walk (nothing there to protect).
- An existing host symlink in the mount plan is `approximate`, not
  `impossible`, so merged-`/usr` hosts compile.
- `host_proxy_fallback`: without a netns, Full network goes through the same
  proxy on a host-loopback port; landlock grants TCP connect to that port only
  (UDP handled, nothing granted, on abi 10+; ICMP ping sockets refused by
  seccomp). Network None becomes a landlock deny-all. Too-old landlock ⇒
  refuse.
- The report names the per-entry form, and drops to `partial` for any
  narrowing it could not express.
- agentty's `init()` compiles the shipped posture and the banner names any
  degraded wall; `--sandbox on` refuses with the reason.
- Tests: claybin `mask_landlock_test` (12 live attacks + 4 controls) and
  `net_landlock_test`; agentty `sandbox_parity_test` asks about the shipped
  posture; `sandbox_live_check` 0 failures in both modes (5 namespace-only
  checks print `n/a`). CI job **sandbox without user namespaces** runs the
  claybin suite both ways on every push.

The original plan for this step, kept for the record:

**claybin:**

1. **A mask that Landlock cannot express must not be dropped silently.** In
   Landlock-only mode, rewrite `grant D` + `deny D/x` into grants on D's
   *siblings of x* (what the compile.cpp comment already says is "the only
   way"): enumerate D's entries at compile time and grant each one except the
   masked path, recursing down the path to the masked leaf. Files created in
   D after compile are not readable — acceptable for system roots, and for the
   workspace the grant is on the workspace's top level, so new files land in
   already-granted subdirectories in practice. Where enumeration is impossible
   or the fan-out exceeds a bound, `compile()` must **refuse** with a named
   reason (`Errc::unsupported, "mask under a landlock grant"`), never report
   `strong`.
2. **A symlink in the mount plan is optional, not impossible, without a mount
   namespace.** On the host the link already exists (merged `/usr`), so the
   Landlock interpretation is "grant the target"; only a symlink whose source
   does not already exist on the host is impossible.
3. **Guarantee report:** `filesystem.read` is `strong` only if every mask was
   enforced; otherwise `partial` with the reason.
4. **Test in claybin:** escape corpus gains "read a masked file" under a host
   model with no namespaces, plus a real run under
   `bwrap --unshare-user --disable-userns` in CI (one extra job; bwrap is
   already in the Arch container).

**agentty:**

5. `available()` compiles the *shipped* posture against the probed host, not
   just "is there a wall". If it refuses, Auto reports
   `sandbox: unavailable (…reason…)` instead of `active`, and On refuses to
   start, naming the reason.
6. `sandbox_parity_test` uses `build_claybin_posture()`, not a hand-built
   policy, so it tests what ships.
7. `sandbox_live_check` gains a `--no-userns` mode that re-execs itself under
   `bwrap --disable-userns` and is run in CI.

### Step 2 — One spawn seam: `sandbox::Child` (fixes F3, prepares F2)  ✅ *done (long-lived half)*

Shipped as `sandbox::Role` + `posture_for(role, argv)` + `spawn_long_lived`
+ `child_spawner(role)`. `util::ChildProcess::Spawn` gained a `spawner` hook
the sandbox layer fills in, so util/ never names the tool layer, and each
long-lived site is one line. The five command spawner lambdas in
`sandbox.cpp` still exist; folding them onto `posture_for(Role::Command)` is
the remaining cleanup.

### Step 3 — Sandbox long-lived children (fixes F2)  ✅ *done*

MCP stdio servers, ACP agents and rag peers all spawn through
`child_spawner`. Fail closed when the sandbox is on. Found on the way and
fixed in claybin: the setup processes that stay behind (outer waiter, pid-1
shepherd) never exec, so they kept copies of every caller descriptor — and a
copy of the guest's stdin write end meant a server never saw EOF and could
not be stopped (`stdin_eof_test`). Verified by `sandbox_live_check`'s
long-lived section in both modes: secrets unreadable, `~/.ssh` unreadable, no
direct network, workspace read-only for rag peers, EOF ends the server, an
unbuildable child is refused.

Also found and corrected while testing: expanding READ-ONLY paths under a
writable workspace (step 1's first version) took write off the workspace root
and broke `git commit` without namespaces. Only masks are expanded now; a
read-only path there makes the report say `filesystem.write partial`, and the
handoff gate is the wall.

### Step 2 — original plan, for the record

Replace the five lambdas with one value type and one function:

```cpp
namespace agentty::sandbox {

// WHAT a child is, not how to wrap it. The role picks the policy.
enum class Role : std::uint8_t {
    Command,      // shell tool, diagnostics, git, hooks, ACP terminals
    McpServer,    // long-lived stdio server: workspace rw, network per config
    Agent,        // ACP agent: needs its own config dir and network
    RagPeer,      // read-only workspace, no network
};

struct ChildSpec {
    Role                     role;
    std::vector<std::string> argv;
    std::string              cwd;                 // empty = workspace
    std::vector<std::string> extra_write;         // editor roots, an agent's own dir
    std::vector<std::string> extra_read;
    std::vector<std::pair<std::string, std::string>> env;
    Stdio                    stdio;               // pipes / pty / null
};

// The ONE place a posture is built, adjusted (JIT W^X, cwd, roots) and
// spawned. Returns a uniform handle; the Linux/macOS/Windows differences stay
// inside. Fails closed: an unsandboxable spec is an error, never a plain spawn,
// unless the sandbox mode is Off.
[[nodiscard]] std::expected<Spawned, SpawnError> spawn(const ChildSpec&);

}
```

`Subprocess::run`'s `spawner` becomes `[spec](…) { return sandbox::spawn(spec); }`
at every site. `util::ChildProcess` gains a constructor taking a `ChildSpec`
and routes through the same `sandbox::spawn`. The posture for each `Role` is a
pure function `posture_for(Role, Config)` in one file, so "what may an MCP
server touch" has exactly one answer and one test.

### Step 3 — Sandbox long-lived children (fixes F2)

Per role, on top of the Step 2 seam:

| Role | filesystem | network | notes |
|---|---|---|---|
| `McpServer` | workspace rw (masks apply), toolchains ro, its own `~/.cache/<name>` rw | per server: `none` / `proxy` (Allowed hosts) / `full` | default `proxy`; config key `"sandbox": "off"` per server for ones that genuinely need more, shown in the Plugins pane as **unsandboxed** in warning colour |
| `Agent` | workspace rw, its own config dir (`~/.claude`, `~/.codex`, …) rw | `full` (it talks to its own API) | agents already have their own sandbox for *their* tools; ours bounds the agent process itself |
| `RagPeer` | workspace ro | none | |

Rollout: default on for `RagPeer`; `McpServer` behind a setting for one
release, with the Plugins pane listing which servers run sandboxed; then
default on. Document in SANDBOX.md what is and is not sandboxed — a table,
not a sentence.

### Step 4 — The agent cannot loosen its own box (hardening against F2-style escapes)

Already mostly true (the seal, `HandoffPolicy`). Make it complete and tested:

- `~/.agentty`, `<ws>/.agentty`, `.mcp.json`, `.claude/settings*.json`,
  `.codex/config.toml` are read-only from inside the sandbox **and** refused
  by the native write/edit tools (they already share `kAlwaysMaskedNames` for
  reads — do the same for writes to config).
- There is no "retry unsandboxed" path for the model. A blocked command is
  reported as blocked; only the user can change the posture, and only by
  restart (already the design).
- An escape-corpus case for the Ona chain: `/proc/self/root/<bin>`,
  `ld.so <bin>`, copy-to-/tmp-and-exec — each must hit the same kernel walls
  (they do, because claybin never filters by command name; pin it with tests).

### Step 5 — Visibility

- Landlock ABI 7 `LANDLOCK_RESTRICT_SELF_LOG_SAME_EXEC_ON` where available,
  surfaced in the blocked-activity feed with the denied path.
- The banner/pane names the real per-capability result for the *shipped*
  posture on this host (Step 1.5), e.g.
  `sandbox: active (claybin) · network: none (no userns) · masks: enforced`.
- The model stanza says, when a capability is degraded, what that means
  ("network isolation is unavailable on this host; commands share your
  network").

### Not doing (and why)

- **gVisor / microVM by default.** Real kernel isolation, but heavy, needs
  KVM, and changes what "the workspace" means. `docs/microvm.md` already
  tracks it as the next isolation level; it should stay opt-in.
- **Command allow/deny lists as a security boundary.** Ona showed why. Keep
  approvals as a UX control and the kernel walls as the boundary.

---

## 4. Acceptance

Per step, before merging:

- `ctest -R 'sandbox|netproxy|claybin'` green.
- `sandbox_live_check` green on a normal host **and** under
  `bwrap --unshare-user --disable-userns` (today: 23 failures there).
- `sandbox_audit` under the same: no preset says `compile refused`, and no
  capability says `strong` that the live check can defeat.
