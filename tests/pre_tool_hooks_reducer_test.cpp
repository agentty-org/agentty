// pre_tool_hooks_reducer_test — what the reducer does with a PreToolUse
// verdict. The hooks themselves are covered in hooks_gate_test; here the
// verdict arrives as PreToolHooksDone through the real update().
#include <chrono>
#include <string>
#include <utility>

#include "agtest.hpp"

#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/tool/hooks.hpp"

namespace A = agentty;
using P = agentty::tools::hooks::PreToolDecision::Permission;

namespace {

// One assistant turn with a shell call waiting on its hooks.
A::Model waiting_model(A::Profile profile = A::Profile::Write) {
    A::Model m;
    m.d.profile = profile;
    m.ui.library.loaded = true;
    m.ui.library.hooks_pre_tool = {"Bash"};
    A::Message user;
    user.role = A::Role::User;
    user.text = "go";
    m.d.current.messages.push_back(std::move(user));
    A::Message asst;
    asst.role = A::Role::Assistant;
    A::ToolUse tc;
    tc.id     = A::ToolCallId{"call_1"};
    tc.name   = A::ToolName{"shell"};
    tc.args   = {{"command", "git status"}};
    tc.status = A::ToolUse::Pending{std::chrono::steady_clock::now()};
    tc.hook_stage = A::ToolUse::HookStage::Running;
    asst.tool_calls.push_back(std::move(tc));
    m.d.current.messages.push_back(std::move(asst));
    m.s.phase = A::phase::ExecutingTool{A::phase::Active{}};
    return m;
}

A::Model deliver(A::Model m, A::PreToolHooksDone e) {
    auto [next, cmd] = agentty::app::update(std::move(m), A::Msg{std::move(e)});
    (void)cmd;
    return std::move(next);
}

// The call, wherever it is now: a settled turn can have a next one after it.
const A::ToolUse& call(const A::Model& m) {
    for (const auto& msg : m.d.current.messages)
        for (const auto& tc : msg.tool_calls)
            if (tc.id.value == "call_1") return tc;
    FAIL("call_1 is gone");
    static const A::ToolUse none;
    return none;
}

}  // namespace

TEST_CASE("pre-tool hooks: a rewrite replaces the args the card shows and the tool runs") {
    A::PreToolHooksDone e{.id = A::ToolCallId{"call_1"}};
    e.permission   = static_cast<std::uint8_t>(P::Allow);
    e.updated_args = nlohmann::json{{"command", "rtk git status"}};
    auto m = deliver(waiting_model(A::Profile::Ask), std::move(e));
    CHECK(call(m).args.value("command", "") == "rtk git status");
    CHECK(call(m).hook_stage == A::ToolUse::HookStage::Done);
    // allow skips the Ask profile's prompt; the call is dispatched.
    CHECK_FALSE(m.d.pending_permission);
    CHECK_FALSE(call(m).is_pending());
}

TEST_CASE("pre-tool hooks: deny settles the call with the reason") {
    A::PreToolHooksDone e{.id = A::ToolCallId{"call_1"}};
    e.permission = static_cast<std::uint8_t>(P::Deny);
    e.reason     = "no git here";
    auto m = deliver(waiting_model(), std::move(e));
    CHECK(call(m).is_failed());
    CHECK(call(m).output().find("no git here") != std::string::npos);
}

TEST_CASE("pre-tool hooks: ask prompts, with the hook's reason") {
    A::PreToolHooksDone e{.id = A::ToolCallId{"call_1"}};
    e.permission = static_cast<std::uint8_t>(P::Ask);
    e.reason     = "touches prod";
    auto m = deliver(waiting_model(A::Profile::Write), std::move(e));
    REQUIRE(m.d.pending_permission);
    CHECK(m.d.pending_permission->reason.find("touches prod") != std::string::npos);
    CHECK(call(m).is_pending());
}

TEST_CASE("pre-tool hooks: a stale verdict is dropped") {
    auto m = waiting_model();
    m.d.current.messages.back().tool_calls[0].hook_stage = A::ToolUse::HookStage::Done;
    A::PreToolHooksDone e{.id = A::ToolCallId{"call_1"}};
    e.updated_args = nlohmann::json{{"command", "rm -rf /"}};
    m = deliver(std::move(m), std::move(e));
    CHECK(call(m).args.value("command", "") == "git status");
}

TEST_CASE("pre-tool hooks: continue:false ends the run after the tool") {
    A::PreToolHooksDone e{.id = A::ToolCallId{"call_1"}};
    e.permission  = static_cast<std::uint8_t>(P::Deny);
    e.reason      = "x";
    e.stop        = true;
    e.stop_reason = "enough for today";
    auto m = deliver(waiting_model(), std::move(e));
    // The only call settled; the run ends instead of starting a next turn.
    CHECK(call(m).is_terminal());
    CHECK(m.s.is_idle());
    CHECK_FALSE(m.s.hook_stop);
}
