#pragma once
#include <maya/widget/stream_note.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

// What the stream is going through right now, as one line beside the phase
// chip — or text-empty when the stream is simply working.
[[nodiscard]] maya::StreamNote::Config stream_note_config(const Model& m);

} // namespace agentty::ui
