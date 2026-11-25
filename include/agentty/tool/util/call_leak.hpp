#pragma once
// Leaked parallel-call wrappers in tool arguments.
//
// OpenAI-family models write several tool calls internally as
//
//   {"tool_uses":[{"recipient_name":"functions.read","parameters":{...}}, ...]}
//
// (the `multi_tool_use.parallel` shape). Under a "batch your reads" prompt a
// model sometimes starts writing that wrapper INSIDE one call's arguments.
// What reaches us then takes one of three shapes:
//
//   1. the wrapper itself, well formed, as the args object
//        {"recipient_name":"functions.read","parameters":{"path":"a.go"}}
//        {"tool_uses":[{...},{...}]}
//   2. the wrapper squeezed into the tool's own schema by a host that
//      constrains decoding to it (Responses strict normalisation):
//        {"path":"cmd/server/main.go}},{","symbol":"recipient_name","offset":1}
//      — valid JSON, schema keys, garbage values. Running it reads a path
//      that does not exist and the model is told "not found", which says
//      nothing about what it did wrong.
//   3. unparsable text (`{"path":"a.go"}},{"recipient_name":...`), which
//      every parse site already refuses.
//
// unwrap_call_wrapper() recovers shape 1 when it is exactly one call to the
// tool being dispatched. call_wrapper_leak() names shapes 1 (when it can't be
// unwrapped) and 2, so the dispatcher refuses the call with an error that
// tells the model what to send instead. raw_call_wrapper_hint() is the line
// parse sites append for shape 3.
//
// Only LOCATION and IDENTIFIER fields (path, symbol, url, …) are inspected
// for shape 2: free text (a file's content, an edit, a grep pattern, a shell
// command, a search query) may legitimately mention any of these strings.

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace agentty::tools::util {

// Replace `args` with the wrapped call's parameters when `args` is a
// single-call wrapper addressed to `tool_name` (`functions.` prefix and case
// ignored). Returns true when it rewrote `args`.
bool unwrap_call_wrapper(std::string_view tool_name, nlohmann::json& args);

// Why `args` (already unwrapped where possible) carries a leaked wrapper,
// phrased for the model; nullopt for ordinary arguments.
[[nodiscard]] std::optional<std::string>
call_wrapper_leak(std::string_view tool_name, const nlohmann::json& args);

// A sentence to append to an "arguments failed to parse" error when the raw
// text is a leaked wrapper; empty otherwise.
[[nodiscard]] std::string raw_call_wrapper_hint(std::string_view raw);

} // namespace agentty::tools::util
