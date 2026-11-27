# Contributing

Bug reports, fixes and well-scoped features are welcome.

- **Bugs:** open an issue with `agentty --version`, your OS, and for screen
  bugs your terminal and `$TERM`.
- **Questions:** use [Discussions](https://github.com/agentty-org/agentty/discussions)
  or the [Discord](https://discord.gg/ZEM3CTAJH7).
- **Code:** clone, then build with `scripts/build.sh` (debug preset, tests on,
  `-j12`, configure fetches the submodules). It locks `build/`, so parallel
  builds of one tree wait their turn instead of corrupting it. Plain CMake
  works too: `cmake --preset debug && cmake --build build -j 12`. If your
  change bumps a submodule (maya, claybin, jaal), say so in the PR so the
  submodule commits can be merged first.

Code conventions, building and the release flow are on
https://agentty.org/contributing. By taking part you agree to the
[Code of Conduct](CODE_OF_CONDUCT.md).
