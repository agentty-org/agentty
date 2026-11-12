---
title: Building from Source
description: Compile agentty with CMake, including the standalone static build.
nav_section: Advanced
nav_order: 60
slug: building
---

To build agentty, clone it and use a CMake preset:

```bash
git clone --recursive https://github.com/agentty-org/agentty.git && cd agentty
cmake --preset release && cmake --build build -j
```

Then run it:

```bash
./build/agentty
```

You need a C++26 compiler. See [Requirements](#requirements).

:::tip Use the presets, not a hand-rolled `-B build`
`cmake -B build` **ignores `CMakePresets.json`**. It gives you Release + LTO +
Makefiles in the same `build/` directory the presets use. Ninja and Make cannot
share a directory, so CMake refuses until you delete it. Always use `--preset`.
If you plan to change code, use [The development loop](#the-development-loop).
:::

## The development loop

Use this if you are editing agentty. The `debug` preset is the default. It uses
Ninja, Debug, ccache and lld, with no LTO. A rebuild after an edit drops from
~36 seconds to under two:

```bash
cmake --preset debug                           # once
cmake --build build --target agentty -j 12     # after each edit
./build/agentty
```

Pass `--target agentty`. Without it the build also makes every example,
benchmark and submodule tool: 19.6 s versus 0.65 s for agentty alone.

**Measured on a 12-core Linux box**, changing one `.cpp` and rebuilding:

| Configuration | Incremental rebuild |
|---------------|--------------------:|
| Release + LTO | **36.2 s** |
| `cmake --preset debug` (Debug + Ninja + ccache + lld) | **1.7 s** |

That is **21×** faster. Almost all of it is the link step: LTO re-optimizes the
whole binary for a one-line change. The first `debug` build is a normal cold
compile (~5 min). Later ones are fast.

Four settings make it fast. All are already configured:

| Setting | Why |
|---|---|
| Ninja | Tighter dependency graph than Make; parallelises the objlib fan-out well. |
| No LTO | The biggest cost in an incremental Release build. |
| `-g1` | Line tables only. Backtraces and breakpoints work; `print localvar` does not. Use `--preset debug-full` when you need it. |
| ccache | Auto-detected by `cmake/AgenttyToolchain.cmake`. Switching branches mostly hits the cache. |

### Running tests fast

Tests live in one consolidated binary plus a handful of standalone ones. Build
and run only what your change touches:

```bash
cmake --build build --target agentty_tests -j 12   # ~1.3 s incremental
./build/agentty_tests -tc="*framing*"              # ~0.16 s
```

`-tc` takes a doctest pattern and matches on test-case *names*, so
`-tc="*conformance*"` or `-tc="*framing*"` narrows to one area. A pattern that
matches nothing reports `0 passed` and still exits 0 — check the case count.

A few suites are separate executables (they need their own process to set
environment before anything initialises). Run those through ctest by name:

```bash
cd build && ctest -R "logx"      # logx_test, logx_redaction_test, logx_format_test
```

For the pre-commit gate, run everything **except** the three long fuzz/replay
tests:

```bash
cd build
ctest -j 8 -E "reveal_scrollback_test|scrollback_wire_fuzz|frozen_invariant_fuzz"
```

| Scope | Tests | Time |
|-------|------:|-----:|
| One case (`-tc=…`) | 1–2 | **0.16 s** |
| Everything but the slow three | 432 | **26 s** |
| Full suite | 435 | **110 s** |

These three take nearly all the time: `reveal_scrollback_test` is 120 s of
CPU, `scrollback_wire_fuzz` 86 s, `frozen_invariant_fuzz` 50 s. They are deep
property/fuzz runs for CI. Everything else is sub-second.

:::warn A green run proves nothing if nothing ran
Check the assertion count, not just the status line. doctest reports
`8 passed` for eight cases that asserted zero times. This happened once when
log tests depended on an environment variable the suite did not set.
`assertions: 0` is the tell.
:::

### The other presets

| Preset | What it builds |
|---|---|
| `debug` | Default. Fastest edit-build-test loop. |
| `dev` | Alias for `debug`. |
| `debug-full` | Debug + full DWARF, for `print localvar` in gdb. |
| `release` | `-O3` + thin LTO + strip. The optimized local binary. |
| `pch` | Release + shared PCH. Opt-in experiment; slower on 8-core Apple clang. |
| `ci` | Mirrors the Linux CI job (Release, no mimalloc, tests on). |
| `sanitizer` | ASan + UBSan over agentty's own-logic set. |
| `standalone` | Portable release binary, no third-party shared libs. |

Every preset builds into the **same** `build/` tree. Switching preset
reconfigures it in place, so you don't keep a second multi-gigabyte copy.
Switching back is cheap because ccache still holds the objects. To keep two
configurations at once, override the directory:
`cmake --preset debug -B build-other`.

## Requirements

- GCC 14+ / Clang 18+ / MSVC 14.40+ (`/std:c++latest`)
- CMake 3.28+
- OpenSSL and nghttp2 (FetchContent pulls maya automatically)

## Platform notes

:::warn macOS
AppleClang tops out at C++23. To build the tests (`AGENTTY_BUILD_TESTS`) on
macOS, use `g++` or stock LLVM `clang++`, not Xcode's bundled toolchain.
:::

### Standalone (static) build

```bash
cmake --preset standalone   # sets AGENTTY_STANDALONE=ON
cmake --build build -j
```

This statically links OpenSSL, nghttp2, libstdc++ and libgcc when their `.a`
archives are installed. libc stays dynamic. For a fully static binary that
runs on any Linux userland, add `-DAGENTTY_FULLY_STATIC=ON`.

The prebuilt Linux release binaries are fully static. They are linked
`-static -no-pie` into a classic `ET_EXEC` with no `NEEDED` entry and no
`PT_INTERP`. One file runs on glibc (Debian/Ubuntu/Fedora), musl (Alpine) and
64-bit Raspberry Pi OS. A build-time check (`cmake/assert_static_pie.cmake`)
fails the build if the binary gains a dynamic dependency.

Termux/Android needs a PIE. Build it with `-DAGENTTY_STATIC_PIE=ON` on a musl
toolchain.

## Optimized builds

Release builds already ship with link-time optimization and a stripped symbol table. Two opt-in levers squeeze out more, for a local build you run yourself:

```bash
# mimalloc allocator — measurable keystroke-latency win on the
# allocation-heavy render/parse paths. Not enabled on the fully-static
# release binaries (allocator override under static musl is a hazard).
cmake -B build -DCMAKE_BUILD_TYPE=Release -DAGENTTY_USE_MIMALLOC=ON

# Profile-guided optimization (two phases). Phase 1 builds an instrumented
# binary and runs a scripted PTY workload over the hot paths; phase 2
# rebuilds using the collected counters.
cmake -B build-pgogen -DCMAKE_BUILD_TYPE=Release -DAGENTTY_PGO=generate
cmake --build build-pgogen -j$(nproc) --target agentty
scripts/pgo-train.sh build-pgogen/agentty
cmake -B build-pgouse -DCMAKE_BUILD_TYPE=Release -DAGENTTY_PGO=use
cmake --build build-pgouse -j$(nproc) --target agentty
```

See [Performance](/docs/performance) for what each buys you.

## Cutting a release (maintainers)

```bash
scripts/cut-release.sh X.Y.Z       # POSIX / macOS / Linux / Git-Bash
scripts\cut-release.cmd X.Y.Z       # Windows cmd.exe

scripts/cut-release.sh X.Y.Z --dry-run   # preview the exact diff, write nothing
```

Single source of truth: `CMakeLists.txt`'s `project(agentty VERSION …)` line. `cut-release.sh` bumps it, promotes `CHANGELOG.md`'s `[Unreleased]` section to a dated `[X.Y.Z]`, commits `release: vX.Y.Z`, tags `vX.Y.Z`, and pushes. The tag push fires GitHub Actions, which builds every binary + OS package (Linux x86_64/aarch64 on native runners, macOS Intel/ARM, Windows exe/msi) and auto-submits to winget, Homebrew, Scoop, and the AUR — nix/snap/gentoo manifests are attached to the release. Guards refuse a downgrade, duplicate version, dirty tree, or existing tag.

The pipeline is **fully automatic and self-verifying** — after `cut-release.sh` there is nothing left to do by hand:

- A final `verify-release` job runs dead-last and checks each channel's LIVE state (release assets, Homebrew formula, Scoop manifest, AUR `pkgver`, a winget PR for the version). If any channel whose secret is configured did **not** reach the new version, the run goes **red** and names the channel — so a green release genuinely means every channel is up to date. Channels with no secret set are reported as skipped and never fail the gate.
- A separate `reconcile-manifests` workflow re-pins AUR/Homebrew/Scoop from the release's `SHA256SUMS` (no rebuild) **automatically when the release run completes**, and again **weekly** — so if a build leg was slow/flaky and a publisher was skipped, the package still catches up on its own.
- The winget submission gates on `checksums-final` and verifies the MSI hash against `SHA256SUMS` before opening its PR, so it can never submit a hash that drifted from the released asset.
