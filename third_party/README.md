# third_party/

Vendored dependencies. Everything in here is a git submodule — code agentty
consumes but does not own. The top level of the repo is agentty's own
(`src/`, `include/`, `tests/`, `cmake/`, `docs/`); this directory is the rest.

| Directory      | Target(s)                  | Role                                          |
|----------------|----------------------------|-----------------------------------------------|
| `maya/`        | `maya::maya`, `maya::app`  | TUI framework: rendering, layout, widgets     |
| `mcp-cpp/`     | `mcp::mcp`, `mcp::tools`   | MCP protocol **and the entire tool set**      |
| `acp-cpp/`     | `acp::acp`                 | Agent Client Protocol (`agentty acp`)         |
| `jsonrpc-cpp/` | `jsonrpc::jsonrpc`         | header-only JSON-RPC 2.0 shared by mcp and acp |
| `rag-cpp/`     | `ragcpp::ragcpp`           | retrieval for `search_docs` / `search_code`   |
| `claybin/`     | `claybin`                  | sandbox for every command the agent runs      |
| `richpaste/`   | `rp`                       | clipboard reads for Ctrl+V (images, files, ssh) |

`maya` carries its own submodule at `maya/third_party/jaal` — jaal is the
generic Elm-architecture runtime kernel. So the dependency chain is
**agentty → maya → jaal**.

All of them are hard requirements; a missing or empty checkout is a configure
`FATAL_ERROR`, not a silent degrade. In particular mcp-cpp cannot be switched
off (`AGENTTY_MCP=OFF` fails loudly) because there is no native tool fallback,
rag-cpp must not be quietly replaced with no-op retrieval stubs, and claybin
is no longer optional (there is no `AGENTTY_CLAYBIN` flag): a sandbox setting
with no engine behind it would be worse than none.

claybin's design, threat model and the planned microvm backend are in
`claybin/docs/` (`design.md`, `platforms.md`, `microvm.md`).

## Getting them

```sh
git submodule update --init --recursive
```

## Updating them

Builds deliberately do **not** touch the submodules: fetching perturbs header
mtimes and needlessly rebuilds the ~64 TUs that include them. Sync on demand:

```sh
cmake --build build --target submodules_sync   # or: ./resync.sh
```

The pull refuses to touch a submodule holding local work — unstaged changes,
staged-uncommitted work, or local commits not on origin — and uses
`merge --ff-only`, so it never discards anything. jsonrpc-cpp is header-only
and not part of the sync; bump it by hand with
`git -C third_party/jsonrpc-cpp pull --ff-only`.

## Notes

- Include paths are unaffected by this directory: headers arrive through target
  include dirs, so it is still `#include <maya/widget/composer.hpp>`, never a
  path relative to here.
- Each submodule runs its own test suite. Embedded in agentty their
  test options (`MAYA_BUILD_TESTS`, `CLAYBIN_TESTS`, `RICHPASTE_TESTS`, ...)
  are OFF, so a change to a submodule can be green here and red in its own
  CI. To check one properly, build it standalone from its directory, e.g.
  `cmake -B build -DMAYA_BUILD_TESTS=ON` inside `third_party/maya/` or a plain
  `cmake -B build` inside `third_party/claybin/` — that is what their CI
  builds. agentty's own end-to-end sandbox check is
  `cmake --build build --target sandbox_live_check && ./build/sandbox_live_check`.
- `add_subdirectory` mirrors these paths into the build tree, so the artifacts
  land at `build/third_party/<name>/` (e.g.
  `build/third_party/mcp-cpp/examples/date-mcp/date_server`).
