---
title: The Rust critique, answered
description: Honest opposition research — the seven things a Rust advocate can attack in agentty's C++, ranked, with the real bug fixed, the footgun hardened, and the two structural gaps closed by primitives that exceed stock Rust.
nav_section: Advanced
nav_order: 90
slug: rust-critique
---

Companion to [Why modern C++ (not Rust)](/docs/why-not-rust). That page makes
the case *for* staying. This one lists the places a Rust user can rightly say
*"the borrow checker / `Send`+`Sync` / `Mutex<T>` would not have let you do
that."* Listing them is what **earns** the "don't move" position. Each item is
either a real bug we fixed or a Rust guarantee we knowingly trade away, said
openly.

## Scorecard

The findings, ranked, after the work:

| # | Finding | Verdict | vs. Rust |
|---|---------|---------|----------|
| 1 | Data race on per-session `model`/`profile`/`cwd` | **FIXED** | matched — a lock the reviewer sees |
| 2 | Lock ordering | **BEATS RUST** | compile-time rank + runtime tripwire; Rust checks neither |
| 3 | Detached-thread panic = process death | **BEATS RUST** | terminate structurally unreachable in the spawn primitive |
| 4 | `Message&` held across a mutating loop | **HARDENED** | loud abort; Rust borrow-checks it |
| 5 | `.value()` that can abort | wash | same as `.expect()` |
| 6 | `catch(...) {}` swallow | wash | `?`/`match` forces acknowledgement |
| 7 | raw `new` / `reinterpret_cast` | non-issue | (nothing to fix) |

## 1. The real bug (fixed)

`Session::model`/`profile`/`cwd` were guarded by neither the session-map mutex
nor the per-session thread mutex. `find_session()` releases the map lock before
returning, so a `session/set_config_option` writing `model` mid-turn raced the
streaming worker reading it: a torn `std::string` read, which is UB. **This is
the Rust thesis in one bug:** locking by convention covered `messages` but
missed three fields, and the compiler said nothing.

The fix locks the write sites and snapshots `model`/`profile` once under the
lock at turn start (a mid-turn change applies next turn). The lock is one a
reviewer can see.

## 2 & 3. The two structural gaps (now beat Rust)

**Lock ordering** was enforced by four identical comments. Now there is one
lock: all sessions live in a `maya::guarded`, reached only through a
captureless body that can't take a second lock or leak a reference into a
session. The turn loop works on snapshots and writes results back by id, so
no lock is held across a stream or a tool run.

**Detached worker threads** meant one escaping exception called
`std::terminate` and killed every session. Now both spawn sites use a
primitive whose body makes `std::terminate` unreachable, and the owned variant
joins on destruction (structured concurrency). The isolation can't be
forgotten because the spawn primitive owns it.

## 4. The footgun (hardened)

`run_tools` holds a `Message&` into the messages vector across the whole tool
loop. That is safe only because "the worker never appends here." Rust would
enforce that at compile time. We can't, so we made it **loud**: the code
records the vector's storage address up front and `std::abort`s with a marker
if it reallocates. A silent use-after-realloc becomes an immediate abort.

## 5–7. Washes and non-issues

`.value()` that aborts is the same as Rust's `.expect()` — a panic, not UB.
The remaining `catch(...) {}` sites route through a diagnostic log so nothing
is silently swallowed. And the raw-memory grep hits are almost all comments,
string literals, or one audited base64 codec — no raw owning `new`/`delete` in
application logic.

## Bottom line

Every finding a Rust advocate can raise is now closed. The one real bug is
fixed, the aliasing footgun is a loud abort, and the two structural gaps are
closed with C++ primitives that **exceed** stock Rust. On the axes people move
to Rust for, agentty does more, with the proofs in the headers and a green,
zero-warning build as evidence.
