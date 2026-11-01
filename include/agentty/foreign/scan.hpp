#pragma once
// agentty::foreign::Scan — the picker's view of other agents' sessions.
// Plain data, so the Model can hold it; the reading lives in sessions.hpp.

#include "agentty/domain/conversation.hpp"

#include <vector>

namespace agentty::foreign {

// Every user session found, as picker headers (no messages, foreign_only
// set), newest first. `skipped` remembers files that aren't user sessions
// (subagent or SDK runs, empty files) so a rescan doesn't read them again.
struct Scan {
    std::vector<Thread>         sessions;
    std::vector<Thread::Import> skipped;
};

}  // namespace agentty::foreign
