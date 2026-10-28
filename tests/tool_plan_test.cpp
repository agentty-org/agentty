// tool_plan_test — #98: which tools ride a request, and tool_search.

#include "agtest.hpp"

#include <string>
#include <unordered_set>
#include <vector>

#include "agentty/tool/tool_plan.hpp"

using namespace agentty;
namespace tp = agentty::tools::plan;
using agentty::tools::ToolDef;
using agentty::tools::ToolOrigin;

namespace {

ToolDef native(std::string name) {
    ToolDef t;
    t.name.value = std::move(name);
    t.description = "native tool";
    t.origin = ToolOrigin::Native;
    t.input_schema = {{"type", "object"}, {"properties", nlohmann::json::object()}};
    return t;
}

ToolDef ext(std::string server, std::string bare, std::string desc,
            nlohmann::json props = nlohmann::json::object()) {
    ToolDef t;
    t.name.value = "mcp__" + server + "__" + bare;
    t.description = std::move(desc);
    t.origin = ToolOrigin::Mcp;
    t.origin_id = std::move(server);
    t.input_schema = {{"type", "object"}, {"properties", std::move(props)}};
    return t;
}

// 45 natives + a 75-tool server, the reporter's shape.
std::vector<ToolDef> reporter_catalog() {
    std::vector<ToolDef> c;
    for (int i = 0; i < 45; ++i) c.push_back(native("native_" + std::to_string(i)));
    for (int i = 0; i < 75; ++i)
        c.push_back(ext("binary_ninja", "op_" + std::to_string(i),
                        "Binary Ninja operation number " + std::to_string(i)));
    return c;
}

ToolDef search_tool() {
    ToolDef t;
    t.name.value = std::string{tp::kSearchToolName};
    t.description = "search tools";
    t.origin = ToolOrigin::Mcp;
    t.origin_id = "catalog";
    return t;
}

std::size_t count_mcp(const tp::Plan& p) {
    std::size_t n = 0;
    for (const auto& w : p.wire)
        if (w.tool->origin == ToolOrigin::Mcp && !w.deferred
            && w.tool->name.value != tp::kSearchToolName) ++n;
    return n;
}

} // namespace

TEST_CASE("tool plan: All loads the reporter's 120 tools on a 128 cap (#98)") {
    const auto cat = reporter_catalog();
    const auto s = search_tool();
    tp::Limits lim{.max_tools = 128, .context_window = 110'592};
    const auto p = tp::make_plan(cat, {}, &s, tp::Loading::All, lim);
    CHECK(count_mcp(p) == 75);
    CHECK(p.on_demand == 0);
    CHECK(!p.search);              // nothing to find, no search tool
    CHECK(!p.capped_by_provider);
}

TEST_CASE("tool plan: past the hard cap, All says so and keeps search") {
    auto cat = reporter_catalog();
    for (int i = 75; i < 150; ++i)
        cat.push_back(ext("binary_ninja", "op_" + std::to_string(i), "more"));
    const auto s = search_tool();
    tp::Limits lim{.max_tools = 128, .context_window = 1'000'000};
    const auto p = tp::make_plan(cat, {}, &s, tp::Loading::All, lim);
    CHECK(p.wire.size() <= 128);   // never over the provider's cap
    CHECK(p.capped_by_provider);
    CHECK(p.search);               // the rest stay reachable
    CHECK(p.on_demand == 150 - (p.loaded - 45));
}

TEST_CASE("tool plan: Auto loads what fits the schema budget, the rest on demand") {
    std::vector<ToolDef> cat{native("read")};
    // Big schemas: each ~3k tokens, so a 10% share of 32k fits one.
    nlohmann::json fat = nlohmann::json::object();
    for (int i = 0; i < 300; ++i)
        fat["arg_" + std::to_string(i)] = {{"type", "string"}, {"description", "x"}};
    for (int i = 0; i < 5; ++i) cat.push_back(ext("srv", "t" + std::to_string(i), "d", fat));
    const auto s = search_tool();
    tp::Limits lim{.max_tools = 0, .context_window = 32'000};
    const auto p = tp::make_plan(cat, {}, &s, tp::Loading::Auto, lim);
    CHECK(p.on_demand > 0);
    CHECK(p.search);
    // Natives always ride; the 10% share bounds only the plugin schemas.
    std::size_t plugin = 0;
    for (const auto& w : p.wire)
        if (w.tool->origin == ToolOrigin::Mcp && w.tool != &s) plugin += tp::schema_tokens(*w.tool);
    CHECK(plugin <= 32'000 / 10);
}

TEST_CASE("tool plan: Auto loads a small plugin set even when natives are big") {
    // The bug this guards: charging the natives to the budget left no room
    // for any plugin, so every MCP tool went on demand on a 128k window.
    std::vector<ToolDef> cat;
    nlohmann::json big = nlohmann::json::object();
    for (int i = 0; i < 60; ++i) big["a" + std::to_string(i)] = {{"type", "string"}, {"description", "xxxxxxxxxx"}};
    for (int i = 0; i < 45; ++i) { auto n = native("n" + std::to_string(i)); n.input_schema["properties"] = big; cat.push_back(n); }
    for (int i = 0; i < 20; ++i) cat.push_back(ext("srv", "t" + std::to_string(i), "small tool"));
    const auto s = search_tool();
    const auto p = tp::make_plan(cat, {}, &s, tp::Loading::Auto, {.context_window = 128'000});
    CHECK(p.on_demand == 0);
    CHECK(!p.search);
}

TEST_CASE("tool plan: a small setup loads everything and adds no search tool") {
    std::vector<ToolDef> cat{native("read"), ext("gh", "list_prs", "List PRs")};
    const auto s = search_tool();
    const auto p = tp::make_plan(cat, {}, &s, tp::Loading::Auto, {});
    CHECK(p.on_demand == 0);
    CHECK(!p.search);
    CHECK(p.wire.size() == 2);
}

TEST_CASE("tool plan: thread-loaded and pinned tools always ride") {
    auto cat = reporter_catalog();
    cat[50].always_expose = true;                 // pinned
    const std::string used = cat[100].name.value; // found earlier in the thread
    const auto s = search_tool();
    const auto p = tp::make_plan(cat, {used}, &s, tp::Loading::OnDemand, {});
    bool pinned = false, sticky = false;
    for (const auto& w : p.wire) {
        if (w.tool == &cat[50]) pinned = true;
        if (w.tool->name.value == used) sticky = true;
    }
    CHECK(pinned);
    CHECK(sticky);
    CHECK(p.loaded == 45 + 2);
    CHECK(p.search);
}

TEST_CASE("tool plan: a long thread's loaded tools never push past the hard cap") {
    // 45 natives + 300 MCP tools, and the thread has used 200 of them.
    auto cat = reporter_catalog();
    for (int i = 75; i < 300; ++i)
        cat.push_back(ext("binary_ninja", "op_" + std::to_string(i), "more"));
    tp::Loaded used;
    for (std::size_t i = 45; i < 245; ++i) used.touch(cat[i].name.value);
    const auto s = search_tool();
    tp::Limits lim{.max_tools = 128, .context_window = 1'000'000};
    for (auto mode : {tp::Loading::Auto, tp::Loading::All, tp::Loading::OnDemand}) {
        const auto p = tp::make_plan(cat, used, &s, mode, lim);
        CHECK(p.wire.size() <= 128);
        CHECK(p.search);   // what got dropped stays findable
        // The most recently used tools are the ones kept.
        bool newest = false;
        for (const auto& w : p.wire) newest |= w.tool == &cat[244];
        CHECK(newest);
    }
}

TEST_CASE("tool plan: Anthropic defers instead of dropping, and keeps the cache order") {
    const auto cat = reporter_catalog();
    const auto s = search_tool();
    tp::Limits lim{.discovery = tp::Discovery::DeferredReferences};
    const auto p = tp::make_plan(cat, {}, &s, tp::Loading::OnDemand, lim);
    // Every tool is on the wire (the API needs them to expand references)...
    CHECK(p.wire.size() == cat.size() + 1);
    std::size_t deferred = 0;
    for (const auto& w : p.wire) deferred += w.deferred ? 1 : 0;
    CHECK(deferred == 75);
    // ...in catalog order, with the search tool last and never deferred.
    for (std::size_t i = 0; i < cat.size(); ++i) CHECK(p.wire[i].tool == &cat[i]);
    CHECK(p.wire.back().tool == &s);
    CHECK(!p.wire.back().deferred);
}

TEST_CASE("tool plan: the wire is byte-stable across turns while the set is") {
    // Score order used to reorder the tools block on every user message,
    // busting the prompt cache. The plan depends only on catalog + loaded set.
    const auto cat = reporter_catalog();
    const auto s = search_tool();
    tp::Limits lim{.max_tools = 128, .context_window = 64'000};
    const auto a = tp::make_plan(cat, {}, &s, tp::Loading::Auto, lim);
    const auto b = tp::make_plan(cat, {}, &s, tp::Loading::Auto, lim);
    REQUIRE(a.wire.size() == b.wire.size());
    for (std::size_t i = 0; i < a.wire.size(); ++i) CHECK(a.wire[i].tool == b.wire[i].tool);
}

TEST_CASE("tool plan: loading a tool only appends, so the prefix keeps its cache") {
    const auto cat = reporter_catalog();
    const auto s = search_tool();
    const auto before = tp::make_plan(cat, {}, &s, tp::Loading::OnDemand, {});
    const auto after = tp::make_plan(cat, {cat[119].name.value}, &s, tp::Loading::OnDemand, {});
    // The newly loaded tool is the last MCP tool, so everything before it is
    // the same bytes in the same order.
    REQUIRE(after.wire.size() == before.wire.size() + 1);
    for (std::size_t i = 0; i + 1 < before.wire.size(); ++i)
        CHECK(before.wire[i].tool == after.wire[i].tool);
}

TEST_CASE("tool search: identifiers, stems, typos, other word orders") {
    std::vector<ToolDef> cat{
        native("read"),
        ext("binary_ninja", "decompile_function", "Decompile a function to HLIL pseudo-C.",
            {{"address", {{"type", "string"}}}}),
        ext("binary_ninja", "get_xrefs", "List cross references to an address."),
        ext("binary_ninja", "renameVariable", "Rename a local variable in a function."),
        ext("github", "list_pull_requests", "List open pull requests in a repository."),
        ext("slack", "post_message", "Post a message to a channel."),
    };
    auto top = [&](std::string_view q) {
        const auto h = tp::search(cat, q, 3);
        return h.empty() ? std::string{} : h.front().tool->name.value;
    };
    CHECK(top("decompile function") == "mcp__binary_ninja__decompile_function");
    CHECK(top("decompiling functions") == "mcp__binary_ninja__decompile_function");   // stems
    CHECK(top("decomple functon") == "mcp__binary_ninja__decompile_function");        // typos
    CHECK(top("rename variable") == "mcp__binary_ninja__renameVariable");             // camelCase
    CHECK(top("cross references") == "mcp__binary_ninja__get_xrefs");
    CHECK(top("pull requests") == "mcp__github__list_pull_requests");
    CHECK(top("slack") == "mcp__slack__post_message");                                // server name
    CHECK(top("address") == "mcp__binary_ninja__decompile_function");                 // argument name
    // Natives are always loaded, so search never returns one.
    for (const auto& h : tp::search(cat, "read", 10))
        CHECK(h.tool->origin != ToolOrigin::Native);
}

TEST_CASE("tool search: select: picks exact names, bare or full") {
    std::vector<ToolDef> cat{
        ext("a", "one", "x"), ext("a", "two", "x"), ext("b", "three", "x")};
    const auto h = tp::search(cat, "select:two, mcp__b__three", 5);
    REQUIRE(h.size() == 2);
    CHECK(h[0].tool->name.value == "mcp__a__two");
    CHECK(h[1].tool->name.value == "mcp__b__three");
}

TEST_CASE("tool plan: loaded_in reads calls and tool_search hits from the thread") {
    Thread t;
    Message m;
    m.role = Role::Assistant;
    ToolUse call;
    call.name.value = "mcp__a__used";
    m.tool_calls.push_back(call);
    ToolUse found;
    found.name.value = std::string{tp::kSearchToolName};
    found.status = ToolUse::Done{{}, {}, std::string{"2 tools loaded\n"} +
        std::string{tp::kLoadedMarker} + "mcp__a__found_one — does X\n" +
        std::string{tp::kLoadedMarker} + "mcp__b__found_two\n"};
    m.tool_calls.push_back(found);
    t.messages.push_back(m);

    const auto got = tp::loaded_in(t);
    CHECK(got.contains("mcp__a__used"));
    CHECK(got.contains("mcp__a__found_one"));
    CHECK(got.contains("mcp__b__found_two"));
    CHECK(!got.contains(std::string{tp::kSearchToolName}));
}

TEST_CASE("tool plan: loading setting round-trips, unknown means auto") {
    for (auto l : {tp::Loading::Auto, tp::Loading::All, tp::Loading::OnDemand})
        CHECK(tp::loading_from(tp::to_string(l)) == l);
    CHECK(tp::loading_from("nonsense") == tp::Loading::Auto);
}
