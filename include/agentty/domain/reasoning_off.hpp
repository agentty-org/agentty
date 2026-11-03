#pragma once
#include <cstdint>

namespace agentty {

// How a model's reasoning is switched OFF. Leaving the effort field out is
// not off for a model that thinks by default (Qwen3, DeepSeek V3.1+, GLM 4.5+,
// gpt-5.1+): it thinks at its default level. So off has to be sent, and how
// depends on the model.
enum class ReasoningOff : std::uint8_t {
    Unknown = 0,   // no declaration
    Omit,          // not a reasoning model, or reasons only when asked
    EffortNone,    // reasoning_effort: "none" (gpt-5.1+, Mistral, many hosts)
    Toggle,        // an on/off switch (chat template enable_thinking, etc.)
    Impossible,    // always reasons (gpt-oss, o-series, R1): lowest tier is the floor
    TemplateOnly,  // learned: this host 400'd reasoning_effort "none"; use the template switch
};

} // namespace agentty
