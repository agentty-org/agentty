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
