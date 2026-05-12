#pragma once

#include <filesystem>
#include <string_view>

namespace eta::util {

namespace fs = std::filesystem;

/**
 * @brief Runtime path-layout tokens used by module and sidecar discovery.
 */
struct RuntimeLayoutConfig {
    std::string_view package_build_dir{"target"};
    std::string_view package_build_profile{"release"};
    std::string_view package_source_dir{"src"};
    std::string_view workspace_state_dir{".eta"};
    std::string_view workspace_modules_dir{"modules"};
    std::string_view bundled_stdlib_dir{"stdlib"};
    std::string_view packages_dir{"packages"};
    std::string_view native_dir{"native"};
};

/**
 * @brief Default runtime layout used by resolver and sidecar wiring.
 */
inline constexpr RuntimeLayoutConfig kDefaultRuntimeLayoutConfig{};

/**
 * @brief Return the package bytecode build directory.
 */
[[nodiscard]] inline fs::path package_build_output_dir(
    const fs::path& package_root,
    const RuntimeLayoutConfig& layout = kDefaultRuntimeLayoutConfig) {
    return package_root / fs::path(layout.package_build_dir)
         / fs::path(layout.package_build_profile);
}

/**
 * @brief Return the package source directory.
 */
[[nodiscard]] inline fs::path package_source_dir(
    const fs::path& package_root,
    const RuntimeLayoutConfig& layout = kDefaultRuntimeLayoutConfig) {
    return package_root / fs::path(layout.package_source_dir);
}

/**
 * @brief Return the workspace dependency modules directory.
 */
[[nodiscard]] inline fs::path workspace_modules_root(
    const fs::path& workspace_root,
    const RuntimeLayoutConfig& layout = kDefaultRuntimeLayoutConfig) {
    return workspace_root / fs::path(layout.workspace_state_dir)
         / fs::path(layout.workspace_modules_dir);
}

/**
 * @brief Return the stdlib root located under one runtime root.
 */
[[nodiscard]] inline fs::path bundled_stdlib_root(
    const fs::path& runtime_root,
    const RuntimeLayoutConfig& layout = kDefaultRuntimeLayoutConfig) {
    return runtime_root / fs::path(layout.bundled_stdlib_dir);
}

/**
 * @brief Return the bundled stdlib root near one executable path.
 */
[[nodiscard]] inline fs::path bundled_stdlib_root_from_executable(
    const fs::path& executable_path,
    const RuntimeLayoutConfig& layout = kDefaultRuntimeLayoutConfig) {
    return bundled_stdlib_root(executable_path.parent_path().parent_path(), layout);
}

/**
 * @brief Return the bundled stdlib native package root under one runtime root.
 */
[[nodiscard]] inline fs::path bundled_stdlib_native_root(
    const fs::path& runtime_root,
    const RuntimeLayoutConfig& layout = kDefaultRuntimeLayoutConfig) {
    return runtime_root / fs::path(layout.packages_dir)
         / fs::path(layout.bundled_stdlib_dir)
         / fs::path(layout.native_dir);
}

} // namespace eta::util
