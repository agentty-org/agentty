// Tool-execution-result helpers: apply_tool_output translates a
// ToolExecOutput into a Done/Failed status on the matching ToolUse;
// mark_tool_rejected is the symmetric one-liner for permission denial.
// Both walk m.d.current.messages because a ToolCallId is only locally
// unique within a turn — we don't index them.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/io/clipboard.hpp"

#include <algorithm>
#include <chrono>
#include <unordered_set>
#include <utility>

#include <maya/core/overload.hpp>
#include <maya/core/motion.hpp>   // anim::keep_animating — frame requests
#include <nlohmann/json.hpp>

#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_args.hpp"
#include "agentty/runtime/view/host_escape.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_helpers.hpp"
#include "agentty/runtime/view/helpers.hpp"   // pretty_model_label
#include "agentty/runtime/view/thread/turn/turn.hpp"   // turn_run_end
#include "agentty/store/store.hpp"
#include "agentty/tool/spec.hpp"
#include "agentty/tool/util/utf8.hpp"
#include "agentty/util/logx.hpp"

#if AGENTTY_MCP
#include <mcp/tools/util/shell_detour.hpp>
#endif

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

// After this many shell detours in a row the tip gets a plain reminder on
// top. 3: one detour is noise, two can be a one-off, three is the habit.
inline constexpr int kDetourReminderAt = 3;

using json = nlohmann::json;

namespace {

// Private OSC number for host (editor) integration escapes — see
// ui::host::file_event_osc. 5379 is unclaimed by mainstream terminals; the
// "agentty;" payload tag lets a host match ours and ignore other OSCs.
constexpr int kHostOsc = 5379;

// Build the host follow-along OSC payload for a finished FILE tool, or nullopt
// for non-file tools / inactive integration. Only tools that name a concrete
// path qualify (read/edit/write/move/list_dir); the path comes from the same
// streaming-aware reader the timeline header uses, and read's 1-based offset
// becomes the follow-along line.
[[nodiscard]] std::optional<std::string> file_event_osc_for(const ToolUse& tc) {
    const std::string& n = tc.name.value;
    const bool is_file_tool =
        n == "read" || n == "edit" || n == "write" || n == "move"
        || n == "list_dir";
    if (!is_file_tool) return std::nullopt;
    std::string path = ui::tool_path_arg(tc);
    if (n == "move" && path.empty())
        path = ui::safe_arg(tc.args, "destination");
    if (path.empty()) return std::nullopt;
    std::optional<int> line;
    if (n == "read") {
        if (int off = ui::safe_int_arg(tc.args, "offset", 0); off > 0) line = off;
    }
    return ui::host::file_event_osc(n, path, line);
}

// Per-tool-output ceiling carried in the conversation. Tool runners
// already cap their own captures (read = 1 MiB, grep = 8 MiB, bash =
// 30 KB, etc.), but those are tuned for fidelity inside one call.
// For long-lived process memory, a uniform conversation-side cap is
// the right knob: a session with 50 grep results at near-cap was
// 400+ MB of *terminal* tool output we never trim. Head + tail
// keeps the model-relevant context (top of file, last error, etc.)
// and lets a multi-MiB capture compact to ~256 KiB.
constexpr std::size_t kStoredOutputCap = 256u * 1024u; // 256 KiB

std::string clamp_output(std::string s) {
    if (s.size() <= kStoredOutputCap) return s;
    constexpr std::size_t half = kStoredOutputCap / 2;
    // Both cut points MUST land on UTF-8 code-point boundaries: the
    // clamped string is what goes onto the wire as the tool_result, and
    // nlohmann::json::dump() throws type_error.316 on a split multi-byte
    // sequence — which would kill serialization of the whole request.
    // Head: largest boundary ≤ half. Tail: walk the start FORWARD past
    // any continuation bytes so the kept suffix begins on a lead byte.
    const std::size_t head_end = tools::util::safe_utf8_cut(s, half);
    std::size_t tail_start = s.size() - half;
    while (tail_start < s.size()
           && (static_cast<unsigned char>(s[tail_start]) & 0xC0) == 0x80)
        ++tail_start;
    std::string out;
    out.reserve(kStoredOutputCap + 64);
    out.append(s, 0, head_end);
    out.append("\n\n… (");
    out.append(std::to_string((s.size() - head_end - (s.size() - tail_start)) / 1024));
    out.append(" KiB elided to keep memory bounded) …\n\n");
    out.append(s, tail_start, s.size() - tail_start);
    return out;
}

// Once a tool reaches a terminal state, the per-call streaming
// scratch buffers (raw delta accumulator, lazy args.dump cache) are
// dead weight — the wire is closed, the args are parsed, and any
// re-render comes from `args` directly. Free them so a long session
// doesn't pin one copy per finished tool call.
void release_streaming_buffers(ToolUse& tc) {
    std::string{}.swap(tc.args_streaming);
    tc.args_dump_cache.clear();
    tc.args_dump_cache.shrink_to_fit();
    tc.args_dump_valid = false;
}

// Keep the render clock ticking for a bounded window so maya gets the
// follow-up frames its live-tail shrink/overflow reconciliation needs
// after a card's rendered height changes mid-turn. This is the same
// lever the deferred settle-freeze uses (subscribe.cpp gates the tick
// on settle_cooldown_ticks > 0); arming it here at every card-height
// mutation reproduces agent_session's always-on clock at the exact
// seams where the height changes. Grow-only so a longer in-flight
// window is never truncated by a shorter one.
void arm_reconcile_cooldown(Model& m) {
    constexpr int kReconcileTicks = 6;
    if (m.ui.settle_cooldown_ticks < kReconcileTicks)
        m.ui.settle_cooldown_ticks = kReconcileTicks;
    ::maya::anim::keep_animating();
}


} // namespace

// Build the tool-output-viewer entry list: every settled tool call in the
// current thread with a non-empty stored output, NEWEST FIRST (the one the
// user just watched scroll past is entry 0). Bounded by kMaxEntries /
// kSnapshotBudget so a marathon session can't balloon the overlay open.
// Snapshotting (copying the output bytes) makes the overlay immune to the
// transcript mutating underneath it — each stored output is already
// clamped to 256 KiB upstream, so the copies are cheap and bounded.
[[nodiscard]] std::vector<tool_output::Entry> collect_viewer_entries(const Model& m) {
    std::vector<tool_output::Entry> out;
    std::size_t budget = tool_output::kSnapshotBudget;

    // Row 0 is the LIVE tool, if one is active right now: this includes
    // argument streaming / permission (Pending or Approved) as well as local
    // execution (Running). Restricting this to Running made Ctrl+O say
    // "nothing to inspect yet" during the entire model→tool-input phase —
    // exactly when edit/write previews are most useful.
    for (auto mit = m.d.current.messages.rbegin();
         mit != m.d.current.messages.rend(); ++mit) {
        bool found = false;
        for (auto tit = mit->tool_calls.rbegin();
             tit != mit->tool_calls.rend(); ++tit) {
            const auto* run = std::get_if<ToolUse::Running>(&tit->status);
            if (!run && !tit->is_pending() && !tit->is_approved()) continue;
            tool_output::Entry e;
            e.is_live  = true;
            e.name     = tit->name.value;
            e.title    = ui::tool_display_name(tit->name.value);
            e.detail   = ui::tool_timeline_detail(*tit);
            e.output   = run ? run->progress_text : std::string{};
            e.call     = *tit;
            e.failed   = false;
            char buf[32];
            const char* phase = run ? "running"
                : (tit->is_approved() ? "approved" : "streaming input");
            std::snprintf(buf, sizeof buf, "%s \xc2\xb7 %.1fs",
                          phase, ui::tool_elapsed(*tit));
            e.trailing = buf;
            if (!e.output.empty())
                e.trailing += (e.output.size() >= 1024)
                    ? " \xc2\xb7 " + std::to_string(e.output.size() / 1024) + " KB"
                    : " \xc2\xb7 " + std::to_string(e.output.size()) + " B";
            budget -= std::min(budget, e.output.size());
            out.push_back(std::move(e));
            found = true;
            break;
        }
        if (found) break;
    }
    for (auto mit = m.d.current.messages.rbegin();
         mit != m.d.current.messages.rend(); ++mit) {
        // Retrieved-context cards surface here too, so the FULL passages the
        // model was grounded on are readable in the Ctrl+O overlay — including
        // after the card has scrolled into the (immutable) frozen prefix,
        // where the in-place Ctrl+U expand can't reach. Rendered through the
        // viewer's plain-text (line-numbered) body path: the default-
        // constructed ToolUse `call` has no structured kind, so the body
        // stage falls through to the raw-text branch automatically.
        if (mit->is_proactive_context() && !mit->text.empty()) {
            // Strip the <retrieved-context> … </retrieved-context> fence and
            // the lead-in preamble so the body is just the passages.
            std::string body = mit->text;
            if (auto open = body.find('>'); open != std::string::npos
                && body.compare(0, 18, "<retrieved-context") == 0) {
                std::size_t start = body.find('\n', open);
                start = (start == std::string::npos) ? open + 1 : start + 1;
                std::size_t end = body.rfind("</retrieved-context>");
                if (end == std::string::npos || end < start) end = body.size();
                body = body.substr(start, end - start);
            }
            while (!body.empty() && (body.back() == '\n' || body.back() == ' '
                                  || body.back() == '\t' || body.back() == '\r'))
                body.pop_back();
            if (!body.empty() && out.size() < tool_output::kMaxEntries
                && body.size() <= budget) {
                budget -= body.size();
                tool_output::Entry e;
                e.name   = "retrieved_context";
                e.title  = "Retrieved context";
                e.source = mit->id;
                e.output = std::move(body);
                e.failed = false;
                // Detail: passage count (each source block starts with "[").
                std::size_t passages = 0;
                for (std::size_t p = 0; (p = e.output.find("[", p)) != std::string::npos; ++p)
                    if (p == 0 || e.output[p - 1] == '\n') ++passages;
                e.detail = passages == 0 ? std::string{}
                    : std::to_string(passages)
                        + (passages == 1 ? " passage" : " passages");
                e.trailing = (e.output.size() >= 1024)
                    ? std::to_string(e.output.size() / 1024) + " KB"
                    : std::to_string(e.output.size()) + " B";
                out.push_back(std::move(e));
            }
            continue;
        }
        for (auto tit = mit->tool_calls.rbegin();
             tit != mit->tool_calls.rend(); ++tit) {
            const auto& tc = *tit;
            if (!tc.is_terminal()) continue;
            const auto& body = tc.output();
            if (body.empty()) continue;
            if (out.size() >= tool_output::kMaxEntries) return out;
            if (body.size() > budget) continue;   // skip, keep older smaller ones
            budget -= body.size();

            tool_output::Entry e;
            e.failed = tc.is_failed();
            e.output = body;
            // Raw name drives the category colour badge; display name +
            // detail reuse the timeline's helpers so the list reads
            // exactly like the transcript cards being re-inspected.
            e.name   = tc.name.value;
            e.title  = ui::tool_display_name(tc.name.value);
            e.detail = ui::tool_timeline_detail(tc);
            e.call   = tc;   // snapshot for the rich body render (diff/gutter)
            // Trailing: duration · size; a failure says so first.
            e.trailing = e.failed ? "failed" : "";
            if (float secs = ui::tool_elapsed(tc); secs >= 0.05f) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%s%.1fs",
                              e.trailing.empty() ? "" : " \xc2\xb7 ", secs);
                e.trailing += buf;
            }
            if (!e.trailing.empty()) e.trailing += " \xc2\xb7 ";
            e.trailing += (body.size() >= 1024)
                ? std::to_string(body.size() / 1024) + " KB"
                : std::to_string(body.size()) + " B";
            out.push_back(std::move(e));
        }
    }
    return out;
}

namespace {

// "820", "1.2k", "18k" — the same ~4 chars/token estimate the block shows.
std::string token_estimate(std::size_t chars) {
    const std::size_t t = (chars + 3) / 4;
    if (t < 1000) return std::to_string(t);
    if (t < 10000) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.1fk", static_cast<double>(t) / 1000.0);
        return buf;
    }
    return std::to_string(t / 1000) + "k";
}

// First non-blank line, without markdown emphasis: what the block is about.
std::string thinking_gist(std::string_view s) {
    while (!s.empty()) {
        auto nl = s.find('\n');
        std::string_view ln = s.substr(0, nl);
        while (!ln.empty() && (ln.front() == ' ' || ln.front() == '#' || ln.front() == '*'
                               || ln.front() == '-' || ln.front() == '>'))
            ln.remove_prefix(1);
        while (!ln.empty() && (ln.back() == ' ' || ln.back() == '*' || ln.back() == '\r'))
            ln.remove_suffix(1);
        if (!ln.empty()) return std::string{ln.substr(0, 240)};
        if (nl == std::string_view::npos) break;
        s.remove_prefix(nl + 1);
    }
    return {};
}

tool_output::Entry thinking_entry(const Model& m, const Message& msg, int turn) {
    tool_output::Entry e;
    e.thinking  = true;
    e.turn      = turn;
    e.source    = msg.id;
    e.streaming = msg.thinking_open();
    e.name      = "thinking";
    e.title     = "Thinking";
    e.output    = std::string{msg.reasoning_display_text()};
    e.detail    = (turn > 0 ? "turn " + std::to_string(turn) + "  " : std::string{})
                + thinking_gist(e.output);
    e.trailing  = "~" + token_estimate(e.output.size()) + " tok";
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        m.now.time_since_epoch()).count();
    if (const auto ms = msg.thinking_elapsed_ms(now_ms); ms >= 100) {
        char buf[32];
        std::snprintf(buf, sizeof buf, " \xc2\xb7 %.1fs", static_cast<double>(ms) / 1000.0);
        e.trailing += buf;
    }
    if (e.streaming) e.trailing += " \xc2\xb7 thinking\xe2\x80\xa6";

    // What's known about the block, for the reader's header.
    auto& f = e.facts;
    const std::string& model = msg.served_model.value;
    if (!model.empty()) f.emplace_back("model", ui::pretty_model_label(model));
    if (!msg.served_effort.empty()) f.emplace_back("effort", msg.served_effort);
    if (const auto ms = msg.thinking_elapsed_ms(now_ms); ms >= 100) {
        char buf[32];
        if (e.streaming)   // whole seconds: it only refreshes on new text
            std::snprintf(buf, sizeof buf, "%llds so far", static_cast<long long>(ms / 1000));
        else
            std::snprintf(buf, sizeof buf, "%.1fs", static_cast<double>(ms) / 1000.0);
        f.emplace_back("thought for", buf);
    }
    // Tokens: the provider's count when it reported one (Claude bills the
    // full thinking even when it only shows a summary), else our estimate.
    if (msg.telemetry && msg.telemetry->reasoning_tokens > 0)
        f.emplace_back("tokens", std::to_string(msg.telemetry->reasoning_tokens)
                                 + " billed \xc2\xb7 ~" + token_estimate(e.output.size()) + " shown");
    else
        f.emplace_back("tokens", "~" + token_estimate(e.output.size()));
    // A summary is the provider's digest of the thinking, not the thinking.
    f.emplace_back("kind", msg.thinking.empty() ? "summary" : "full text");
    std::size_t shown = 0, redacted = 0;
    for (const auto& b : msg.thinking_blocks)
        (b.redacted_data.empty() ? shown : redacted) += 1;
    if (shown + redacted > 1 || redacted > 0) {
        std::string v = std::to_string(shown + redacted);
        if (redacted > 0) v += " (" + std::to_string(redacted) + " redacted by the provider)";
        f.emplace_back("blocks", std::move(v));
    }
    if (e.streaming) f.emplace_back("status", "thinking now");
    return e;
}

} // namespace

[[nodiscard]] std::vector<tool_output::Entry> collect_thinking_entries(const Model& m) {
    const auto& msgs = m.d.current.messages;
    // Turn number per message, counted the way the transcript labels turns:
    // one per assistant run.
    std::vector<int> turn_of(msgs.size(), 0);
    int turn = 0;
    for (std::size_t k = 0; k < msgs.size();) {
        const std::size_t end = ui::turn_run_end(msgs, k);
        if (msgs[k].role == Role::Assistant) ++turn;
        for (std::size_t j = k; j < end && j < msgs.size(); ++j) turn_of[j] = turn;
        k = std::max(end, k + 1);
    }
    std::vector<tool_output::Entry> out;
    std::size_t budget = tool_output::kSnapshotBudget;
    for (std::size_t i = msgs.size(); i-- > 0;) {
        const auto& msg = msgs[i];
        if (msg.role != Role::Assistant || !msg.has_reasoning()) continue;
        if (out.size() >= tool_output::kMaxEntries) break;
        const auto size = msg.reasoning_display_text().size();
        if (size > budget) continue;
        budget -= size;
        out.push_back(thinking_entry(m, msg, turn_of[i]));
    }
    return out;
}

namespace {

bool entry_running(const tool_output::Entry& e) { return e.is_live || e.streaming; }

// Put a fresh snapshot in the viewer. Following: the cursor goes to the
// new top entry. Otherwise it stays on the entry you're on, by identity,
// and entries that land above are counted for the "↑ N new" pill.
void reconcile(Model& m, tool_output::Open& o, std::vector<tool_output::Entry> fresh) {
    const int sz = static_cast<int>(o.entries.size());
    const bool follow = viewer_following(m, o);
    const std::string was = (o.index >= 0 && o.index < sz)
        ? std::string{tool_output::entry_key(o.entries[static_cast<std::size_t>(o.index)])}
        : std::string{};

    if (follow) {
        const bool moved = fresh.empty() || was != tool_output::entry_key(fresh.front());
        o.entries = std::move(fresh);
        o.index = 0;
        o.fresh_above = 0;
        if (moved && o.viewing) {
            m.ui.tool_viewer_scroll.y = 0;
            m.ui.tool_viewer_tail = true;
        }
        return;
    }

    std::unordered_set<std::string_view> seen;
    for (const auto& e : o.entries) seen.insert(tool_output::entry_key(e));
    int at = -1;
    for (std::size_t k = 0; k < fresh.size(); ++k)
        if (!was.empty() && tool_output::entry_key(fresh[k]) == was) { at = static_cast<int>(k); break; }
    if (at < 0) {
        // Our entry left the snapshot (rare: the budget dropped it). Stay
        // at the same distance from the bottom, which is what didn't move.
        at = static_cast<int>(fresh.size()) - (sz - o.index);
    }
    at = std::clamp(at, 0, std::max(0, static_cast<int>(fresh.size()) - 1));
    int added = 0;
    for (int k = 0; k < at; ++k)
        if (!seen.contains(tool_output::entry_key(fresh[static_cast<std::size_t>(k)]))) ++added;
    o.fresh_above = at == 0 ? 0 : o.fresh_above + added;
    o.entries = std::move(fresh);
    o.index = at;
    arm_viewer_follow(m, o);
}

} // namespace

bool viewer_following(const Model& m, const tool_output::Open& o) {
    return o.following && o.index == 0 && o.query.empty() && !o.typing
        && (!o.viewing || m.ui.tool_viewer_tail);
}

void arm_viewer_follow(const Model& m, tool_output::Open& o) {
    if (o.index == 0 && !o.entries.empty() && entry_running(o.entries.front())
        && o.query.empty() && !o.typing && (!o.viewing || m.ui.tool_viewer_tail))
        o.following = true;
}

void refresh_thinking_viewer(Model& m, const Message& msg) {
    auto* o = m.ui.panel.get<pn::ToolOutput>();
    if (!o || !o->thinking_only || !msg.has_reasoning()) return;
    for (auto& e : o->entries)
        if (e.source == msg.id) {
            const int turn = e.turn;
            e = thinking_entry(m, msg, turn);
            arm_viewer_follow(m, *o);
            return;
        }
    // A block the reader hasn't seen yet.
    auto fresh = collect_thinking_entries(m);
    if (!fresh.empty()) reconcile(m, *o, std::move(fresh));
}

// Keep the Ctrl+O snapshot synchronized from both stream.cpp (tool-input
// deltas/snapshots) and this TU (execution progress/results).
void resync_live_tool_viewer(Model& m) {
    auto* o = m.ui.panel.get<pn::ToolOutput>();
    if (!o) return;
    if (o->thinking_only) return;   // the ^U reader follows thinking, not tools
    auto fresh = collect_viewer_entries(m);
    if (fresh.empty()) return;   // keep the last snapshot rather than blank
    reconcile(m, *o, std::move(fresh));
}

void apply_tool_output(Model& m, const ToolCallId& id,
                       std::expected<std::string, tools::ToolError>&& result,
                       std::optional<FileChange>&& change,
                       std::vector<FileChange>&& changes,
                       std::vector<ImageContent>&& images,
                       std::uint64_t exec_seq) {
    const bool landed = with_exec_tool(m, id, exec_seq, [&](ToolUse& tc) {
        // Idempotent: a tool already in a terminal state
        // (Done / Failed / Rejected) keeps that state. Realistic
        // ways a late ToolExecOutput can land here:
        //   (a) Wall-clock watchdog force-failed the tool at
        //       60 s; the worker thread eventually unwound
        //       seconds/minutes later. The original failure
        //       reason ("hung") is more useful to the user
        //       than the late output would be — and overwriting
        //       could re-arm a turn that's already advanced
        //       past this tool.
        //   (b) A duplicate dispatch on the same id (shouldn't
        //       happen but cheap to defend against).
        // Either way, dropping the late result keeps history
        // stable.
        //
        // Frozen prefix: with_live_tool already skips messages with
        // index < frozen_through, so a ToolExecOutput that races a
        // freeze (turn settled, user submitted again, tool worker
        // finally returned) silently no-ops here. Without the gate
        // the mutation would land on a Message whose rendered
        // Element in m.ui.frozen is immutable — visible as a
        // permanently-Running spinner in scrollback.
        if (tc.is_terminal()) return;
        auto now = m.now;
        if (result) {
            // Drift reminder. Shell begets shell: after a shell inspection
            // the next one is shell 81% of the time (22% after a native
            // one). The per-call tip is easy to skim past once the habit has
            // set in, so when detours chain, say so plainly at the point of
            // decision (event-driven reminder, cf. OpenDev 2603.05344
            // §2.3.4). Counted with substitutable(), the same gate the tip
            // uses. Any other call resets the streak.
            bool detour = false;
#if AGENTTY_MCP
            if (tc.name.value == "shell" && tc.args.is_object())
                if (auto it = tc.args.find("command"); it != tc.args.end() && it->is_string())
                    detour = ::mcp::tools::util::analyze_detour(it->get<std::string>()).substitutable();
#endif
            if (detour) {
                if (++m.d.shell_detour_streak >= kDetourReminderAt)
                    *result = "[reminder] shell detour #"
                        + std::to_string(m.d.shell_detour_streak)
                        + " in a row. the tip below names the exact native "
                          "call; make that call next. native calls run in "
                          "parallel and each gets its own card.\n"
                        + std::move(*result);
            } else {
                m.d.shell_detour_streak = 0;
            }
            tc.settle_done(now, clamp_output(std::move(*result)),
                           std::move(images));
        } else {
            // Render typed error as "[kind] detail" so the category
            // is visible in tool-card / history without losing the
            // human-readable detail. The model needs only the
            // string back; the kind is preserved structurally for
            // the future, when the view branches on category.
            tc.settle_failed(now, clamp_output(result.error().render()));
        }
        release_streaming_buffers(tc);
    });
    // A stale worker (its call was cancelled) must not queue diffs for
    // review either: the user already walked away from that call.
    if (exec_seq != 0 && !landed) return;
    // Queue the structured change(s) (edit / write / apply_patch / replace)
    // for diff-review. Multi-file tools (replace) fill `changes`; single-file
    // tools fill `change`. DEDUP per file: successive edits to the SAME path
    // collapse into one review entry showing net original→latest, so the pane
    // reads as "here's what changed in login.cpp", not a stack of overlapping
    // diffs. Only queue on success — a failed tool wrote nothing.
    if (result) {
        auto queue_one = [&](FileChange&& ch) {
            auto& q = m.d.pending_changes;
            auto it = std::ranges::find(q, ch.path, &FileChange::path);
            if (it != q.end()) {
                std::string original = it->original_contents;
                *it = diff::compute(ch.path, original, ch.new_contents);
            } else {
                q.push_back(std::move(ch));
            }
        };
        if (!changes.empty())
            for (auto& ch : changes) queue_one(std::move(ch));
        else if (change)
            queue_one(std::move(*change));
    }
    // running spinner into the full output/error body — the card's
    // rendered height changes at this instant (a Failed card in
    // particular grows by its error rows). If that height change shifts
    // the overflowed prefix while the clock is about to lapse (last tool
    // of the turn dropping toward Idle, or a coalesced fps=0 frame),
    // maya composes the shifted frame ONCE and never gets the follow-up
    // frames its shrink/overflow reconciliation needs — the old card's
    // top rows strand in scrollback (the "card cut off one screen up"
    // corruption). agent_session never sees this because its clock ticks
    // UNCONDITIONALLY. Mirror that: arm the reconciliation cooldown so
    // the clock keeps running for a few frames after any card-height
    // change, guaranteeing maya reconciles the seam exactly like the
    // reference's always-on tick.
    arm_reconcile_cooldown(m);
}

void mark_tool_rejected(Model& m, const ToolCallId& id,
                        std::string_view reason) {
    with_live_tool(m, id, [&](ToolUse& tc) {
        auto now = m.now;
        if (reason.empty()) {
            tc.status = ToolUse::Rejected{now};
        } else {
            // A reason means a policy/failure rejection. settle_failed keeps a
            // real exec window if the tool had dispatched, and records NONE if
            // it hadn't -- which for "User rejected this tool call" is always:
            // the tool was sitting at the permission prompt. Copying with
            // executing_since() here used to bake card birth in as a fake
            // dispatch stamp and bill the whole prompt wait as tool time.
            tc.settle_failed(now, clamp_output(std::string{reason}));
        }
        release_streaming_buffers(tc);
    });
    // Same rationale as apply_tool_output: a rejected tool's card body
    // changes height (spinner → rejection reason), so keep the clock
    // ticking a few frames to let maya reconcile the seam.
    arm_reconcile_cooldown(m);
}

// ============================================================================
// tool_update — reducer for `msg::ToolMsg`
// ============================================================================
// Tool-execution results from the local runner + permission-prompt
// resolutions from the user. Permission lives here because a permission
// prompt is always *about* a specific pending tool call and the resolution
// feeds back into the tool state machine — no clean split.

Cmd tool_update(Model& m, msg::ToolMsg tm) {
    using maya::overload;

    return std::visit(overload{
        // ── Live tool progress (streaming subprocess output) ────────────
        // Arrives from the subprocess runner every ~80 ms with the full
        // accumulated output so far. We just set it — no Cmd to return —
        // and rely on the existing Tick subscription (active during
        // ExecutingTool) to re-render. Ignore if the tool has already
        // finalised (a late snapshot racing the terminal ToolExecOutput).
        [&](ToolExecProgress& e) -> Cmd {
            // Frozen prefix is immutable — a late progress snapshot
            // for a turn that's already settled into m.ui.frozen
            // silently no-ops here.
            with_exec_tool(m, e.id, e.exec_seq, [&](ToolUse& tc) {
                if (auto* r = std::get_if<ToolUse::Running>(&tc.status)) {
                    // Belt-and-braces terminal line-discipline. The in-tree
                    // subprocess runners already clean at the capture
                    // boundary, but snapshots can also arrive from paths
                    // that don't ride them (external MCP servers streaming
                    // progress, subagent feeds echoing tool output). A raw
                    // ESC/CR/BS byte reaching the card body paints control
                    // bytes as cells and commits them to scrollback — the
                    // stray-glyph corruption class. Cheap: O(n) scan, and
                    // the common (already-clean) case only copies.
                    // Trim first, then strip. Snapshots arrive pre-cut from the
                    // in-tree runners (progress_tail on the worker); the cut
                    // here bounds any other source to the same window. Bounded
                    // because progress_text is copied into the card every
                    // frame the live timeline is rebuilt.
                    r->progress_text = tools::util::strip_terminal_controls(
                        tools::util::progress_tail(e.snapshot));
                    // Reset the liveness clock: this tool is demonstrably
                    // making progress, so the wedge net's cap restarts from
                    // here rather than from the (possibly many-minutes-ago)
                    // launch time.
                    r->last_progress_at = m.now;
                }
            });
            // If the user is watching the Ctrl+O viewer, keep its Live row
            // (row 0) tailing this fresh output in place.
            resync_live_tool_viewer(m);
            return Cmd::none();
        },

        // ── Per-tool wall-clock watchdog ──────────────────────────────────
        [&](ToolTimeoutCheck& e) -> Cmd {
            bool flipped = false;
            with_live_tool(m, e.id, [&](ToolUse& tc) {
                if (tc.is_terminal()) return;
                auto now = m.now;
                const auto* sp = tools::spec::lookup(tc.name.value);
                auto secs = sp ? sp->max_seconds : std::chrono::seconds{0};
                std::string reason;
                if (tc.is_pending() || tc.is_approved()) {
                    reason = "tool stayed " + std::string{tc.status_name()}
                        + " for " + std::to_string(secs.count())
                        + " s \xe2\x80\x94 args probably never finished streaming "
                        "(transient API error mid-tool_use, or the "
                        "stream silently exited without a terminal event).";
                } else {
                    reason = "tool execution exceeded "
                        + std::to_string(secs.count())
                        + " s wall-clock \xe2\x80\x94 likely hung on a blocking "
                        "syscall (slow/dead filesystem mount, network "
                        "freeze, or worker deadlock). The tool's worker "
                        "thread may continue in the background; its "
                        "result will be discarded if it ever returns.";
                }
                tc.settle_failed(now, std::move(reason));
                flipped = true;
            });
            if (!flipped) return Cmd::none();
            auto cmd = cmd::kick_pending_tools(m);
            return cmd;
        },

        // ── Tool execution result ───────────────────────────────────────
        [&](ToolExecOutput& e) -> Cmd {
            // A worker whose call was cancelled or settled; its id may now
            // belong to a different call. Drop it whole.
            if (!tool_exec_is_current(m, e.id, e.exec_seq)) {
                AGT_LOG(Tool, Info, "tool.stale_result",
                        "id={} seq={} dropped", e.id.value, e.exec_seq);
                return Cmd::none();
            }
            // todo's side effect on the UI's plan state — runs only
            // when the call actually succeeded; failures don't synthesise
            // a plan. The final exact state lands here even if the live
            // streaming sync (stream.cpp) raced a partial array.
            if (e.result) {
                // Newest-first and stop at the match: the call is in the
                // current turn, so this is O(1) in practice instead of a
                // walk over every tool call in the thread per result.
                bool found = false;
                for (auto mit = m.d.current.messages.rbegin();
                     !found && mit != m.d.current.messages.rend(); ++mit)
                    for (const auto& tc : mit->tool_calls)
                        if (tc.id == e.id) {
                            if (tc.name == "todo") sync_todo_state_from_args(m, tc.args);
                            found = true;
                            break;
                        }
            }
            apply_tool_output(m, e.id, std::move(e.result), std::move(e.change),
                              std::move(e.changes), std::move(e.images),
                              e.exec_seq);
            // If the Ctrl+O viewer is open, refresh it so the Live row settles
            // into a finished entry the instant this tool completes.
            resync_live_tool_viewer(m);

            // Cooperating-host follow-along: when running on an editor PTY
            // (Emacs/vterm) that watches for our OSC, tell it which file the
            // agent just touched so it can open / reveal / diff it natively.
            // Frame-safe (maya cmd::emit_osc, out-of-band), and a complete
            // no-op on a normal terminal (m.env.host_integration is false).
            Cmd host_cmd = Cmd::none();
            if (m.env.host_integration) {
                for (const auto& msg_ : m.d.current.messages) {
                    bool done_scan = false;
                    for (const auto& tc : msg_.tool_calls) {
                        if (tc.id != e.id || !tc.is_done()) continue;
                        if (auto osc = file_event_osc_for(tc))
                            host_cmd = cmd::emit_osc(kHostOsc, *osc);
                        done_scan = true; break;
                    }
                    if (done_scan) break;
                }
            }

            // No mid-run freeze or trim here. The single freeze site is
            // finalize_turn (the agent_session MessageStop analog) — the
            // whole agent turn is wrapped into one Turn Element and
            // pushed to m.ui.frozen atomically there. Carving mid-run
            // (the prior freeze_settled_subturns + trim_frozen_above_
            // viewport calls that lived here) was the documented source
            // of "redraws from top + scrollback corruption" at every
            // tool→continuation seam: the freeze pushed an entry whose
            // hash_id maya's component cache had not seen on the live
            // tail's previous frame, so the cache missed and re-emitted
            // those rows — sometimes over already-committed scrollback.
            // agent_session never carves mid-stream and shows zero
            // corruption / zero slowdown on long runs (proven by the
            // long_session bench); we now do the same.
            // A SPECULATIVE read-only tool (launched mid-stream at
            // StreamToolUseEnd) can finish while the wire is still
            // streaming this turn. Do NOT run kick_pending_tools then:
            // it would pull the Active ctx out of phase::Streaming (or
            // worse, see zero pending work and launch the continuation
            // stream against a wire that is still delivering). Just land
            // the output; StreamFinished → finalize_turn runs the normal
            // kick and finds this tool already terminal.
            if (m.s.is_streaming()) {
                if (host_cmd.is_none()) return Cmd::none();
                return host_cmd;
            }
            auto kick = cmd::kick_pending_tools(m);
            if (host_cmd.is_none())
                return kick;
            return Cmd::batch(
                std::move(kick), std::move(host_cmd));
        },

        // ── Permission ──────────────────────────────────────────────
        [&](PermissionApprove) -> Cmd {
            if (!m.d.pending_permission) return Cmd::none();
            auto id = m.d.pending_permission->id;
            // Permission only ever fires against a tool in the live
            // tail — a frozen turn is by definition past every pending
            // permission. with_live_tool's frozen-prefix gate is the
            // structural guarantee of that invariant.
            with_live_tool(m, id, [&, shown = m.d.pending_permission->def_hash](ToolUse& tc) {
                // Mark approval as type state: Pending → Approved.
                // kick_pending_tools then treats Approved as
                // "permission already granted" and routes through
                // the same effect-parallel gate as a non-permissioned
                // tool — so if a sibling Read is still running, a
                // freshly approved Write/Bash waits for it instead
                // of racing.
                tc.status = ToolUse::Approved{tc.started_at()};
                // What the user actually saw. If the catalog is rebuilt
                // between here and the launch, execute_approved refuses
                // rather than running a different tool under this name.
                tc.approved_def_hash = shown;
            });
            m.d.pending_permission.reset();
            return cmd::kick_pending_tools(m);
        },
        [&](PermissionReject) -> Cmd {
            if (!m.d.pending_permission) return Cmd::none();
            auto id = m.d.pending_permission->id;
            mark_tool_rejected(m, id, "User rejected this tool call.");
            m.d.pending_permission.reset();
            auto cmd = cmd::kick_pending_tools(m);
            return cmd;
        },
        [&](PermissionApproveAlways) -> Cmd {
            if (!m.d.pending_permission) return Cmd::none();
            auto id   = m.d.pending_permission->id;
            auto name = m.d.pending_permission->tool_name;
            // Record a session-scoped grant for this tool NAME so every
            // future call to it this session auto-approves (consulted in
            // kick_pending_tools). This also propagates to sibling
            // pending tools of the same name in the current batch: the
            // re-kick below re-evaluates each pending tool against the
            // now-populated grant set, so a queued sibling `bash` won't
            // re-prompt. Mirrors Zed's per-session allow-list with live
            // sibling propagation.
            m.d.session_grants.insert(name.value);
            // And WHAT was approved, not just its name. A standing grant on a
            // bare name covers whatever that name resolves to later, which
            // for an MCP tool is the server's choice -- so the grant carries
            // the fingerprint of the definition the user saw, and
            // execute_approved refuses if it changes.
            const auto granted_hash = m.d.pending_permission->def_hash;
            if (granted_hash != 0) m.d.grant_def_hash[name.value] = granted_hash;
            // Persist the grant (Zed's always_allow rules): reload-proof.
            // Onto the record, so the next whole-record save carries it
            // instead of writing a copy that predates it.
            //
            // Stored as "name@<hex>" so the definition survives a restart
            // too. A bare name (an older build's entry, or a hand-edited
            // settings.json) still loads and means "any definition".
            Cmd grant_save = Cmd::none();
            {
                auto& allow = m.d.persisted.always_allow_tools;
                std::string entry = name.value;
                if (granted_hash != 0)
                    entry += "@" + std::format("{:016x}", granted_hash);
                const auto same_tool = [&](const std::string& e) {
                    const auto at = e.rfind('@');
                    return (at == std::string::npos ? e : e.substr(0, at))
                           == name.value;
                };
                auto it = std::find_if(allow.begin(), allow.end(), same_tool);
                if (it == allow.end()) {
                    allow.push_back(std::move(entry));
                    grant_save = save_record(m);
                } else if (*it != entry) {
                    *it = std::move(entry);   // re-granted for a new definition
                    grant_save = save_record(m);
                }
            }
            m.s.status = name.value + ": always allowed (persists \xc2\xb7 "
                         "Shift+Tab profile cycle resets)";
            m.s.status_until = m.now
                             + std::chrono::seconds{4};
            with_live_tool(m, id, [&, shown = m.d.pending_permission->def_hash](ToolUse& tc) {
                tc.status = ToolUse::Approved{tc.started_at()};
                tc.approved_def_hash = shown;   // see PermissionApprove
            });
            m.d.pending_permission.reset();
            return Cmd::batch(std::move(grant_save),
                              cmd::kick_pending_tools(m));
        },
    }, tm);
}

} // namespace agentty::app::detail
