// SPDX-License-Identifier: Apache-2.0
#pragma once
// agentty::app::apply_file_effect — what a WriteFile effect does to disk.
//
// One function, called by the host (runtime/app/host.hpp) and by the tests
// that check undo / keep against real files, so the test exercises exactly
// the bytes-on-disk behaviour the user gets.

#include <filesystem>
#include <system_error>

#include "agentty/runtime/store_fx.hpp"
#include "agentty/tool/util/fs_helpers.hpp"

namespace agentty::app {

// Best-effort: an error shows up on the next read (the pane shows the file
// unchanged), and throwing out of an effect would take the loop down for
// something the user can recover from.
inline void apply_file_effect(const WriteFile& e) {
    const std::filesystem::path p{e.path};
    if (e.remove) {
        std::error_code ec;
        std::filesystem::remove(p, ec);
        return;
    }
    (void)tools::util::write_file(p, e.contents);
}

} // namespace agentty::app
