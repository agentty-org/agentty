#pragma once
// agentty::foreign — conversations from other agents (Codex, Claude Code).
//
// Their session files are read live and never written. A session shows in
// the ^J picker as a row; opening it turns it into a normal agentty thread
// with a stable id, so opening it again reuses that thread.

#include "agentty/domain/conversation.hpp"
#include "agentty/foreign/names.hpp"
#include "agentty/foreign/scan.hpp"
#include "agentty/util/io.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agentty::foreign {

// The agentty thread id a session maps to: 16 hex, stable per session.
[[nodiscard]] ThreadId thread_id_for(Source, std::string_view session_id);

// One session read whole, before it becomes a Thread.
struct Parsed {
    std::string session_id;
    std::string cwd;
    std::string title;          // the tool's own title, when it keeps one
    std::chrono::system_clock::time_point started{};
    std::chrono::system_clock::time_point updated{};
    std::vector<Message> messages;
    std::vector<Thread::CompactionRecord> compactions;
    bool skip = false;          // not a user session (subagent, SDK/ACP run)
};

struct ParseOptions {
    bool images = true;         // false: skip decoding images (header scans)
};

// Pure parsers over a whole .jsonl file's text.
[[nodiscard]] Parsed parse_codex(std::string_view jsonl, ParseOptions = {});
[[nodiscard]] Parsed parse_claude(std::string_view jsonl, ParseOptions = {});

// A parsed session as an agentty thread: stable id, provenance and the
// picker summary.
[[nodiscard]] Thread to_thread(Source, Parsed, const std::filesystem::path& file,
                               std::uint64_t size, std::int64_t mtime);

// Where each tool keeps its sessions ($CODEX_HOME, $CLAUDE_CONFIG_DIR).
[[nodiscard]] std::filesystem::path sessions_root(Source, const std::filesystem::path& home);

// Every user session on this machine. Pass the last scan back in: a file
// whose size and mtime haven't changed isn't read again.
[[nodiscard]] Scan list_sessions(Io, const Scan& previous);

// Read one session whole. nullopt when the file is gone or unreadable.
[[nodiscard]] std::optional<Thread> read_session(Io, const Thread::Import&);

// `agentty sessions`: list them, newest first. `--dump N` prints how the
// Nth one reads as an agentty thread.
int cli(Io, const std::vector<std::string>& argv);

}  // namespace agentty::foreign
