// Leaked parallel-call wrappers in tool arguments (tool/util/call_leak.hpp).
//
// THE BUG THIS PINS: an OpenAI-family subagent told to "batch independent
// reads into ONE turn" began writing its internal multi-call wrapper
//   {"recipient_name":"functions.read","parameters":{...}}
// inside one read's arguments. A host that constrains decoding to the read
// schema turned that into valid, schema-shaped garbage:
//   {"path":"cmd/server/main.go}},{","symbol":"recipient_name","offset":1}
// agentty ran it, read reported "not found", and the model learned nothing.

#include "agtest.hpp"

#include "agentty/runtime/app/update/stream_args.hpp"
#include "agentty/tool/util/call_leak.hpp"

#include <string>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace u = agentty::tools::util;

namespace {
bool has(const std::optional<std::string>& s, std::string_view needle) {
    return s && s->find(needle) != std::string::npos;
}
} // namespace

TEST_CASE("call leak: the screenshot shapes are refused with a useful reason") {
    // Exactly what the subagent cards showed.
    for (const char* tail : {"}},{", "'}},{", "\"}},{", "}}]}"}) {
        json args = {{"path", std::string{"cmd/server/main.go"} + tail},
                     {"symbol", "recipient_name"}, {"offset", 1}};
        auto leak = u::call_wrapper_leak("read", args);
        CHECK_MESSAGE(leak.has_value(), tail);
        CHECK(has(leak, "own function call"));
        CHECK(has(leak, "`path`"));
        CHECK(has(leak, "`symbol`"));   // the weak token joins the report
    }
    // file_path spelling, and the wrapper text kept verbatim in the value.
    json a2 = {{"file_path",
                "internal/store/store.go}},{\"recipient_name\":\"functions.read\"}]}"}};
    CHECK(u::call_wrapper_leak("read", a2).has_value());
    // Same debris in another tool's location field.
    json a3 = {{"path", "src}},{"}, {"pattern", "TODO"}};
    CHECK(u::call_wrapper_leak("grep", a3).has_value());
    json a4 = {{"paths", json::array({"a.go", "b.go}},{"})}};
    CHECK(u::call_wrapper_leak("read", a4).has_value());
}

TEST_CASE("call leak: ordinary arguments pass") {
    const json ok1 = {{"path", "src/main.go"}};
    const json ok2 = {{"path", "a/b{c}.txt"}};
    const json ok3 = {{"path", "x.go"}, {"symbol", "operator()"}};
    // A symbol that merely shares a wrapper word is not evidence on its own.
    const json ok4 = {{"path", "x.py"}, {"symbol", "parameters"}};
    const json ok5 = {{"symbol", "recipient_name"}};
    // Free text may contain anything: an edit of THIS test, a grep for the
    // wrapper, a shell command that prints JSON.
    const json ok6 = {{"path", "t.json"},
                      {"content", "{\"recipient_name\":\"functions.read\"}}},{"}};
    const json ok7 = {{"pattern", "recipient_name\"}},{"}, {"path", "src"}};
    const json ok8 = {{"command", "echo '{\"a\":{\"b\":1}},{\"c\":2}'"}};
    CHECK_FALSE(u::call_wrapper_leak("read", ok1).has_value());
    CHECK_FALSE(u::call_wrapper_leak("read", ok2).has_value());
    CHECK_FALSE(u::call_wrapper_leak("read", ok3).has_value());
    CHECK_FALSE(u::call_wrapper_leak("read", ok4).has_value());
    CHECK_FALSE(u::call_wrapper_leak("find_definition", ok5).has_value());
    CHECK_FALSE(u::call_wrapper_leak("write", ok6).has_value());
    CHECK_FALSE(u::call_wrapper_leak("grep", ok7).has_value());
    CHECK_FALSE(u::call_wrapper_leak("shell", ok8).has_value());
    CHECK_FALSE(u::call_wrapper_leak("read", json::object()).has_value());
    CHECK_FALSE(u::call_wrapper_leak("read", json()).has_value());
}

TEST_CASE("call leak: a single wrapped call to this tool is unwrapped") {
    json a = {{"recipient_name", "functions.read"},
              {"parameters", {{"path", "web/src/App.jsx"}, {"offset", 3}}}};
    CHECK(u::unwrap_call_wrapper("read", a));
    const json want_a = {{"path", "web/src/App.jsx"}, {"offset", 3}};
    CHECK(a == want_a);
    CHECK_FALSE(u::call_wrapper_leak("read", a).has_value());

    json b = {{"tool_uses", json::array({{{"recipient_name", "read"},
                                          {"parameters", {{"path", "a.go"}}}}})}};
    CHECK(u::unwrap_call_wrapper("read", b));
    const json want_b = {{"path", "a.go"}};
    CHECK(b == want_b);

    // parameters sent as a JSON string
    json c = {{"recipient_name", "Functions.Grep"}, {"parameters", "{\"pattern\":\"x\"}"}};
    CHECK(u::unwrap_call_wrapper("grep", c));
    const json want_c = {{"pattern", "x"}};
    CHECK(c == want_c);

    // canonify (every parse site + the dispatch bridge) does it too.
    json d = {{"recipient_name", "functions.shell"}, {"parameters", {{"cmd", "ls"}}}};
    CHECK(agentty::app::detail::canonify_tool_args("shell", d));
    CHECK(d.value("command", "") == "ls");
}

TEST_CASE("call leak: wrappers that can't be unwrapped are named, not run") {
    json many = {{"tool_uses", json::array({
        {{"recipient_name", "functions.read"}, {"parameters", {{"path", "a.go"}}}},
        {{"recipient_name", "functions.read"}, {"parameters", {{"path", "b.go"}}}},
    })}};
    CHECK_FALSE(u::unwrap_call_wrapper("read", many));
    auto l1 = u::call_wrapper_leak("read", many);
    CHECK(has(l1, "2 tool calls"));
    CHECK(has(l1, "read(path=a.go)"));
    CHECK(has(l1, "read(path=b.go)"));

    json other = {{"recipient_name", "functions.grep"}, {"parameters", {{"pattern", "x"}}}};
    CHECK_FALSE(u::unwrap_call_wrapper("read", other));
    CHECK(has(u::call_wrapper_leak("read", other), "wrap a call to `grep`"));

    json mixed = {{"recipient_name", "functions.read"}, {"parameters", {{"path", "a"}}},
                  {"path", "b"}};
    CHECK_FALSE(u::unwrap_call_wrapper("read", mixed));
    CHECK(u::call_wrapper_leak("read", mixed).has_value());
}

TEST_CASE("call leak: raw-text hint for args that never parsed") {
    CHECK_FALSE(u::raw_call_wrapper_hint(
        R"({"path":"a.go"}},{"recipient_name":"functions.read","parameters":{"path":"b"}}]})")
        .empty());
    CHECK(u::raw_call_wrapper_hint(R"({"path":"a.go")").empty());
}
