#pragma once
// agentty::tools::schema — make an MCP tool safe to send to every provider.
//
// One bad tool used to fail EVERY request: providers validate every tool in
// `tools` (deferred ones too) and 400 the whole call. So each MCP tool goes
// through here once, at load:
//
//   check()     what would make a provider reject it. A tool that fails is
//               left out of the request and the reason is shown; the rest of
//               its server keeps working.
//   wire_name() a name every provider accepts: [A-Za-z0-9_-], at most 64,
//               unique, stable across runs.
//   normalize() fix the shapes that are only a matter of spelling: root
//               anyOf/oneOf/allOf flattened, $ref inlined, type arrays and
//               other Gemini-hostile forms rewritten, oversized schemas
//               compacted. What the server receives is unchanged.
//
// Measured on the live endpoints: Anthropic rejects property keys outside
// ^[a-zA-Z0-9_.-]{1,64}$ and schemas that aren't valid JSON Schema 2020-12;
// OpenAI rejects names longer than 64. Claude Code, Codex and OpenCode do the
// same three things.
//
// Pure: no IO.

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace agentty::tools::schema {

// Empty when the schema is fine to send; else a short reason ("property
// 'file path' has characters providers reject").
[[nodiscard]] std::string check(const nlohmann::json& schema);

// What normalize() did, for the tool description: a root union is merged
// into one object, and which parameter groups belong together goes in a
// sentence the model reads. Empty when nothing needs saying.
struct Normalized {
    nlohmann::json schema;
    std::string    note;
};
// Default size budget for one tool's schema; larger ones lose nested
// descriptions first (Codex: 5 KB).
inline constexpr std::size_t kCompactBytes = 5'000;
[[nodiscard]] Normalized normalize(nlohmann::json schema,
                                   std::size_t compact_bytes = kCompactBytes);

// `mcp__<server>__<tool>`, sanitised to [A-Za-z0-9_-] and fit to 64 chars.
// When it doesn't fit, the tool part is shortened and an 8-hex hash of the
// full raw name is appended, so two long names never collide and the same
// tool gets the same name every run. `taken` holds names already handed out;
// a clash gets a different hash.
inline constexpr std::size_t kMaxNameLen = 64;
[[nodiscard]] std::string wire_name(std::string_view server, std::string_view tool,
                                    std::unordered_set<std::string>& taken);

} // namespace agentty::tools::schema
