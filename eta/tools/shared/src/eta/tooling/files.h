#pragma once

#include <expected>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eta::tooling {
    struct NativeSnapshot;
    struct Snapshot {
        std::filesystem::path path;
        std::string bytes;
        std::string identity;
        std::shared_ptr<NativeSnapshot> native;
    };
    struct Selection { std::filesystem::path path; std::string display; };
    struct FileIssue { std::string display; std::string message; };
    struct Discovery { std::vector<Selection> files; std::vector<FileIssue> issues; };

    Discovery discover(const std::vector<std::filesystem::path> &paths,
        const std::vector<std::string> &excludes, const std::filesystem::path &root, bool mutating);
    bool glob_matches(std::string_view pattern, std::string_view path);
    std::expected<Snapshot, std::string> read_snapshot(const std::filesystem::path &path,
        bool mutating, std::size_t limit = 16 * 1024 * 1024);

    enum class CommitOutcome { NotCommitted, Committed, DurabilityUnconfirmed, Unknown };
    enum class CommitStage { BeforeRecheck, BeforeTemporary, BeforeFlush, BeforeReplace, AfterReplace };
    /// Internal failure injection; the CLI never exposes a bypass of the protocol.
    struct CommitHooks {
        std::function<void(CommitStage)> observe;
        std::function<bool(CommitStage)> fail;
        std::shared_ptr<const std::atomic_bool> cancelled;
    };
    struct CommitResult {
        bool success{false};
        CommitOutcome outcome{CommitOutcome::NotCommitted};
        std::string message;
    };
    CommitResult replace_atomically(const Snapshot &snapshot, std::string_view output, const CommitHooks &hooks = {});
    std::string unified_diff(std::string_view original, std::string_view formatted, std::string_view display);
}
