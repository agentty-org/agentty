// tool_body_preview — `web_fetch` tool body.
//
// Json (pretty-printed key/value). Only a DONE web_fetch produces a body;
// otherwise returns false to fall through to the shared Failure fallback.

#include "tool_body_common.hpp"

#include <string>

#include "agentty/runtime/view/palette.hpp"

namespace agentty::ui::detail {

bool web_fetch_body(const ToolUse& tc, maya::ToolBodyPreview::Config& out) {
    using Kind = maya::ToolBodyPreview::Kind;
    if (!tc.is_done()) return false;
    const std::string_view body = card_output(tc);
    if (!body.empty()) {
        // A page reads from the top (title, lead paragraph); its tail is
        // footer chrome. JSON stays as JSON.
        const auto first = body.find_first_not_of(" \n\t");
        const bool json = first != std::string_view::npos
                       && (body[first] == '{' || body[first] == '[');
        out.kind = json ? Kind::Json : Kind::CodeBlock;
        out.text = clean_output(body);
        // Blank lines between paragraphs cost a row each in a 5-row preview.
        if (!json) {
            std::string dense;
            dense.reserve(out.text.size());
            for (std::size_t p = 0; p <= out.text.size();) {
                auto e = out.text.find('\n', p);
                if (e == std::string::npos) e = out.text.size();
                std::string_view ln{out.text.data() + p, e - p};
                if (ln.find_first_not_of(" \t") != std::string_view::npos) {
                    dense.append(ln);
                    dense += '\n';
                }
                if (e == out.text.size()) break;
                p = e + 1;
            }
            if (!dense.empty()) dense.pop_back();
            out.text = std::move(dense);
        }
        out.text_color = text_primary;
        out.tail_only = false;
        out.code_head = 5;
        out.code_tail = 0;
    }
    return true;
}

} // namespace agentty::ui::detail
