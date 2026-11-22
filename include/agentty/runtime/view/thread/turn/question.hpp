#pragma once
// The `question` tool's card: the picker the user answers, drawn inline below
// the live turn exactly where the permission prompt goes.
#include <maya/element/element.hpp>

#include "agentty/domain/question.hpp"

namespace agentty::ui {

// `drafting`: the composer below holds a typed answer, which Enter will send.
[[nodiscard]] maya::Element question_card(const question::State& s, bool drafting = false);

} // namespace agentty::ui
