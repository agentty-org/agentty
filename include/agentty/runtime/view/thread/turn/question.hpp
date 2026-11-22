#pragma once
// The `question` tool's card: the picker the user answers, drawn inline below
// the live turn exactly where the permission prompt goes.
#include <maya/element/element.hpp>

#include "agentty/domain/question.hpp"

namespace agentty::ui {

[[nodiscard]] maya::Element question_card(const question::State& s);

} // namespace agentty::ui
