#pragma once
// DiskFiles with the trust-handoff gate on every write, rename and copy.
//
// write/edit/apply_patch/move are also gated up front on their path argument,
// which gives the model a better message; this layer is what covers replace
// and rewrite_structural, which write whatever their glob matches, and any
// writing tool added later. An ordinary path costs the gate's cheap checks.

#include "agentty/tool/util/disk_files.hpp"
#include "agentty/tool/util/handoff_gate.hpp"

#include <mcp/tools/files.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace agentty::tools::util {

class GatedFiles final : public ::mcp::tools::FileSystem {
    using FileStat = ::mcp::tools::FileStat;
    using DirEntry = ::mcp::tools::DirEntry;
    using WalkStep = ::mcp::tools::WalkStep;

public:
    [[nodiscard]] FileStat stat(const fs::path& p) const override { return disk_.stat(p); }
    [[nodiscard]] fs::path canonical(const fs::path& p, std::error_code& ec) const override {
        return disk_.canonical(p, ec);
    }
    [[nodiscard]] fs::path read_symlink(const fs::path& p, std::error_code& ec) const override {
        return disk_.read_symlink(p, ec);
    }
    [[nodiscard]] fs::path cwd() const override { return disk_.cwd(); }
    [[nodiscard]] fs::path scratch_dir() const override { return disk_.scratch_dir(); }
    [[nodiscard]] std::optional<std::string> read(const fs::path& p) const override {
        return disk_.read(p);
    }
    [[nodiscard]] std::optional<std::string> read_prefix(const fs::path& p,
                                                         std::size_t n) const override {
        return disk_.read_prefix(p, n);
    }
    [[nodiscard]] std::string write_atomic(const fs::path& p, std::string_view content) override {
        if (auto v = handoff::check(p.string(), "write"); !v.allowed) return v.reason;
        return disk_.write_atomic(p, content);
    }
    [[nodiscard]] std::vector<DirEntry> list(const fs::path& dir,
                                             std::error_code& ec) const override {
        return disk_.list(dir, ec);
    }
    bool walk(const fs::path& root,
              const std::function<WalkStep(const DirEntry&)>& visit) const override {
        return disk_.walk(root, visit);
    }
    bool create_directories(const fs::path& p, std::error_code& ec) override {
        return disk_.create_directories(p, ec);
    }
    bool remove(const fs::path& p, std::error_code& ec) override { return disk_.remove(p, ec); }
    std::uintmax_t remove_all(const fs::path& p, std::error_code& ec) override {
        return disk_.remove_all(p, ec);
    }
    void rename(const fs::path& from, const fs::path& to, std::error_code& ec) override {
        if (!allowed(to, ec)) return;
        disk_.rename(from, to, ec);
    }
    void copy(const fs::path& from, const fs::path& to, std::error_code& ec) override {
        if (!allowed(to, ec)) return;
        disk_.copy(from, to, ec);
    }

private:
    [[nodiscard]] static bool allowed(const fs::path& to, std::error_code& ec) {
        if (handoff::check(to.string(), "move").allowed) return true;
        ec = std::make_error_code(std::errc::permission_denied);
        return false;
    }
    DiskFiles disk_;
};

}  // namespace agentty::tools::util
