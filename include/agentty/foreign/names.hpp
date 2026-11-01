#pragma once
// agentty::foreign — names for the tools whose sessions agentty can open.
// Pure: safe for views and reducers.

#include <optional>
#include <string>
#include <string_view>

namespace agentty::foreign {

enum class Source : std::uint8_t { Codex, ClaudeCode };

[[nodiscard]] constexpr std::string_view key_of(Source s) noexcept {
    return s == Source::Codex ? "codex" : "claude";
}
[[nodiscard]] constexpr std::string_view name_of(Source s) noexcept {
    return s == Source::Codex ? "Codex" : "Claude Code";
}
[[nodiscard]] constexpr std::optional<Source> parse_source(std::string_view k) noexcept {
    if (k == "codex") return Source::Codex;
    if (k == "claude") return Source::ClaudeCode;
    return std::nullopt;
}
// Display name for a stored key, or the key itself when unknown.
[[nodiscard]] inline std::string display_name(std::string_view k) {
    if (auto s = parse_source(k)) return std::string{name_of(*s)};
    return std::string{k};
}

}  // namespace agentty::foreign
