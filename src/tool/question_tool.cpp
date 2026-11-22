// agentty::tools::question_tool — see the header.

#include "agentty/tool/question_tool.hpp"

#include <string>

#include "agentty/domain/question.hpp"
#include "agentty/util/snapshot.hpp"

namespace agentty::tools::question_tool {

namespace {
// Same slot type as web_search_policy: a value replaced whole, read lock-free.
util::AtomicSnapshot<bool>& slot() {
    static util::AtomicSnapshot<bool> s;
    return s;
}
}  // namespace

bool enabled() noexcept {
    const auto v = slot().load();
    return v && *v;
}
void install(bool on) noexcept { slot().store(on); }

const ToolDef& def() {
    static const ToolDef d = [] {
        ToolDef t;
        t.name.value = std::string{kName};
        // When to use it is most of the description. A model that asks about
        // everything is worse than one that never asks, so the bar is stated
        // first and the mechanics second.
        t.description =
            "Ask the user one or more multiple-choice questions and wait for the "
            "answers. Use it only when the request is genuinely ambiguous or a "
            "decision is the user's to make and costly to get wrong (which of two "
            "designs, whether to delete data, which of several matching files they "
            "meant). Do not use it for things you can find out yourself by reading "
            "the code, for confirmation of what you are about to do anyway, or for "
            "small choices you can make and mention. Ask everything you need in ONE "
            "call (up to " + std::to_string(question::kMaxQuestions) + " questions). "
            "Give 2-" + std::to_string(question::kMaxOptions) + " short, distinct "
            "options per question, the recommended one first; the user can always "
            "type their own answer instead, so do not add an \"Other\" option. The "
            "result is the user's answers, or a note that they skipped.";
        const nlohmann::json option = {
            {"type", "object"},
            {"properties", {
                {"label", {{"type", "string"},
                           {"description", "The choice, 1-5 words."}}},
                {"description", {{"type", "string"},
                                 {"description", "What picking it means, one short sentence."}}},
            }},
            {"required", {"label"}},
        };
        t.input_schema = {
            {"type", "object"},
            {"properties", {
                {"questions", {
                    {"type", "array"},
                    {"minItems", 1},
                    {"maxItems", question::kMaxQuestions},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"question", {{"type", "string"},
                                          {"description", "The full question, ending in '?'."}}},
                            {"header", {{"type", "string"},
                                        {"description", "A 1-3 word label for it, e.g. \"Database\"."}}},
                            {"options", {{"type", "array"},
                                         {"maxItems", question::kMaxOptions},
                                         {"items", option}}},
                            {"multiple", {{"type", "boolean"},
                                          {"description", "True when more than one option may be chosen."}}},
                        }},
                        {"required", {"question", "options"}},
                    }},
                }},
            }},
            {"required", {"questions"}},
        };
        t.origin = ToolOrigin::Native;
        t.effects = EffectSet{};          // pure: it only asks
        t.scheduling_effects = t.effects;
        t.max_output_chars = 8'000;
        t.output_truncation = OutputTruncation::Head;
        // A person is answering; the TUI never dispatches it, and the paths
        // that do return at once.
        t.timeout = std::chrono::milliseconds{5'000};
        t.execute = [](const maya::unlocked&, const nlohmann::json& args,
                       const CallContext&) -> ExecResult {
            if (auto p = question::parse(args); !p.error.empty())
                return std::unexpected(ToolError::invalid_args(std::move(p.error)));
            ToolOutput o;
            o.text = question::unavailable();
            return o;
        };
        return t;
    }();
    return d;
}

}  // namespace agentty::tools::question_tool
