# AgenttyTarget.cmake — what platform this build is for, asked of the headers.
#
# Included right after project(), before anything branches on the platform.
# The answer is jaal's one decision, re-exported by maya (maya/core/
# target.hpp), asked through maya's door (maya/cmake/MayaTarget.cmake):
# agentty → maya → jaal, and no second copy in WIN32 / APPLE / UNIX /
# CMAKE_SYSTEM_NAME. Sets, ON/OFF:
#
#   MAYA_TARGET_WINDOWS  MAYA_TARGET_APPLE    MAYA_TARGET_LINUX
#   MAYA_TARGET_ANDROID  MAYA_TARGET_POSIX    MAYA_TARGET_MINGW
#   MAYA_TARGET_GNU_LINUX  (Linux, not Android)
#   MAYA_TARGET_ELF_UNIX   (POSIX, not Apple)
#
# The one_os_cmake lint (tests: one_os_cmake) holds every other build file to
# these names. Toolchain questions -- MSVC, CMAKE_CXX_COMPILER_ID, which flags
# a compiler takes -- are a different axis and stay as they are.

include_guard(GLOBAL)

set(_agentty_maya_target ${CMAKE_SOURCE_DIR}/third_party/maya/cmake/MayaTarget.cmake)
if(NOT EXISTS ${_agentty_maya_target})
    # No in-tree maya (a tarball without submodules): fetch the source now,
    # once, so the platform can be asked before anything else is configured.
    # AgenttySubmodules.cmake then uses this checkout instead of cloning again.
    include(FetchContent)
    set(_agentty_maya_src ${CMAKE_BINARY_DIR}/_deps/maya-src)
    if(NOT EXISTS ${_agentty_maya_src}/cmake/MayaTarget.cmake)
        FetchContent_Populate(agentty_maya_early QUIET
            GIT_REPOSITORY https://github.com/agentty-org/maya.git
            GIT_TAG        master
            GIT_SHALLOW    TRUE
            SOURCE_DIR     ${_agentty_maya_src}
            BINARY_DIR     ${CMAKE_BINARY_DIR}/_deps/maya-early-build
            SUBBUILD_DIR   ${CMAKE_BINARY_DIR}/_deps/maya-early-subbuild)
    endif()
    set(FETCHCONTENT_SOURCE_DIR_MAYA ${_agentty_maya_src} CACHE PATH
        "maya checkout (fetched early by AgenttyTarget.cmake)" FORCE)
    set(_agentty_maya_target ${_agentty_maya_src}/cmake/MayaTarget.cmake)
endif()
include(${_agentty_maya_target})
maya_target_facts()
