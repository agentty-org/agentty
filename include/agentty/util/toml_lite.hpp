#pragma once
// agentty::util::toml_lite — a small TOML reader for importing other tools'
// config (Codex's config.toml). Covers tables, dotted keys, quoted keys,
// arrays of tables, inline tables, arrays, all four string forms, integers,
// floats and booleans. Dates come back as strings. Not a validator.

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace agentty::util::toml_lite {

// Parse `text` into a JSON object. On a syntax error returns false and sets
// `error` to "line N: what".
[[nodiscard]] bool parse(std::string_view text, nlohmann::json& out, std::string& error);

}  // namespace agentty::util::toml_lite
