// tool_body_preview — `bash` / `diagnostics` tool body.
//
// BashOutput (tail-oriented, structured-extraction fallback chain in maya
// picks up gtest-style `N tests passed` summaries and compiler-error rows,
// otherwise falls back to a dim 4-row tail). `failed` is wired for the
// inline `· exit N` suffix on the last line of failed output. We keep
// BashOutput on failure (instead of routing to Kind::Failure) so the
// timeline card border + status icon carry the failure signal and the body
// stays calm — agent_session.cpp's "no double-flagging" discipline.

#include "tool_body_common.hpp"

#include <string>
#include <utility>

#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_args.hpp"

namespace agentty::ui::detail {

namespace {
// Matches the start of tools::util::sandbox::denial_note().
constexpr std::string_view kSandboxNoteTag = "\n\n[sandbox] ";
}

bool bash_body(const ToolUse& tc, maya::ToolBodyPreview::Config& out) {
    using Kind = maya::ToolBodyPreview::Kind;

    if (tc.is_running() && !tc.progress_text().empty()) {
        out.kind = Kind::BashOutput;
        out.text = clean_output(tc.progress_text());
        out.text_color = text_primary;   // plain output reads as normal text
        out.is_streaming = true;
        return true;
    }
    if (tc.is_terminal()) {
        std::string raw = tc.is_failed() ? failure_body_text(tc)
                                         : std::string{card_output(tc)};
        // The [sandbox] note is written for the model. The card shows the
        // user a short version instead.
        if (auto at = raw.find(kSandboxNoteTag); at != std::string::npos) {
            raw.resize(at);
            out.note = "likely blocked by the sandbox (approving doesn't change that)\n"
                       "run it yourself, allow it in Settings \xe2\x80\xba Sandbox, "
                       "or use --sandbox off";
        }
        auto stripped = clean_output(strip_bash_output_fence(raw));
        if (!stripped.empty()) {
            out.kind = Kind::BashOutput;
            out.text = std::move(stripped);
            out.text_color = text_primary;   // plain output reads as normal text
            out.failed = tc.is_failed();
        }
        return true;
    }
    return true;
}

} // namespace agentty::ui::detail
