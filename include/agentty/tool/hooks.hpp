#pragma once
// agentty::tools::hooks — user-authored lifecycle hooks with an explicit
// consent gate.
//
// A hook is a shell command the user configures to run around tool calls:
//
//   PreToolUse   (pre_tool)   before the permission check. Can deny, ask,
//                allow, or rewrite the call's arguments.
//   PostToolUse  (post_tool)  after a tool succeeds, before the model reads
//                the result. Can replace the result or add a note.
//   PostToolUseFailure        after a tool fails. Can add a note.
//
// Two config shapes, both read from the same file:
//
//   agentty's own (unchanged):
//     { "pre_tool":  [ {"match": "shell", "run": "./guard.sh"} ],
//       "post_tool": [ {"match": "edit|write", "run": "fmt.sh"} ] }
//
//   Claude Code's, so tools written for it (RTK, guards, formatters) work
//   as they are:
//     { "hooks": { "PreToolUse": [ { "matcher": "Bash",
//         "hooks": [ {"type": "command", "command": "rtk hook claude",
//                      "timeout": 10} ] } ] } }
//
// Every hook gets the event as JSON on stdin, in Claude Code's shape
// (hook_event_name, tool_name, tool_input, tool_response, cwd, ...). Tool
// names are Claude Code's (Bash, Read, Edit, Write, Grep, Glob, WebFetch)
// and args are translated both ways, so a hook that rewrites
// tool_input.command for "Bash" rewrites agentty's shell command.
//
// What a hook can answer, Claude Code's protocol:
//   exit 2                  block; stderr is the reason the model reads
//   exit 0 + JSON on stdout hookSpecificOutput.{permissionDecision,
//                           permissionDecisionReason, updatedInput,
//                           additionalContext} for PreToolUse;
//                           {decision:"block", reason}, updatedToolOutput,
//                           additionalContext for PostToolUse;
//                           continue:false + stopReason, systemMessage
//   other exit codes        a non-blocking error, logged
// agentty's legacy pre_tool entries keep their meaning: any non-zero exit
// blocks, with stdout as the reason.
//
// Hooks of one event run in file order, each seeing the input as the
// previous one left it. Decision precedence is deny > ask > allow.
//
// The payload is also written to AGENTTY_HOOK_PAYLOAD_FILE for older
// hooks, and these env vars are set:
//   AGENTTY_HOOK_EVENT   pre_tool | post_tool
//   AGENTTY_HOOK_TOOL    tool name (agentty's)
//
// ── SECURITY: the consent gate ─────────────────────────────────────────
// Hooks are arbitrary shell that runs AUTOMATICALLY on every matching tool
// call — the single most dangerous extension surface an agent host can
// offer (a malicious repo could ship .agentty/hooks.json and exfiltrate on
// the first `read`). agentty therefore NEVER runs a hooks file that has
// not been explicitly approved:
//
//   • The FIRST time a hooks file (or a changed version of one) is seen,
//     its hooks DO NOT RUN. agentty surfaces a one-line notice telling the
//     user to run `agentty hooks approve` to inspect + approve it.
//   • `agentty hooks approve` prints the full file, asks for explicit
//     y/N confirmation on the terminal, and stores the file's SHA-256 in
//     ~/.agentty/hooks_approved.json.
//   • Any byte change to the file invalidates the approval (hash mismatch
//     → back to not-running + notice). There is no "approve forever".
//   • AGENTTY_NO_HOOKS=1 disables the whole subsystem.
//
// Hook commands run through the SAME sandbox wrapper as the bash tool
// (bwrap / sandbox-exec when enabled), so an approved-but-compromised hook
// is still workspace-confined.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "agentty/util/io.hpp"

#include <nlohmann/json.hpp>
#include <maya/runtime.hpp>

namespace agentty::tools::hooks {

// What the PreToolUse hooks decided. Default: no hook spoke, nothing changes.
struct PreToolDecision {
    enum class Permission : std::uint8_t { None, Allow, Ask, Deny };
    Permission permission = Permission::None;
    // Deny: the error the model reads. Ask: shown in the permission prompt.
    std::string reason;
    // Set when a hook rewrote the call, in agentty's arg shape.
    std::optional<nlohmann::json> updated_args;
    // Added to what the model sees with the tool's result.
    std::string additional_context;
    // A hook said continue:false. The turn ends after this tool.
    bool stop = false;
    std::string stop_reason;
    // Shown to the user as a toast (systemMessage), never to the model.
    std::string system_message;

    [[nodiscard]] bool blocked() const noexcept { return permission == Permission::Deny; }
};

// Run the PreToolUse hooks for one call. `session_id` and `tool_use_id` go
// into the payload. Never throws; a hooks file that is missing, unapproved
// or unparseable decides nothing.
[[nodiscard]] PreToolDecision
run_pre_tool(std::string_view tool, const nlohmann::json& args,
             std::string_view tool_use_id = {}, std::string_view session_id = {});

// What the PostToolUse hooks decided.
struct PostToolDecision {
    // Replaces what the model reads as the tool's result.
    std::optional<std::string> updated_output;
    // decision:"block" + reason, or additionalContext: added next to the
    // result for the model.
    std::string additional_context;
    bool stop = false;
    std::string stop_reason;
    std::string system_message;
};

// Run the PostToolUse hooks (PostToolUseFailure when `failed`).
[[nodiscard]] PostToolDecision
run_post_tool(std::string_view tool, const nlohmann::json& args,
              const std::string& result_text, bool failed = false,
              std::string_view tool_use_id = {}, std::string_view session_id = {},
              std::int64_t duration_ms = -1);

// The matchers of the approved PreToolUse hooks ("*" for match-all), or
// none. Reads the disk: call it on a worker (cmd::load_library does), and
// keep the result on the Model so the reducer can ask pre_tool_matches
// without IO.
[[nodiscard]] std::vector<std::string> pre_tool_matchers(Io io);
// Pure: would any of these matchers pick `tool`?
[[nodiscard]] bool pre_tool_matches(const std::vector<std::string>& matchers,
                                    std::string_view tool);

// Claude Code's name for an agentty tool ("shell" -> "Bash"), and the args
// translated to its field names. Exposed for tests.
[[nodiscard]] std::string cc_tool_name(std::string_view tool);
[[nodiscard]] nlohmann::json to_cc_input(std::string_view tool, const nlohmann::json& args);
[[nodiscard]] nlohmann::json from_cc_input(std::string_view tool, const nlohmann::json& in,
                                           const nlohmann::json& original);

// True when a hooks file exists whose hash is NOT approved (drives the
// one-line TUI notice). Cheap: stat + memoised hash.
[[nodiscard]] bool pending_approval(Io);

// Path of the active hooks file (project first, then user), empty if none.
[[nodiscard]] std::string active_file(Io);

// The `agentty hooks` subcommand: `list` shows configured hooks + approval
// state; `approve` prints the file and interactively approves it. Returns
// a process exit code.
int cli(Io, const maya::unlocked&, const std::string& verb);

} // namespace agentty::tools::hooks
