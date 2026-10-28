// schema_safety_test — one bad MCP tool must not break every request.
//
// The rules are the ones measured on the live endpoints (Anthropic and an
// OpenAI-validating server): what each rejects, and what each accepts.

#include "agtest.hpp"

#include <string>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "agentty/tool/schema.hpp"

namespace sc = agentty::tools::schema;
using nlohmann::json;

TEST_CASE("schema check: what the live API rejects is caught") {
    // Each of these 400'd a real Anthropic request.
    CHECK(!sc::check(json::parse(R"({"type":"object","properties":{"x":{"type":5}}})")).empty());
    CHECK(!sc::check(json::parse(R"({"type":"object","properties":{"x":{"type":"strng"}}})")).empty());
    CHECK(!sc::check(json::parse(R"({"type":"object","properties":[{"x":1}]})")).empty());
    CHECK(!sc::check(json::parse(R"({"type":"object","properties":{"x":{"enum":"a"}}})")).empty());
    CHECK(!sc::check(json::parse(R"({"type":"object","properties":{"x":{"type":"string","minLength":-1}}})")).empty());
    CHECK(!sc::check(json::parse(R"({"type":"object","properties":{"x":{"type":"array","items":5}}})")).empty());
    CHECK(!sc::check(json::parse(R"({"type":"object","properties":{"x":{"type":"string","description":5}}})")).empty());
    // Top-level property name outside [A-Za-z0-9_.-]{1,64}.
    const auto why = sc::check(json::parse(R"({"type":"object","properties":{"file path":{"type":"string"}}})"));
    CHECK(why.find("file path") != std::string::npos);
}

TEST_CASE("schema check: what the live API accepts is not flagged") {
    // Each of these passed a real Anthropic request; flagging them would
    // hide a working tool.
    for (const char* ok : {
             R"({"type":"object","properties":{"x":{"type":"string"}},"required":"x"})",
             R"({"type":"object","properties":{"x":{"type":"string"}},"required":["y"]})",
             R"({"type":"object","properties":{"x":{"type":"string","enum":[]}}})",
             R"({"type":"object","properties":{"x":{"$ref":"#/$defs/Nope"}}})",
             R"({"$schema":"http://json-schema.org/draft-07/schema#","type":"object","properties":{}})",
             R"({"type":"object","properties":{"x":{"type":"string","format":"bogus"}}})",
             R"({"type":"object","properties":{"x":{"type":"string","foo":1}}})",
             R"({"type":"object","properties":{"o":{"type":"object","properties":{"a b":{"type":"string"}}}}})",
             R"({"type":"object","properties":{"x":{"type":"string","pattern":"(["}}})",
             R"({"type":"object","properties":{"v":{"type":["string","null"]}}})",
             R"({"anyOf":[{"type":"object","properties":{"a":{"type":"string"}}}]})",
         })
        CHECK_MESSAGE(sc::check(json::parse(ok)).empty(), ok);
}

TEST_CASE("schema normalize: root union becomes one object with a note") {
    const auto n = sc::normalize(json::parse(R"({"oneOf":[
        {"type":"object","properties":{"path":{"type":"string"}},"required":["path"]},
        {"type":"object","properties":{"url":{"type":"string"}},"required":["url"]}]})"));
    CHECK(n.schema["type"] == "object");
    CHECK(!n.schema.contains("oneOf"));
    CHECK(n.schema["properties"].contains("path"));
    CHECK(n.schema["properties"].contains("url"));
    CHECK(!n.schema.contains("required"));   // either group, so neither is required
    CHECK(n.note.find("(path)") != std::string::npos);
    CHECK(n.note.find("(url)") != std::string::npos);

    const auto a = sc::normalize(json::parse(R"({"allOf":[
        {"type":"object","properties":{"a":{"type":"string"}},"required":["a"]},
        {"type":"object","properties":{"b":{"type":"string"}},"required":["b"]}]})"));
    CHECK(a.schema["required"] == json::array({"a", "b"}));
    CHECK(a.note.empty());
}

TEST_CASE("schema normalize: $ref inlined, loops cut, type arrays collapsed") {
    const auto n = sc::normalize(json::parse(R"({"type":"object",
        "$defs":{"P":{"type":"object","properties":{"x":{"type":"integer"}}},
                 "Loop":{"type":"object","properties":{"next":{"$ref":"#/$defs/Loop"}}}},
        "properties":{"p":{"$ref":"#/$defs/P"},"l":{"$ref":"#/$defs/Loop"},
                      "v":{"type":["string","null"]}}})"));
    const auto s = n.schema.dump();
    CHECK(s.find("$ref") == std::string::npos);
    CHECK(s.find("$defs") == std::string::npos);
    CHECK(n.schema["properties"]["p"]["properties"]["x"]["type"] == "integer");
    CHECK(n.schema["properties"]["v"]["type"] == "string");
    CHECK(n.schema["properties"]["v"]["nullable"] == true);
    // The loop is cut, not expanded forever.
    CHECK(n.schema["properties"]["l"]["properties"]["next"].is_object());
}

TEST_CASE("schema normalize: old-draft spellings are modernized") {
    const auto n = sc::normalize(json::parse(R"({"type":"object",
        "properties":{"a":{"type":"number","minimum":0,"exclusiveMinimum":true},
                      "t":{"type":"array","items":[{"type":"string"},{"type":"number"}]}},
        "required":["a","ghost"]})"));
    CHECK(n.schema["required"] == json::array({"a"}));          // undeclared name dropped
    CHECK(n.schema["properties"]["a"]["exclusiveMinimum"] == 0); // boolean -> number
    CHECK(!n.schema["properties"]["a"].contains("minimum"));
    CHECK(n.schema["properties"]["t"].contains("prefixItems"));
    CHECK(!n.schema["properties"]["t"].contains("items"));
    // A string `required` (draft-3 style) becomes a list.
    const auto s = sc::normalize(json::parse(R"({"type":"object","properties":{"x":{}},"required":"x"})"));
    CHECK(s.schema["required"] == json::array({"x"}));
    // And everything normalize produces passes check.
    CHECK(sc::check(n.schema).empty());
    CHECK(sc::check(s.schema).empty());
}

TEST_CASE("schema normalize: oversized schemas are compacted") {
    json props = json::object();
    for (int i = 0; i < 80; ++i)
        props["f" + std::to_string(i)] = {{"type", "object"}, {"description", std::string(200, 'd')},
            {"properties", {{"inner", {{"type", "string"}, {"description", std::string(200, 'e')}}}}}};
    const auto n = sc::normalize({{"type", "object"}, {"properties", props}});
    CHECK(n.schema.dump().size() < json({{"type", "object"}, {"properties", props}}).dump().size() / 2);
    CHECK(n.schema["properties"].size() == 80);   // every parameter survives
}

TEST_CASE("wire_name: fits every provider, unique, stable") {
    std::unordered_set<std::string> taken;
    CHECK(sc::wire_name("gh", "list_prs", taken) == "mcp__gh__list_prs");
    CHECK(sc::wire_name("my.srv", "do it", taken) == "mcp__my_srv__do_it");

    const std::string longname = "a_really_long_tool_name_that_goes_on_and_on_for_quite_a_while_yes";
    const auto a = sc::wire_name("binary_ninja", longname, taken);
    CHECK(a.size() <= 64);
    CHECK(a.starts_with("mcp__binary_ninja__"));
    // Same raw name, fresh run: same wire name (the hash is of the name).
    std::unordered_set<std::string> fresh;
    CHECK(sc::wire_name("binary_ninja", longname, fresh) == a);
    // Two long names sharing a long prefix don't collide.
    const auto b = sc::wire_name("binary_ninja", longname + "_two", taken);
    CHECK(b != a);
    CHECK(b.size() <= 64);
    // Two raw names that sanitise to the same thing don't collide either.
    const auto c1 = sc::wire_name("s", "a.b", taken);
    const auto c2 = sc::wire_name("s", "a-b_", taken);   // distinct, sanitises differently
    const auto c3 = sc::wire_name("s", "a_b", taken);    // same as c1 sanitised
    CHECK(c1 != c3);
    CHECK(c3.size() <= 64);
    (void)c2;
    for (const auto& n : taken)
        for (char ch : n) CHECK((std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-'));
}
