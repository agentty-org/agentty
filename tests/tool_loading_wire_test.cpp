// tool_loading_wire_test — #98 end to end through the real encoders.
//
// The planner decides; these check the bytes each provider actually gets:
//   Anthropic  every tool on the wire, on-demand ones `defer_loading`, the
//              cache pin on a non-deferred tool, and a tool_search result
//              turned into tool_reference blocks.
//   OpenAI     on-demand tools absent, never more than 128, and a deferred
//              flag can never leak into the body.

#include "agtest.hpp"

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agentty/provider/anthropic/transport.hpp"
#include "agentty/provider/msg_shared.hpp"
#include "agentty/tool/tool_loading.hpp"
#include "agentty/tool/tool_plan.hpp"

using namespace agentty;
namespace tl = agentty::tools::tool_loading;
namespace tp = agentty::tools::plan;
using agentty::tools::ToolDef;
using agentty::tools::ToolOrigin;

namespace {

std::vector<ToolDef> catalog(int mcp_tools) {
    std::vector<ToolDef> c;
    for (const char* n : {"read", "edit", "shell"}) {
        ToolDef t;
        t.name.value = n;
        t.origin = ToolOrigin::Native;
        t.input_schema = {{"type", "object"}};
        c.push_back(t);
    }
    for (int i = 0; i < mcp_tools; ++i) {
        ToolDef t;
        t.name.value = "mcp__srv__tool_" + std::to_string(i);
        t.description = "tool " + std::to_string(i);
        t.origin = ToolOrigin::Mcp;
        t.origin_id = "srv";
        t.input_schema = {{"type", "object"}};
        c.push_back(t);
    }
    return c;
}

std::vector<provider::ToolSpec> plan_for(const std::vector<ToolDef>& cat, provider::Kind kind,
                                         tp::Loading mode, tp::Loaded loaded = {}) {
    provider::Selection sel;
    sel.kind = kind;
    tl::Request r;
    for (const auto& t : cat) r.pool.push_back(&t);
    r.loaded = std::move(loaded);
    r.limits = tl::limits_for(sel, 200'000);
    r.mode = mode;
    std::vector<provider::ToolSpec> out;
    (void)tl::plan_into(r, out);
    return out;
}

} // namespace

TEST_CASE("tool loading wire: Anthropic defers, OpenAI omits (#98)") {
    const auto cat = catalog(200);
    const auto a = plan_for(cat, provider::Kind::Anthropic, tp::Loading::OnDemand);
    const auto o = plan_for(cat, provider::Kind::OpenAI, tp::Loading::OnDemand);

    std::size_t a_deferred = 0;
    for (const auto& s : a) a_deferred += s.defer_loading ? 1 : 0;
    CHECK(a.size() == cat.size() + 1);           // every tool + tool_search
    CHECK(a_deferred == 200);
    CHECK(a.back().name == "tool_search");
    CHECK(!a.back().defer_loading);

    for (const auto& s : o) CHECK(!s.defer_loading);
    CHECK(o.size() == 3 + 1);                    // natives + tool_search
    CHECK(o.back().name == "tool_search");
    // The search tool names what it can find.
    CHECK(o.back().description.find("srv (200 tools)") != std::string::npos);
}

TEST_CASE("tool loading wire: OpenAI never gets more than 128 tools") {
    const auto cat = catalog(500);
    const auto o = plan_for(cat, provider::Kind::OpenAI, tp::Loading::All);
    CHECK(o.size() <= 128);
    CHECK(o.back().name == "tool_search");       // the rest stay reachable
    // And the chat encoder drops anything deferred, as a backstop.
    std::vector<provider::ToolSpec> mixed = o;
    mixed.push_back({"leak", "x", {{"type", "object"}}, false, /*defer_loading=*/true});
    CHECK(provider::wire::openai_chat_tools(mixed).size() == o.size());
}

TEST_CASE("tool loading wire: a tool_search hit on Anthropic is a tool_reference") {
    Thread t;
    Message u;
    u.role = Role::User;
    u.text = "decompile main";
    t.messages.push_back(u);
    Message a;
    a.role = Role::Assistant;
    ToolUse call;
    call.id.value = "toolu_1";
    call.name.value = "tool_search";
    call.args = {{"query", "decompile"}};
    call.status = ToolUse::Done{{}, {}, std::string{"1 tool loaded for this conversation.\n"}
        + std::string{tp::kLoadedMarker} + "mcp__bn__decompile_function - Decompile\n"
        + "  schema: {\"type\":\"object\"}\n"};
    a.tool_calls.push_back(call);
    t.messages.push_back(a);

    const auto body = nlohmann::json::parse(provider::anthropic::messages_json_string(t));
    // messages[2] is the user turn carrying the tool_result.
    const auto& content = body.at(2).at("content");
    REQUIRE(content.is_array());
    const auto& result = content.at(0);
    CHECK(result.at("type") == "tool_result");
    bool ref = false, other = false;
    for (const auto& b : result.at("content")) {
        if (b.at("type") == "tool_reference" && b.at("tool_name") == "mcp__bn__decompile_function")
            ref = true;
        else
            other = true;
    }
    CHECK(ref);
    // The live API 400s a tool_result mixing references with other blocks.
    CHECK(!other);
    // And the thread now counts it as loaded.
    CHECK(tp::loaded_in(t).contains("mcp__bn__decompile_function"));
}

TEST_CASE("tool loading wire: a loaded tool stays on the OpenAI wire next turn") {
    const auto cat = catalog(200);
    const auto before = plan_for(cat, provider::Kind::OpenAI, tp::Loading::OnDemand);
    const auto after = plan_for(cat, provider::Kind::OpenAI, tp::Loading::OnDemand,
                                {"mcp__srv__tool_150"});
    bool found = false;
    for (const auto& s : after) found |= s.name == "mcp__srv__tool_150";
    CHECK(found);
    CHECK(after.size() == before.size() + 1);
}
