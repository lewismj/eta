#pragma once

/// Types and helpers shared by the eta CLI translation units (main_eta.cpp and cli_*.cpp).

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "eta/package/discovery.h"
#include "eta/package/manifest.h"
#include "eta/package/resolver.h"

namespace fs = std::filesystem;

namespace eta::cli {

enum class ProjectKind {
    Bin,
    Lib,
};

struct RunOptions {
    std::string profile{"release"};
    std::optional<std::string> bin_name;
    std::optional<std::string> example_name;
    std::optional<fs::path> input_path;
    std::optional<std::string> prof_mode;
    std::optional<std::string> prof_format;
    std::optional<std::uint32_t> prof_hz;
    std::optional<fs::path> prof_out;
    std::vector<std::string> program_args;
    bool strict_shadows{false};
};

struct BuildOptions {
    std::string profile{"release"};
    std::optional<std::string> bin_name;
};

struct PathCommandOptions {
    std::optional<std::string> filter;
};

struct VendorOptions {
    std::optional<fs::path> target;
};

struct InstallOptions {
    std::optional<std::string> package;
    bool global{false};
};

struct AddOptions {
    std::string name;
    bool dev{false};
    std::optional<fs::path> path;
    std::optional<std::string> git;
    std::optional<std::string> rev;
    std::optional<std::string> tarball;
    std::optional<std::string> sha256;
};

struct CleanOptions {
    bool all{false};
};

enum class SelectionCommandMode {
    Aggregate,
    SingleTarget,
};

enum class InvocationContext {
    StandalonePackage,
    WorkspaceRoot,
    WorkspaceMember,
    WorkspaceNonMember,
};

struct WorkspaceCliOptions {
    bool workspace{false};
    std::vector<std::string> packages;
    std::vector<std::string> exclude;
    std::optional<fs::path> manifest_path;
};

struct ParsedCommandArgs {
    WorkspaceCliOptions workspace;
    std::vector<std::string> command_args;
};

struct SelectedPackageTarget {
    std::string name;
    fs::path manifest_path;
    fs::path package_root;
};

struct ResolvedCommandTargets {
    InvocationContext context{InvocationContext::StandalonePackage};
    std::optional<fs::path> workspace_manifest_path;
    fs::path workspace_root;
    std::vector<SelectedPackageTarget> selected;
};

struct ExternalResult {
    int exit_code{1};
    std::string output;
};

struct ResolvedProjectState {
    fs::path manifest_path;
    fs::path project_root;
    fs::path lockfile_root;
    fs::path modules_root;
    std::optional<fs::path> workspace_root;
    eta::package::Manifest manifest;
    eta::package::ResolvedGraph graph;
    eta::package::Lockfile lockfile;
};

using ErrorResult = std::expected<void, std::string>;
template <typename T>
using CliResult = std::expected<T, std::string>;

// cli_util.cpp: String, path and filesystem helpers shared by the CLI.
[[nodiscard]] bool is_valid_package_name(std::string_view value);
[[nodiscard]] std::string module_name_from_package_name(std::string_view package_name);
[[nodiscard]] std::string lower_ascii(std::string_view text);
[[nodiscard]] fs::path canonicalize_path(const fs::path& path);
[[nodiscard]] std::string path_key(const fs::path& path);
[[nodiscard]] bool is_path_within(const fs::path& root, const fs::path& candidate);
[[nodiscard]] uint64_t fnv1a_64(std::string_view data);
[[nodiscard]] std::string to_hex_u64(uint64_t value);
[[nodiscard]] bool looks_like_hex(std::string_view value, std::size_t expected_len);
[[nodiscard]] std::optional<fs::path> home_dir();
[[nodiscard]] CliResult<fs::path> eta_cache_modules_root();
ErrorResult ensure_directory(const fs::path& dir);
ErrorResult remove_path_if_exists(const fs::path& path);
ErrorResult copy_directory_tree(const fs::path& source, const fs::path& destination);
ErrorResult materialize_from_cache(const fs::path& cache_dir, const fs::path& destination);
[[nodiscard]] CliResult<std::size_t> parse_depth(std::string_view value);
[[nodiscard]] CliResult<std::uint32_t> parse_positive_u32(std::string_view value,
                                                          std::string_view field_name);

// cli_process.cpp: Launching external processes (etai, etac, git, tar, ...).
CliResult<int> run_external(const fs::path& program,
                            const std::vector<std::string>& args,
                            const std::optional<fs::path>& cwd = std::nullopt);
CliResult<ExternalResult> run_external_capture(
    const fs::path& program,
    const std::vector<std::string>& args,
    const std::optional<fs::path>& cwd = std::nullopt);

// cli_usage.cpp: Version and usage text for each subcommand.
void print_version();
void print_usage(const char* program);
void print_new_usage(const char* program);
void print_init_usage(const char* program);
void print_tree_usage(const char* program);
void print_run_usage(const char* program);
void print_prof_usage(const char* program);
void print_add_usage(const char* program);
void print_remove_usage(const char* program);
void print_update_usage(const char* program);
void print_build_usage(const char* program);
void print_path_command_usage(const char* program,
                              std::string_view command,
                              std::string_view dir);
void print_vendor_usage(const char* program);
void print_install_usage(const char* program);
void print_clean_usage(const char* program);

// cli_workspace.cpp: Manifest discovery, workspace selection flags and target resolution.
CliResult<eta::package::ManifestDiscovery> discover_manifest_context(const fs::path& start_dir);
CliResult<ParsedCommandArgs> parse_workspace_command_args(std::string_view command,
                                                          const std::vector<std::string>& args);
CliResult<ResolvedCommandTargets> resolve_command_targets(std::string_view command,
                                                          const fs::path& cwd,
                                                          const WorkspaceCliOptions& options,
                                                          SelectionCommandMode mode,
                                                          std::string_view missing_manifest_message);

// cli_scaffold.cpp: Project scaffolding for `eta new` and `eta init`.
CliResult<int> scaffold_project(const fs::path& project_root,
                                std::string package_name,
                                ProjectKind kind,
                                bool in_place);

// cli_packages.cpp: Dependency fetching, cache materialization and project state resolution.
ErrorResult materialize_modules_from_lockfile(const fs::path& lockfile_root,
                                              const eta::package::Lockfile& lockfile,
                                              const fs::path& modules_root);
CliResult<ResolvedProjectState> resolve_project_state(const fs::path& manifest_path,
                                                      bool include_dev_dependencies,
                                                      bool write_lockfile,
                                                      bool materialize_modules);
[[nodiscard]] fs::path workspace_member_target_profile_root(const fs::path& workspace_root,
                                                            std::string_view profile,
                                                            std::string_view member_name);
[[nodiscard]] fs::path selected_package_target_profile_root(const ResolvedProjectState& state,
                                                            std::string_view profile);
[[nodiscard]] fs::path lockfile_package_release_root(
    const ResolvedProjectState& state,
    const eta::package::LockfilePackage& package);
[[nodiscard]] std::string display_source(const eta::package::ResolvedPackage& package,
                                         const fs::path& root_dir);

// cli_module_paths.cpp: Module path assembly, tool discovery and compile/test drivers.
void print_tree_children(std::ostream& out,
                         const eta::package::ResolvedPackage& node,
                         const fs::path& root_dir,
                         const std::unordered_map<std::string, const eta::package::ResolvedPackage*>& by_name,
                         std::string prefix,
                         std::size_t depth,
                         std::optional<std::size_t> max_depth);
[[nodiscard]] std::string join_module_path_entries(const std::vector<fs::path>& entries);
std::vector<fs::path> module_entries_from_lockfile(const ResolvedProjectState& state,
                                                   const char* argv0);
CliResult<int> run_with_etai(const char* argv0,
                             const std::vector<std::string>& args,
                             const std::optional<fs::path>& cwd = std::nullopt);
CliResult<int> run_with_eta_prof(const char* argv0,
                                 const std::vector<std::string>& args,
                                 const std::optional<fs::path>& cwd = std::nullopt);
CliResult<int> compile_project_sources(const char* argv0,
                                       const ResolvedProjectState& state,
                                       const BuildOptions& options);
CliResult<int> run_project_suite(const char* argv0,
                                 const ResolvedProjectState& state,
                                 const fs::path& suite_root,
                                 const PathCommandOptions& options);

// cli_cmd_project.cpp: Subcommands: new, init, tree, build, test, bench.
CliResult<int> command_new(const char* program,
                           const std::vector<std::string>& args,
                           const fs::path& cwd);
CliResult<int> command_init(const char* program,
                            const std::vector<std::string>& args,
                            const fs::path& cwd);
CliResult<int> command_tree(const char* program,
                            const std::vector<std::string>& args,
                            const fs::path& cwd);
CliResult<int> command_build(const char* program,
                             const char* argv0,
                             const std::vector<std::string>& args,
                             const fs::path& cwd);
CliResult<int> command_test(const char* program,
                            const char* argv0,
                            const std::vector<std::string>& args,
                            const fs::path& cwd);
CliResult<int> command_bench(const char* program,
                             const char* argv0,
                             const std::vector<std::string>& args,
                             const fs::path& cwd);

// cli_cmd_run.cpp: Subcommands: run, prof.
CliResult<int> command_run(const char* program,
                           const char* argv0,
                           const std::vector<std::string>& args,
                           const fs::path& cwd);
CliResult<int> command_prof(const char* program,
                            const char* argv0,
                            const std::vector<std::string>& args,
                            const fs::path& cwd);

// cli_cmd_deps.cpp: Subcommands: vendor, install, add, remove, update, clean.
CliResult<int> command_vendor(const char* program,
                              const std::vector<std::string>& args,
                              const fs::path& cwd);
CliResult<int> command_install(const char* program,
                               const char* argv0,
                               const std::vector<std::string>& args,
                               const fs::path& cwd);
CliResult<int> command_add(const char* program,
                           const std::vector<std::string>& args,
                           const fs::path& cwd);
CliResult<int> command_remove(const char* program,
                              const std::vector<std::string>& args,
                              const fs::path& cwd);
CliResult<int> command_update(const char* program,
                              const std::vector<std::string>& args,
                              const fs::path& cwd);
CliResult<int> command_clean(const char* program,
                             const std::vector<std::string>& args,
                             const fs::path& cwd);

} ///< namespace eta::cli
