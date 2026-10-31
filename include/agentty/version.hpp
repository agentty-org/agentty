#pragma once
// agentty's version, from CMakeLists.txt's project(VERSION).
//
// Only src/version.cpp sees the AGENTTY_VERSION define. Read it through
// these functions so a version bump recompiles one file, not every TU.

#include <string>

namespace agentty {

// "0.9.38". "0.0.0-dev" on a build that bypasses our CMake.
[[nodiscard]] const char* version() noexcept;

// "agentty/0.9.38", the User-Agent agentty sends.
[[nodiscard]] const std::string& user_agent();

}  // namespace agentty
