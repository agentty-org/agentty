# AGENTS.md — notes for coding agents working in this repo

## Building

- **Always build through `scripts/build.sh`, at `-j12`** (the script's default):
  ```sh
  scripts/build.sh                          # debug preset (tests on), all targets
  scripts/build.sh -- --target agentty      # just the binary (fastest loop)
  scripts/build.sh --preset release         # reconfigure the tree as release
  ```
- Several sessions often share `build/`. The script holds an exclusive lock on
  it, so if it prints `another build holds … — waiting`, let it wait. Don't
  kill the other build, and don't run `cmake --build build` / `ninja -C build`
  directly, because that skips the lock.
- Never kill a build mid-write unless you have to. If you see
  `ninja: warning: premature end of file; recovering` and objects keep
  rebuilding, the logs are torn: `ninja -C build -t recompact` (the script does
  this automatically).
- Full builds take several minutes. Run them as a background process and poll,
  rather than in a blocking command with a short timeout.
- Configure fetches missing submodules itself. Parallelism is also capped by
  RAM-sized Ninja pools (`AGENTTY_COMPILE_JOBS` / `AGENTTY_LINK_JOBS`). On a
  12-thread / 32 GB box, configure with `-DAGENTTY_COMPILE_JOBS=12` so the pool
  matches `-j12`.

## Tests

- `cd build && ctest -R <name> --output-on-failure`.
- Standalone suites (MCP, logx, PTY, …) live in `agentty_standalone_tests`. Build
  that target first (`scripts/build.sh -- --target agentty_standalone_tests`),
  otherwise ctest reports them as *Not Run*. A few `race` tests are their own
  targets, e.g. `mcp_connection_race_test`.

Details: [docs/website/building.md](docs/website/building.md).

## Tool performance

shell, edit, read, grep and write are ~92% of all tool calls, and every one
runs on the agent's critical path. The rules below come from bugs that made
edit 20x and shell 2x slower than they had to be, with every test green.

- **Measure, don't guess.** `python3 scripts/tool_latency.py <bin>` times the
  real tools through `agentty mcp-serve`; profile with `perf record
  --call-graph dwarf` wrapping that process, on a RelWithDebInfo build (debug
  numbers mislead). `third_party/mcp-cpp/tests/edit_stress --bench` times
  edit on 1 MiB files.
- **Cost must scale with the change, not the file or the tree.** Diffs window
  to the changed lines (`myers::window`); edits splice in place; nothing
  copies or re-splits a whole buffer per edit.
- **Per-command work must be cached and change-driven.** Anything a spawn
  needs about the workspace (handoff snapshot, secret mask, host caps) is
  answered from a cache invalidated by mtime/inotify, never re-walked. Keep
  caches exact: a cache that can go stale is a sandbox hole.
- **Use the libc primitives.** `memchr`/`memmem`/`memcmp`, readdir's `d_type`
  (`directory_entry::is_directory()`, not `symlink_status()`), one `lstat`
  per path, `string::find` is NOT fast for code (it stalls on the needle's
  first byte; use `mcp::tools::util::find_bytes`).
- **No O(n^2) where n can be big:** no sort-per-insert, no LCS tables, no
  linear scans inside per-line or per-entry loops.
- **Guarded by:** `spawn_work_budget_test` (counts walks/re-reads/probes per
  spawn: an unchanged tree must cost 0), `edit_stress` (speed budgets), and
  the nightly `tool_latency.py --assert` ceilings. If you make a tool
  faster for good, tighten its ceiling.
