#pragma once
// agentty::tools::question_tool — the `question` tool: the model asks the user
// multiple-choice questions and waits for the answers.
//
// The TUI answers it, not this ToolDef: kick_pending_tools sees a `question`
// call, opens the picker (Model::Domain::pending_question) and settles the
// call with the user's answers. execute() below runs only on paths with no
// one to ask, and says so.
//
// The switch (Settings → General → "Ask clarifying questions") follows
// web_search_policy's shape: persisted with the settings, read off the UI
// thread by every request builder through withheld_by_policy(), so it lives
// in one process-wide slot. Only the TUI's dispatch seam installs it
// (PublishQuestions). Nothing else does, so `agentty run`, `agentty acp`,
// `mcp serve` and subagents never advertise a tool nobody can answer.

#include <string_view>

#include "agentty/tool/registry.hpp"

namespace agentty::tools::question_tool {

inline constexpr std::string_view kName = "question";

// The ToolDef, appended to the native registry.
[[nodiscard]] const ToolDef& def();

// Is there someone to ask, and do they want to be asked? False until the TUI
// installs its setting.
[[nodiscard]] bool enabled() noexcept;
void install(bool on) noexcept;

}  // namespace agentty::tools::question_tool
