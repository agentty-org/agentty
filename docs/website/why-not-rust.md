---
title: Why modern C++ (not Rust)
description: agentty stays modern C++ and gets the guarantees people move to Rust for, checked at compile time in the headers. Hundreds of static_assert proofs, a typed lock hierarchy, and isolated worker panics.
nav_section: Advanced
nav_order: 80
slug: why-not-rust
---

This is not a flame war. It is a claim with evidence: **agentty already gets
the guarantees people move to Rust for, at compile time, in the header next to
the code they constrain.**

fish moved from C++ to Rust in 4.0. That was right for fish. This page goes
through fish's reasons, shows why they don't apply here, and where agentty
goes further.

## The receipts, up front

- **Hundreds of `static_assert` / `consteval` proofs** in
  `include/agentty/`, evaluated on every build. A green `cmake --build` means
  every invariant below holds. No CI step to skip, no fixture to drift.
- The proofs sit **next to the code they constrain**, so a reader sees the
  contract before the implementation.

## fish's reasons, mapped onto agentty

**"The C++ toolchain was painful on old / LTS systems."** True for a shell in
every distro's base image. agentty is an application built with a pinned
toolchain and one `cmake --build`.

**"Memory safety on untrusted input."** A shell parses arbitrary command
lines, a large attack surface. agentty passes its untrusted input (prompt,
model output, tool results) through **one thin dynamic boundary**. Past it,
every value is typed: `std::expected<T,E>`, closed `variant` state machines,
tagged identities. The bugs that hit agentty in practice were protocol and
rendering logic bugs, which the borrow checker does *not* catch. agentty pins
those invariants at compile time.

**"Cargo dependency management."** A convenience, not a reason to rewrite a
working codebase.

**"Rust's `#[test]` discipline."** This is the crux. A Rust contributor
proves a table correct with `#[test]` run in CI. agentty writes `consteval`
predicates and `static_assert`s them. The proof runs **every build**, can't be
skipped, and lives in the same header as the table.

## The exhibit — proofs you can read in five minutes

- **`tool/policy.hpp`** — the permission matrix `(EffectSet × Profile) →
  Decision` is a pure `constexpr` function; every cell is `static_assert`ed.
- **`provider/error_class.hpp`** — `classify(HttpError) → ErrorClass` is
  exhaustive on the enum and every retry-relevant status is pinned
  (`{Status,429} == RateLimit`, `{Status,401} == Auth`).
- **`tool/spec.hpp`** — the capability catalog proves name↔Kind bijection,
  "only known tools carry Exec", truncation-strategy correlation, and the one
  sanctioned scheduling divergence for `task`, all `consteval`.
- **`domain/refined.hpp`** — refinement types: a `Bounded<uint16_t,1,65535>`
  port literally cannot hold 0 or 65536.
- **`util/env.hpp`** — the env-var enum and its string catalog are proven in
  bijection at build time.

## Beyond parity: two places agentty does *more* than stock Rust

- **Lock ordering is a type, not a comment.** Every `maya::guarded<T, Rank>`
  names its rank (`util/lock_levels.hpp`). Taking a lock needs a capability
  proving the thread holds only lower ranks, so an out-of-order call does not
  compile, anywhere in the call graph. **Rust's type system does not check
  lock ordering at all**; deadlocks are memory-safe there.
- **Worker panics are isolated.** Turn workers spawn through a primitive
  whose body makes `std::terminate` *unreachable*, so one bad turn can't take
  down every session. The guarantee lives in the spawn primitive, so no call
  site can forget it.

## When moving *would* be right

Move to Rust if you want a large external plugin ecosystem and C++ scares
contributors away, if you hit real data races the borrow checker would
prevent, or if you are rewriting from scratch anyway. None of those is true
today.

:::tip
The full contract is in `docs/DESIGN.md`. Every point a Rust advocate can
raise, and how each was closed, is in `docs/RUST-CRITIQUE.md`. Or read the
five headers above.
:::
