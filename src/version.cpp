// The one TU compiled with -DAGENTTY_VERSION. See agentty/version.hpp.

#include "agentty/version.hpp"

#ifndef AGENTTY_VERSION
#define AGENTTY_VERSION "0.0.0-dev"
#endif

namespace agentty {

const char* version() noexcept { return AGENTTY_VERSION; }

const std::string& user_agent() {
    static const std::string ua = "agentty/" AGENTTY_VERSION;
    return ua;
}

}  // namespace agentty
