#pragma once
// Per-tool view-side helpers. NOT an adapter for any single widget —
// these are pure mappings used by the AgentTimeline adapter (display
// name, category color/label, event status, one-line detail).
//
// Pure data — no Element work, no Config construction.

#include <string>
#include <string_view>

#include <maya/style/color.hpp>
#include <maya/widget/agent_timeline.hpp>

#include "agentty/domain/conversation.hpp"

namespace agentty::ui {

[[nodiscard]] std::string             tool_display_name(const std::string& name);
[[nodiscard]] maya::Color             tool_category_color(const std::string& name);
[[nodiscard]] std::string_view        tool_category_label(const std::string& name);
[[nodiscard]] maya::AgentEventStatus  tool_event_status(const ToolUse& tc);
[[nodiscard]] std::string             tool_timeline_detail(const ToolUse& tc);
// A subagent's tool call as a throwaway ToolUse, so it goes through the
// same name/detail/status builders as a top-level call. `t0` anchors its
// timestamps (only the duration is known).
[[nodiscard]] ToolUse                 child_as_tool_use(const ToolUse::Child& c,
                                                        std::chrono::steady_clock::time_point t0);

} // namespace agentty::ui
