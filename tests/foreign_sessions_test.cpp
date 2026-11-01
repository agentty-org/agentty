// foreign_sessions_test.cpp — reading Codex and Claude Code sessions.
//
// Fixtures are trimmed copies of real session lines: the shapes both tools
// actually write, including Codex's code-mode `exec` scripts, `wait` polls
// and compaction, and Claude Code's split assistant lines and rewinds.

#include "agentty/foreign/sessions.hpp"
#include "agentty/provider/anthropic/transport.hpp"
#include "agentty/provider/openai/transport.hpp"
#include "agentty/provider/provider.hpp"
#include "agentty/provider/responses/responses.hpp"

#include "agtest.hpp"

#include <nlohmann/json.hpp>

#include <set>
#include <string>

using namespace agentty;
namespace fg = agentty::foreign;

namespace {

std::string lines(std::initializer_list<const char*> ls) {
    std::string s;
    for (auto l : ls) { s += l; s += '\n'; }
    return s;
}

const Message* first_with_tool(const std::vector<Message>& ms, std::string_view name) {
    for (const auto& m : ms)
        for (const auto& t : m.tool_calls)
            if (t.name.value == name) return &m;
    return nullptr;
}

const ToolUse* find_tool(const std::vector<Message>& ms, std::string_view name) {
    for (const auto& m : ms)
        for (const auto& t : m.tool_calls)
            if (t.name.value == name) return &t;
    return nullptr;
}

}  // namespace

TEST_CASE("codex: messages, code-mode tools, reasoning and usage") {
    const std::string text = lines({
        R"({"timestamp":"2026-07-27T13:57:55.436Z","type":"session_meta","payload":{"id":"019f-a","cwd":"/home/me/proj","originator":"codex-tui","cli_version":"0.145.0","source":"cli"}})",
        R"({"timestamp":"2026-07-27T13:57:55.500Z","type":"turn_context","payload":{"cwd":"/home/me/proj","model":"gpt-5.6-terra"}})",
        R"({"timestamp":"2026-07-27T13:57:55.600Z","type":"response_item","payload":{"type":"message","role":"developer","content":[{"type":"input_text","text":"<permissions instructions>sandbox"}]}})",
        R"({"timestamp":"2026-07-27T13:57:55.700Z","type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"<environment_context>\n  <cwd>/home/me/proj</cwd>"}]}})",
        R"({"timestamp":"2026-07-27T13:57:56.000Z","type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"fix the build"}]}})",
        R"({"timestamp":"2026-07-27T13:57:56.000Z","type":"event_msg","payload":{"type":"user_message","message":"fix the build"}})",
        R"({"timestamp":"2026-07-27T13:57:57.000Z","type":"response_item","payload":{"type":"reasoning","summary":[{"type":"summary_text","text":"**Checking status**"}],"encrypted_content":"gAAAA-secret"}})",
        R"({"timestamp":"2026-07-27T13:57:58.000Z","type":"response_item","payload":{"type":"custom_tool_call","status":"completed","call_id":"call_1","name":"exec","input":"const r = await tools.exec_command({\"cmd\":\"git status --short\",\"workdir\":\"/home/me/proj\",\"yield_time_ms\":10000});\ntext(r.output);"}})",
        R"({"timestamp":"2026-07-27T13:57:59.000Z","type":"response_item","payload":{"type":"custom_tool_call_output","call_id":"call_1","output":[{"type":"input_text","text":"Script completed\nWall time 0.1 seconds\nOutput:\n"},{"type":"input_text","text":" M src/a.cpp\n"}]}})",
        R"({"timestamp":"2026-07-27T13:58:00.000Z","type":"response_item","payload":{"type":"custom_tool_call","status":"completed","call_id":"call_2","name":"exec","input":"const patch = \"*** Begin Patch\\n*** Update File: /home/me/proj/src/a.cpp\\n@@\\n-old\\n+new\\n*** End Patch\";\nconst r = await tools.apply_patch(patch); text(r);"}})",
        R"({"timestamp":"2026-07-27T13:58:01.000Z","type":"response_item","payload":{"type":"custom_tool_call_output","call_id":"call_2","output":"Script completed\nWall time 0.0 seconds\nOutput:\nSuccess. Updated the following files:\nM src/a.cpp"}})",
        R"({"timestamp":"2026-07-27T13:58:01.500Z","type":"event_msg","payload":{"type":"patch_apply_end","call_id":"exec-x","success":true,"changes":{"/home/me/proj/src/a.cpp":{"type":"update","unified_diff":"@@ -1 +1 @@\n-old\n+new\n"}}}})",
        R"({"timestamp":"2026-07-27T13:58:02.000Z","type":"response_item","payload":{"type":"custom_tool_call","status":"completed","call_id":"call_3","name":"exec","input":"const plan = [\n  { step: \"Fix build\", status: \"completed\" },\n  { step: \"Run tests\", status: \"in_progress\" }\n];\nconst r = await tools.update_plan({ explanation: \"Starting.\", plan });\ntext(r);"}})",
        R"({"timestamp":"2026-07-27T13:58:03.000Z","type":"response_item","payload":{"type":"custom_tool_call_output","call_id":"call_3","output":"Script completed\nOutput:\nPlan updated"}})",
        R"({"timestamp":"2026-07-27T13:58:04.000Z","type":"response_item","payload":{"type":"message","role":"assistant","content":[{"type":"output_text","text":"Fixed it."}]}})",
        R"({"timestamp":"2026-07-27T13:58:04.100Z","type":"event_msg","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":14121,"cached_input_tokens":11008,"output_tokens":122,"reasoning_output_tokens":12}}}})",
        R"({"timestamp":"2026-07-27T13:58:04.200Z","type":"event_msg","payload":{"type":"task_complete"}})",
        R"(not json at all)",
        R"({"timestamp":"2026-07-27T13:58:05.000Z","type":"brand_new_kind","payload":{"x":1}})",
    });
    const fg::Parsed p = fg::parse_codex(text);
    CHECK(p.session_id == "019f-a");
    CHECK(p.cwd == "/home/me/proj");
    CHECK(!p.skip);

    REQUIRE(p.messages.size() >= 2);
    CHECK(p.messages[0].role == Role::User);
    CHECK(p.messages[0].text == "fix the build", "injected context is dropped");
    for (const auto& m : p.messages) CHECK(m.text.find("<environment_context>") == std::string::npos);

    const ToolUse* sh = find_tool(p.messages, "shell");
    REQUIRE(sh != nullptr);
    CHECK(sh->args["command"] == "git status --short", "exec_command unwraps to agentty's shell");
    CHECK(sh->args["cd"] == "/home/me/proj");
    CHECK(sh->is_done());
    CHECK(sh->output() == " M src/a.cpp\n", "the script header is stripped");

    const ToolUse* ap = find_tool(p.messages, "apply_patch");
    REQUIRE(ap != nullptr);
    CHECK(ap->args["path"] == "/home/me/proj/src/a.cpp");
    CHECK(ap->args["patch"].get<std::string>().find("+new") != std::string::npos);
    CHECK(ap->output().starts_with("```diff\n"), "the applied diff renders as a diff card");
    CHECK(ap->output().find("+new") != std::string::npos);

    const ToolUse* todo = find_tool(p.messages, "todo");
    REQUIRE(todo != nullptr);
    REQUIRE(todo->args["todos"].size() == 2);
    CHECK(todo->args["todos"][1]["status"] == "in_progress");

    const Message* first_reply = first_with_tool(p.messages, "shell");
    REQUIRE(first_reply != nullptr);
    CHECK(first_reply->thinking == "**Checking status**", "readable reasoning is kept");
    CHECK(first_reply->reasoning_encrypted.empty(), "encrypted reasoning is never carried over");
    CHECK(first_reply->served_model.value == "gpt-5.6-terra");

    const Message& last = p.messages.back();
    CHECK(last.text == "Fixed it.");
    REQUIRE(last.telemetry.has_value());
    CHECK(last.telemetry->cache_read == 11008);
    CHECK(last.telemetry->input_tokens == 14121 - 11008);
}

TEST_CASE("codex: a yielded exec gets its output from the later wait") {
    const std::string text = lines({
        R"({"timestamp":"2026-07-27T13:00:00.000Z","type":"session_meta","payload":{"id":"s2","cwd":"/p","originator":"codex-tui"}})",
        R"({"timestamp":"2026-07-27T13:00:01.000Z","type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"build it"}]}})",
        R"({"timestamp":"2026-07-27T13:00:02.000Z","type":"response_item","payload":{"type":"custom_tool_call","call_id":"c1","name":"exec","input":"await tools.exec_command({\"cmd\":\"make -j12\"});"}})",
        R"({"timestamp":"2026-07-27T13:00:12.000Z","type":"response_item","payload":{"type":"custom_tool_call_output","call_id":"c1","output":"Script running with cell ID 13"}})",
        R"({"timestamp":"2026-07-27T13:00:13.000Z","type":"response_item","payload":{"type":"function_call","name":"wait","arguments":"{\"cell_id\":\"13\"}","call_id":"w1"}})",
        R"({"timestamp":"2026-07-27T13:00:40.000Z","type":"response_item","payload":{"type":"function_call_output","call_id":"w1","output":[{"type":"input_text","text":"Script completed\nOutput:\n"},{"type":"input_text","text":"[100%] Built target app\n"}]}})",
    });
    const fg::Parsed p = fg::parse_codex(text);
    const ToolUse* sh = find_tool(p.messages, "shell");
    REQUIRE(sh != nullptr);
    CHECK(sh->output() == "[100%] Built target app\n", "the wait result replaces 'running'");
    CHECK(find_tool(p.messages, "wait") == nullptr, "wait is plumbing, not a card");
}

TEST_CASE("codex: compaction becomes a record, turns stay on screen") {
    const std::string text = lines({
        R"({"timestamp":"2026-07-27T13:00:00.000Z","type":"session_meta","payload":{"id":"s3","cwd":"/p","originator":"codex-tui"}})",
        R"({"timestamp":"2026-07-27T13:00:01.000Z","type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"one"}]}})",
        R"({"timestamp":"2026-07-27T13:00:02.000Z","type":"response_item","payload":{"type":"message","role":"assistant","content":[{"type":"output_text","text":"first"}]}})",
        R"({"timestamp":"2026-07-27T13:00:03.000Z","type":"compacted","payload":{"message":"User asked one; I answered first.","replacement_history":[]}})",
        R"({"timestamp":"2026-07-27T13:00:04.000Z","type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"two"}]}})",
    });
    const fg::Parsed p = fg::parse_codex(text);
    CHECK(p.messages.size() == 3);
    REQUIRE(p.compactions.size() == 1);
    CHECK(p.compactions[0].up_to_index == 2);
    CHECK(p.compactions[0].summary == "User asked one; I answered first.");
}

TEST_CASE("codex: runs started by other programs are skipped") {
    const std::string text = lines({
        R"({"timestamp":"2026-07-27T13:00:00.000Z","type":"session_meta","payload":{"id":"s4","cwd":"/p","originator":"codex_exec"}})",
        R"({"timestamp":"2026-07-27T13:00:01.000Z","type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"hi"}]}})",
    });
    CHECK(fg::parse_codex(text).skip);
}

TEST_CASE("codex: a call cut off mid-session is marked, not left running") {
    const std::string text = lines({
        R"({"timestamp":"2026-07-27T13:00:00.000Z","type":"session_meta","payload":{"id":"s5","cwd":"/p","originator":"codex-tui"}})",
        R"({"timestamp":"2026-07-27T13:00:01.000Z","type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"go"}]}})",
        R"({"timestamp":"2026-07-27T13:00:02.000Z","type":"response_item","payload":{"type":"function_call","name":"exec_command","arguments":"{\"cmd\":\"sleep 100\"}","call_id":"c9"}})",
    });
    const fg::Parsed p = fg::parse_codex(text);
    const ToolUse* sh = find_tool(p.messages, "shell");
    REQUIRE(sh != nullptr);
    CHECK(sh->is_terminal(), "every imported call is settled so the wire pairs it");
}

TEST_CASE("claude: chain from the newest leaf, tools mapped, sidechains out") {
    const std::string text = lines({
        R"({"type":"queue-operation","operation":"enqueue","sessionId":"cc-1"})",
        R"({"parentUuid":null,"isSidechain":false,"type":"user","message":{"role":"user","content":"read the readme"},"uuid":"u1","timestamp":"2026-08-01T19:13:19.621Z","cwd":"/home/me/proj","sessionId":"cc-1","entrypoint":"cli"})",
        R"({"parentUuid":"u1","isSidechain":false,"type":"assistant","message":{"id":"msg_1","role":"assistant","model":"claude-opus-4-7","content":[{"type":"thinking","thinking":"Let me look.","signature":"sig-secret"}]},"uuid":"a1","timestamp":"2026-08-01T19:13:20.000Z","cwd":"/home/me/proj","sessionId":"cc-1"})",
        R"({"parentUuid":"a1","isSidechain":false,"type":"assistant","message":{"id":"msg_1","role":"assistant","model":"claude-opus-4-7","content":[{"type":"tool_use","id":"toolu_1","name":"Read","input":{"file_path":"/home/me/proj/README.md"}}],"usage":{"input_tokens":10,"output_tokens":20,"cache_read_input_tokens":300}},"uuid":"a2","timestamp":"2026-08-01T19:13:21.000Z","sessionId":"cc-1"})",
        R"({"parentUuid":"a2","isSidechain":false,"type":"user","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"toolu_1","content":"# Proj\nhello"}]},"uuid":"u2","timestamp":"2026-08-01T19:13:22.000Z","sessionId":"cc-1"})",
        R"({"parentUuid":"u2","isSidechain":true,"type":"assistant","message":{"id":"msg_side","role":"assistant","content":[{"type":"text","text":"subagent noise"}]},"uuid":"s1","sessionId":"cc-1"})",
        R"({"parentUuid":"u2","isSidechain":false,"type":"assistant","message":{"id":"msg_2","role":"assistant","model":"claude-opus-4-7","content":[{"type":"text","text":"Rewound answer"}]},"uuid":"a3old","timestamp":"2026-08-01T19:13:23.000Z","sessionId":"cc-1"})",
        R"({"parentUuid":"u2","isSidechain":false,"type":"assistant","message":{"id":"msg_3","role":"assistant","model":"claude-opus-4-7","content":[{"type":"text","text":"It says hello."},{"type":"tool_use","id":"toolu_2","name":"Edit","input":{"file_path":"/home/me/proj/README.md","old_string":"hello","new_string":"hi"}}]},"uuid":"a3","timestamp":"2026-08-01T19:13:24.000Z","sessionId":"cc-1"})",
        R"({"parentUuid":"a3","isSidechain":false,"type":"user","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"toolu_2","content":"ok","is_error":true}]},"uuid":"u3","timestamp":"2026-08-01T19:13:25.000Z","sessionId":"cc-1"})",
        R"({"type":"summary","summary":"Readme tweak","leafUuid":"u3"})",
    });
    const fg::Parsed p = fg::parse_claude(text);
    CHECK(p.session_id == "cc-1");
    CHECK(p.cwd == "/home/me/proj");
    CHECK(p.title == "Readme tweak");
    CHECK(!p.skip);

    REQUIRE(p.messages.size() == 3);
    CHECK(p.messages[0].text == "read the readme");

    const Message& a1 = p.messages[1];
    CHECK(a1.thinking == "Let me look.", "the split lines of one reply join up");
    CHECK(a1.thinking_signature.empty(), "signatures are dropped");
    REQUIRE(a1.tool_calls.size() == 1);
    CHECK(a1.tool_calls[0].name.value == "read");
    CHECK(a1.tool_calls[0].args["path"] == "/home/me/proj/README.md");
    CHECK(a1.tool_calls[0].output() == "# Proj\nhello");
    REQUIRE(a1.telemetry.has_value());
    CHECK(a1.telemetry->cache_read == 300);

    const Message& a3 = p.messages[2];
    CHECK(a3.text == "It says hello.", "the rewound branch is gone");
    REQUIRE(a3.tool_calls.size() == 1);
    CHECK(a3.tool_calls[0].name.value == "edit");
    CHECK(a3.tool_calls[0].args["edits"][0]["new_text"] == "hi");
    CHECK(a3.tool_calls[0].is_failed());
    for (const auto& m : p.messages) CHECK(m.text != "subagent noise");
}

TEST_CASE("claude: SDK runs are not the user's sessions") {
    const std::string text = lines({
        R"({"parentUuid":null,"type":"user","message":{"role":"user","content":"x"},"uuid":"u1","sessionId":"cc-2","entrypoint":"sdk-ts","cwd":"/p"})",
    });
    CHECK(fg::parse_claude(text).skip);
}

TEST_CASE("claude: a compact summary becomes a record") {
    const std::string text = lines({
        R"({"parentUuid":null,"type":"user","message":{"role":"user","content":"one"},"uuid":"u1","sessionId":"cc-3","cwd":"/p","entrypoint":"cli"})",
        R"({"parentUuid":"u1","type":"assistant","message":{"id":"m1","role":"assistant","content":[{"type":"text","text":"first"}]},"uuid":"a1","sessionId":"cc-3"})",
        R"({"parentUuid":"a1","type":"user","isCompactSummary":true,"message":{"role":"user","content":"Summary: asked one, answered first."},"uuid":"u2","sessionId":"cc-3"})",
        R"({"parentUuid":"u2","type":"user","message":{"role":"user","content":"two"},"uuid":"u3","sessionId":"cc-3"})",
    });
    const fg::Parsed p = fg::parse_claude(text);
    CHECK(p.messages.size() == 3);
    REQUIRE(p.compactions.size() == 1);
    CHECK(p.compactions[0].up_to_index == 2);
}

TEST_CASE("continuing: an imported session is a valid request on every dialect") {
    // A Codex session with a tool round, reasoning and an encrypted blob,
    // then one turn typed in agentty.
    const std::string text = lines({
        R"({"timestamp":"2026-07-27T13:00:00.000Z","type":"session_meta","payload":{"id":"w1","cwd":"/p","originator":"codex-tui"}})",
        R"({"timestamp":"2026-07-27T13:00:01.000Z","type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"status?"}]}})",
        R"({"timestamp":"2026-07-27T13:00:02.000Z","type":"response_item","payload":{"type":"reasoning","summary":[],"encrypted_content":"gAAAA-ENCRYPTED"}})",
        R"({"timestamp":"2026-07-27T13:00:03.000Z","type":"response_item","payload":{"type":"custom_tool_call","call_id":"call_A","name":"exec","input":"await tools.exec_command({\"cmd\":\"git status\"});"}})",
        R"({"timestamp":"2026-07-27T13:00:04.000Z","type":"response_item","payload":{"type":"custom_tool_call_output","call_id":"call_A","output":"Script completed\nOutput:\nclean\n"}})",
        R"({"timestamp":"2026-07-27T13:00:05.000Z","type":"response_item","payload":{"type":"message","role":"assistant","content":[{"type":"output_text","text":"Clean tree."}]}})",
    });
    Thread t = fg::to_thread(fg::Source::Codex, fg::parse_codex(text), "/x.jsonl", 1, 1);
    Message next; next.role = Role::User; next.text = "now run the tests";
    t.messages.push_back(next);

    // Anthropic Messages: every tool_use is answered by a tool_result.
    {
        const auto j = nlohmann::json::parse(agentty::provider::anthropic::messages_json_string(t));
        std::set<std::string> uses, results;
        for (const auto& m : j)
            for (const auto& b : m.value("content", nlohmann::json::array())) {
                if (b.value("type", "") == "tool_use") uses.insert(b.value("id", ""));
                if (b.value("type", "") == "tool_result") results.insert(b.value("tool_use_id", ""));
                CHECK(b.value("type", "") != "thinking", "no unsigned thinking replayed");
            }
        CHECK(!uses.empty());
        CHECK(uses == results, "anthropic: calls and results pair up");
        CHECK(j.back()["role"] == "user");
    }
    // OpenAI Chat: each tool_call id has a tool message.
    {
        const auto arr = agentty::provider::openai::build_messages(t);
        std::set<std::string> calls, answered;
        for (const auto& m : arr) {
            if (m.contains("tool_calls"))
                for (const auto& c : m["tool_calls"]) calls.insert(c.value("id", ""));
            if (m.value("role", "") == "tool") answered.insert(m.value("tool_call_id", ""));
        }
        CHECK(!calls.empty());
        CHECK(calls == answered, "chat: calls and results pair up");
    }
    // OpenAI Responses: function_call + output, and the old encrypted
    // reasoning never rides along.
    {
        agentty::provider::Request req;
        req.messages = t.messages;
        const auto input = agentty::provider::responses::build_input(req);
        std::set<std::string> calls, outs;
        for (const auto& it : input) {
            if (it.value("type", "") == "function_call") calls.insert(it.value("call_id", ""));
            if (it.value("type", "") == "function_call_output") outs.insert(it.value("call_id", ""));
        }
        CHECK(!calls.empty());
        CHECK(calls == outs, "responses: calls and outputs pair up");
        CHECK(input.dump().find("ENCRYPTED") == std::string::npos);
    }
}

TEST_CASE("to_thread: stable id, provenance, summary, never auto-retitled") {
    fg::Parsed p;
    p.session_id = "019f-a";
    p.cwd = "/home/me/proj";
    Message u; u.role = Role::User; u.text = "fix the build please";
    Message a; a.role = Role::Assistant; a.text = "done";
    p.messages = {u, a};
    const Thread t = fg::to_thread(fg::Source::Codex, p, "/x/rollout.jsonl", 123, 456);
    CHECK(t.id == fg::thread_id_for(fg::Source::Codex, "019f-a"));
    CHECK(t.id.value.size() == 16);
    CHECK(t.id != fg::thread_id_for(fg::Source::ClaudeCode, "019f-a"), "tools don't collide");
    REQUIRE(t.imported.has_value());
    CHECK(t.imported->source == "codex");
    CHECK(t.imported->size == 123);
    CHECK(t.imported->messages == 2);
    CHECK(t.workspace == "/home/me/proj");
    CHECK(t.title == "fix the build please");
    CHECK(t.message_count == 2);
    CHECK(t.auto_titled, "an imported thread keeps the name it had");
}
