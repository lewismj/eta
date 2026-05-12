#include "eta/util/path.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace eta::util {

namespace {

[[nodiscard]] std::string lower_ascii(std::string value) {
    std::transform(value.begin(),
                   value.end(),
                   value.begin(),
                   [](const char c) {
                       return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                   });
    return value;
}

} // namespace

fs::path canonicalize_path(const fs::path& path) {
    std::error_code ec;
    const auto canonical = fs::weakly_canonical(path, ec);
    if (!ec) return canonical;
    return path.lexically_normal();
}

std::string canonical_path_key(const fs::path& path) {
    auto normalized = canonicalize_path(path).string();
#if defined(_WIN32)
    for (char& ch : normalized) {
        if (ch == '/') ch = '\\';
    }
    return lower_ascii(std::move(normalized));
#else
    return normalized;
#endif
}

std::optional<fs::path> current_executable_path() {
#if defined(_WIN32)
    std::array<wchar_t, 4096u> buffer{};
    const DWORD len = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len > 0 && len < buffer.size()) {
        return canonicalize_path(fs::path(buffer.data()));
    }
    return std::nullopt;
#elif defined(__linux__)
    std::error_code ec;
    const auto proc_self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return canonicalize_path(proc_self);
    return std::nullopt;
#elif defined(__APPLE__)
    std::array<char, 4096u> buffer{};
    uint32_t size = static_cast<uint32_t>(buffer.size());
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
        return canonicalize_path(fs::path(buffer.data()));
    }
    if (size == 0u) return std::nullopt;
    std::string dynamic(size, '\0');
    if (_NSGetExecutablePath(dynamic.data(), &size) == 0) {
        return canonicalize_path(fs::path(dynamic.c_str()));
    }
    return std::nullopt;
#else
    return std::nullopt;
#endif
}

fs::path sibling_executable_path(std::string_view basename) {
    std::string executable_name(basename);
#if defined(_WIN32)
    if (fs::path(executable_name).extension().empty()) {
        executable_name += ".exe";
    }
#endif

    if (auto self = current_executable_path(); self.has_value()) {
        return canonicalize_path(self->parent_path() / fs::path(executable_name));
    }
    return fs::path(std::move(executable_name));
}

} // namespace eta::util
