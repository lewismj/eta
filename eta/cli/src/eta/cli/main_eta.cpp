#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "eta/package/manifest.h"
#include "eta/package/resolver.h"

namespace fs = std::filesystem;

namespace {

enum class ProjectKind {
    Bin,
    Lib,
};

struct RunOptions {
    std::string profile{"release"};
    std::optional<std::string> bin_name;
    std::optional<std::string> example_name;
    std::optional<fs::path> input_path;
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

struct ExternalResult {
    int exit_code{1};
    std::string output;
};

struct ResolvedProjectState {
    fs::path manifest_path;
    fs::path project_root;
    eta::package::Manifest manifest;
    eta::package::ResolvedGraph graph;
    eta::package::Lockfile lockfile;
};

using ErrorResult = std::expected<void, std::string>;
template <typename T>
using CliResult = std::expected<T, std::string>;

[[nodiscard]] bool is_valid_package_name(std::string_view value) {
    static const std::regex kPattern("^[a-z][a-z0-9_-]{0,63}$");
    return std::regex_match(value.begin(), value.end(), kPattern);
}

[[nodiscard]] std::string module_name_from_package_name(std::string_view package_name) {
    std::string module_name(package_name);
    std::replace(module_name.begin(), module_name.end(), '-', '_');
    return module_name;
}

[[nodiscard]] std::string lower_ascii(std::string_view text) {
    std::string lowered(text);
    std::transform(lowered.begin(),
                   lowered.end(),
                   lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered;
}

[[nodiscard]] fs::path canonicalize_path(const fs::path& path) {
    std::error_code ec;
    auto canonical = fs::weakly_canonical(path, ec);
    if (!ec) return canonical;
    return path.lexically_normal();
}

[[nodiscard]] std::string path_key(const fs::path& path) {
    auto normalized = canonicalize_path(path).generic_string();
#ifdef _WIN32
    return lower_ascii(normalized);
#else
    return normalized;
#endif
}

[[nodiscard]] uint64_t fnv1a_64(std::string_view data) {
    constexpr uint64_t kOffset = 1469598103934665603ULL;
    constexpr uint64_t kPrime = 1099511628211ULL;
    uint64_t value = kOffset;
    for (const unsigned char c : data) {
        value ^= static_cast<uint64_t>(c);
        value *= kPrime;
    }
    return value;
}

[[nodiscard]] std::string to_hex_u64(uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::nouppercase;
    out.width(16);
    out.fill('0');
    out << value;
    return out.str();
}

[[nodiscard]] bool looks_like_hex(std::string_view value, std::size_t expected_len) {
    if (value.size() != expected_len) return false;
    for (const char c : value) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

[[nodiscard]] std::optional<fs::path> home_dir() {
#ifdef _WIN32
    const char* user_profile = std::getenv("USERPROFILE");
    if (user_profile != nullptr && user_profile[0] != '\0') {
        return fs::path(user_profile);
    }
    const char* home_drive = std::getenv("HOMEDRIVE");
    const char* home_path = std::getenv("HOMEPATH");
    if (home_drive != nullptr && home_path != nullptr
        && home_drive[0] != '\0' && home_path[0] != '\0') {
        return fs::path(std::string(home_drive) + std::string(home_path));
    }
#else
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') return fs::path(home);
#endif
    return std::nullopt;
}

[[nodiscard]] CliResult<fs::path> eta_cache_modules_root() {
    auto home = home_dir();
    if (!home.has_value()) {
        return std::unexpected("failed to resolve user home directory");
    }
    return canonicalize_path(*home / ".eta" / "cache" / "modules");
}

ErrorResult ensure_directory(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        return std::unexpected("failed to create directory '" + dir.string() + "': " + ec.message());
    }
    return {};
}

ErrorResult remove_path_if_exists(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return {};
    if (ec) return std::unexpected("failed to stat path '" + path.string() + "': " + ec.message());

    fs::remove_all(path, ec);
    if (ec) {
        return std::unexpected("failed to remove path '" + path.string() + "': " + ec.message());
    }
    return {};
}

ErrorResult copy_directory_tree(const fs::path& source, const fs::path& destination) {
    std::error_code ec;
    if (!fs::is_directory(source, ec) || ec) {
        return std::unexpected("source directory does not exist: " + source.string());
    }

    if (auto rm = remove_path_if_exists(destination); !rm) return rm;
    if (auto mk = ensure_directory(destination.parent_path()); !mk) return mk;

    fs::copy(source,
             destination,
             fs::copy_options::recursive
                 | fs::copy_options::copy_symlinks
                 | fs::copy_options::overwrite_existing,
             ec);
    if (ec) {
        return std::unexpected("failed to copy '" + source.string() + "' to '"
                               + destination.string() + "': " + ec.message());
    }
    return {};
}

ErrorResult materialize_from_cache(const fs::path& cache_dir, const fs::path& destination) {
    if (auto rm = remove_path_if_exists(destination); !rm) return rm;
    if (auto mk = ensure_directory(destination.parent_path()); !mk) return mk;

    std::error_code ec;
    fs::create_directory_symlink(cache_dir, destination, ec);
    if (!ec) return {};

    return copy_directory_tree(cache_dir, destination);
}

#ifdef _WIN32
[[nodiscard]] std::wstring quote_windows_arg(const std::wstring& arg) {
    if (arg.empty()) return L"\"\"";

    bool needs_quotes = false;
    for (const wchar_t ch : arg) {
        if (ch == L' ' || ch == L'\t' || ch == L'"') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) return arg;

    std::wstring out;
    out.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        if (backslashes > 0) out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(ch);
    }
    if (backslashes > 0) out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

[[nodiscard]] std::wstring build_windows_command_line(const fs::path& program,
                                                      const std::vector<std::string>& args) {
    std::wstring command = quote_windows_arg(program.wstring());
    for (const auto& arg : args) {
        command.push_back(L' ');
        command += quote_windows_arg(fs::path(arg).wstring());
    }
    return command;
}

[[nodiscard]] std::string win32_error_message(DWORD error_code) {
    LPSTR buffer = nullptr;
    const DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error_code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buffer),
        0,
        nullptr);
    std::string message;
    if (size > 0 && buffer != nullptr) {
        message.assign(buffer, buffer + size);
        while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
            message.pop_back();
        }
    } else {
        message = "Win32 error " + std::to_string(static_cast<unsigned long long>(error_code));
    }
    if (buffer != nullptr) LocalFree(buffer);
    return message;
}
#else
std::vector<char*> make_exec_argv(const fs::path& program,
                                  const std::vector<std::string>& args,
                                  std::vector<std::string>& storage) {
    storage.clear();
    storage.reserve(args.size() + 1u);
    storage.push_back(program.string());
    storage.insert(storage.end(), args.begin(), args.end());

    std::vector<char*> argv;
    argv.reserve(storage.size() + 1u);
    for (auto& entry : storage) argv.push_back(entry.data());
    argv.push_back(nullptr);
    return argv;
}
#endif

CliResult<ExternalResult> run_external_impl(const fs::path& program,
                                            const std::vector<std::string>& args,
                                            const std::optional<fs::path>& cwd,
                                            bool capture_output) {
#ifdef _WIN32
    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info{};

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (capture_output) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = nullptr;

        if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
            return std::unexpected("CreatePipe failed: " + win32_error_message(GetLastError()));
        }
        (void)SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
        startup_info.dwFlags |= STARTF_USESTDHANDLES;
        startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup_info.hStdOutput = write_pipe;
        startup_info.hStdError = write_pipe;
    }

    std::wstring command_line = build_windows_command_line(program, args);
    std::vector<wchar_t> mutable_cmd(command_line.begin(), command_line.end());
    mutable_cmd.push_back(L'\0');

    std::wstring cwd_w;
    LPCWSTR cwd_ptr = nullptr;
    if (cwd.has_value()) {
        cwd_w = cwd->wstring();
        cwd_ptr = cwd_w.c_str();
    }

    const BOOL created = CreateProcessW(
        nullptr,
        mutable_cmd.data(),
        nullptr,
        nullptr,
        capture_output ? TRUE : FALSE,
        0,
        nullptr,
        cwd_ptr,
        &startup_info,
        &process_info);
    if (capture_output && write_pipe != nullptr) {
        CloseHandle(write_pipe);
        write_pipe = nullptr;
    }
    if (!created) {
        if (read_pipe != nullptr) CloseHandle(read_pipe);
        return std::unexpected("failed to execute '" + program.string()
                               + "': " + win32_error_message(GetLastError()));
    }

    std::string output;
    if (capture_output && read_pipe != nullptr) {
        std::array<char, 4096> buffer{};
        while (true) {
            DWORD bytes_read = 0;
            const BOOL ok =
                ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr);
            if (!ok || bytes_read == 0) break;
            output.append(buffer.data(), buffer.data() + bytes_read);
        }
        CloseHandle(read_pipe);
    }

    WaitForSingleObject(process_info.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process_info.hProcess, &exit_code);
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);

    ExternalResult result;
    result.exit_code = static_cast<int>(exit_code);
    result.output = std::move(output);
    return result;
#else
    int pipe_fds[2]{-1, -1};
    if (capture_output) {
        if (pipe(pipe_fds) != 0) {
            return std::unexpected("pipe() failed for '" + program.string() + "'");
        }
    }

    std::vector<std::string> argv_storage;
    auto exec_argv = make_exec_argv(program, args, argv_storage);

    const pid_t pid = fork();
    if (pid < 0) {
        if (capture_output) {
            close(pipe_fds[0]);
            close(pipe_fds[1]);
        }
        return std::unexpected("failed to fork for '" + program.string() + "'");
    }

    if (pid == 0) {
        if (cwd.has_value()) {
            if (chdir(cwd->c_str()) != 0) _exit(127);
        }
        if (capture_output) {
            close(pipe_fds[0]);
            dup2(pipe_fds[1], STDOUT_FILENO);
            dup2(pipe_fds[1], STDERR_FILENO);
            close(pipe_fds[1]);
        }
        execvp(program.c_str(), exec_argv.data());
        _exit(127);
    }

    if (capture_output) {
        close(pipe_fds[1]);
    }

    std::string output;
    if (capture_output) {
        std::array<char, 4096> buffer{};
        while (true) {
            const ssize_t n = read(pipe_fds[0], buffer.data(), buffer.size());
            if (n <= 0) break;
            output.append(buffer.data(), buffer.data() + static_cast<std::size_t>(n));
        }
        close(pipe_fds[0]);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return std::unexpected("waitpid failed for '" + program.string() + "': "
                               + std::string(std::strerror(errno)));
    }

    ExternalResult result;
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    } else {
        result.exit_code = 1;
    }
    result.output = std::move(output);
    return result;
#endif
}

CliResult<int> run_external(const fs::path& program,
                            const std::vector<std::string>& args,
                            const std::optional<fs::path>& cwd = std::nullopt) {
    auto result = run_external_impl(program, args, cwd, false);
    if (!result) return std::unexpected(result.error());
    return result->exit_code;
}

CliResult<ExternalResult> run_external_capture(
    const fs::path& program,
    const std::vector<std::string>& args,
    const std::optional<fs::path>& cwd = std::nullopt) {
    return run_external_impl(program, args, cwd, true);
}

void print_usage(const char* program) {
    std::cerr
        << "Usage: " << program << " <command> [options]\n"
        << "\n"
        << "Commands:\n"
        << "  eta new <name> [--bin|--lib]\n"
        << "  eta init [--bin|--lib]\n"
        << "  eta tree [--depth N]\n"
        << "  eta run [--profile <release|debug>] [--strict-shadows] [--bin NAME] [--example NAME] [file.eta] [-- args...]\n"
        << "  eta add <pkg> [--path DIR|--git URL --rev SHA|--tarball PATH --sha256 HEX] [--dev]\n"
        << "  eta remove <pkg>\n"
        << "  eta update [<pkg>...]\n"
        << "  eta build [--profile <release|debug>] [--bin NAME]\n"
        << "  eta test [filter]\n"
        << "  eta bench [filter]\n"
        << "  eta vendor [--target DIR]\n"
        << "  eta install [<pkg>] [--global]\n"
        << "  eta clean [--all]\n"
        << "\n"
        << "Use `eta <command> --help` for command-specific options.\n";
}

void print_new_usage(const char* program) {
    std::cerr << "Usage: " << program << " new <name> [--bin|--lib]\n";
}

void print_init_usage(const char* program) {
    std::cerr << "Usage: " << program << " init [--bin|--lib]\n";
}

void print_tree_usage(const char* program) {
    std::cerr << "Usage: " << program << " tree [--depth N]\n";
}

void print_run_usage(const char* program) {
    std::cerr
        << "Usage: " << program
        << " run [--profile <release|debug>] [--strict-shadows] [--bin NAME] [--example NAME] [file.eta] [-- args...]\n";
}

void print_add_usage(const char* program) {
    std::cerr
        << "Usage: " << program
        << " add <pkg> [--path DIR|--git URL --rev SHA|--tarball PATH --sha256 HEX] [--dev]\n";
}

void print_remove_usage(const char* program) {
    std::cerr << "Usage: " << program << " remove <pkg>\n";
}

void print_update_usage(const char* program) {
    std::cerr << "Usage: " << program << " update [<pkg>...]\n";
}

void print_build_usage(const char* program) {
    std::cerr << "Usage: " << program << " build [--profile <release|debug>] [--bin NAME]\n";
}

void print_path_command_usage(const char* program,
                              std::string_view command,
                              std::string_view dir) {
    std::cerr << "Usage: " << program << " " << command << " [filter]\n";
    std::cerr << "If no filter is provided, eta " << command << " runs all tests under "
              << dir << "/\n";
}

void print_vendor_usage(const char* program) {
    std::cerr << "Usage: " << program << " vendor [--target DIR]\n";
}

void print_install_usage(const char* program) {
    std::cerr << "Usage: " << program << " install [<pkg>] [--global]\n";
}

void print_clean_usage(const char* program) {
    std::cerr << "Usage: " << program << " clean [--all]\n";
}

[[nodiscard]] std::optional<fs::path> find_manifest_path(fs::path start_dir) {
    start_dir = canonicalize_path(start_dir);
    while (true) {
        const auto candidate = start_dir / "eta.toml";
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec) && !ec) {
            return canonicalize_path(candidate);
        }
        const auto parent = start_dir.parent_path();
        if (parent.empty() || parent == start_dir) break;
        start_dir = parent;
    }
    return std::nullopt;
}

ErrorResult write_text_file(const fs::path& path, std::string_view content) {
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    if (ec) {
        return std::unexpected("failed to create directory '" + path.parent_path().string()
                               + "': " + ec.message());
    }

    std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out) {
        return std::unexpected("failed to open '" + path.string() + "' for writing");
    }
    out << content;
    if (!out.good()) {
        return std::unexpected("failed to write '" + path.string() + "'");
    }
    return {};
}

[[nodiscard]] std::string render_manifest(std::string_view package_name) {
    std::ostringstream out;
    out << "[package]\n"
        << "name = \"" << package_name << "\"\n"
        << "version = \"0.1.0\"\n"
        << "license = \"MIT\"\n"
        << "\n"
        << "[compatibility]\n"
        << "eta = \">=0.6, <0.8\"\n"
        << "\n"
        << "[dependencies]\n";
    return out.str();
}

[[nodiscard]] std::string render_module_source(std::string_view module_name, ProjectKind kind) {
    std::ostringstream out;
    if (kind == ProjectKind::Lib) {
        out << "(module " << module_name << "\n"
            << "  (export square)\n"
            << "  (begin\n"
            << "    (define (square x)\n"
            << "      (* x x))))\n";
    } else {
        out << "(module " << module_name << "\n"
            << "  (import std.io)\n"
            << "  (begin\n"
            << "    (println \"Hello from " << module_name << "\")))\n";
    }
    return out.str();
}

[[nodiscard]] std::string render_smoke_test(std::string_view module_name, ProjectKind kind) {
    std::ostringstream out;
    if (kind == ProjectKind::Lib) {
        out << "(module " << module_name << ".tests.smoke\n"
            << "  (import std.test " << module_name << ")\n"
            << "  (begin\n"
            << "    (define suite\n"
            << "      (make-group \"" << module_name << "\"\n"
            << "        (list\n"
            << "          (make-test \"square\"\n"
            << "            (lambda ()\n"
            << "              (assert-equal 4 (square 2)))))))\n"
            << "    (print-tap (run suite))))\n";
    } else {
        out << "(module " << module_name << ".tests.smoke\n"
            << "  (import std.test)\n"
            << "  (begin\n"
            << "    (define suite\n"
            << "      (make-group \"" << module_name << "\"\n"
            << "        (list\n"
            << "          (make-test \"smoke\"\n"
            << "            (lambda ()\n"
            << "              (assert-true #t))))))\n"
            << "    (print-tap (run suite))))\n";
    }
    return out.str();
}

[[nodiscard]] std::string render_readme(std::string_view package_name, ProjectKind kind) {
    std::ostringstream out;
    out << "# " << package_name << "\n"
        << "\n";
    if (kind == ProjectKind::Lib) {
        out << "Eta library package scaffolded by `eta new`.\n";
    } else {
        out << "Eta executable package scaffolded by `eta new`.\n";
    }
    return out.str();
}

CliResult<int> scaffold_project(const fs::path& project_root,
                                std::string package_name,
                                ProjectKind kind,
                                bool in_place) {
    std::error_code ec;
    if (!in_place) {
        if (fs::exists(project_root, ec) && !ec) {
            return std::unexpected("destination already exists: " + project_root.string());
        }
        fs::create_directories(project_root, ec);
        if (ec) {
            return std::unexpected("failed to create project directory '" + project_root.string()
                                   + "': " + ec.message());
        }
    } else {
        if (fs::is_regular_file(project_root / "eta.toml", ec) && !ec) {
            return std::unexpected("eta.toml already exists in " + project_root.string());
        }
    }

    const std::string module_name = module_name_from_package_name(package_name);
    const auto source_path = project_root / "src" / (module_name + ".eta");
    const auto test_path = project_root / "tests" / "smoke.test.eta";

    if (auto res = write_text_file(project_root / "eta.toml", render_manifest(package_name)); !res) {
        return std::unexpected(res.error());
    }
    if (auto res = write_text_file(source_path, render_module_source(module_name, kind)); !res) {
        return std::unexpected(res.error());
    }
    if (auto res = write_text_file(test_path, render_smoke_test(module_name, kind)); !res) {
        return std::unexpected(res.error());
    }
    if (auto res = write_text_file(project_root / ".gitignore", ".eta/\n"); !res) {
        return std::unexpected(res.error());
    }
    if (auto res = write_text_file(project_root / "README.md", render_readme(package_name, kind)); !res) {
        return std::unexpected(res.error());
    }

    std::cout << (in_place ? "initialized " : "created ") << project_root.string() << "\n";
    return 0;
}

[[nodiscard]] CliResult<std::size_t> parse_depth(std::string_view value) {
    if (value.empty()) return std::unexpected("depth must be a non-negative integer");
    std::size_t parsed = 0;
    for (const char c : value) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return std::unexpected("depth must be a non-negative integer");
        }
        parsed = parsed * 10u + static_cast<std::size_t>(c - '0');
    }
    return parsed;
}

[[nodiscard]] std::string dependency_source_id(const eta::package::ManifestDependency& dependency) {
    switch (dependency.kind) {
        case eta::package::ManifestDependencyKind::Path:
            return {};
        case eta::package::ManifestDependencyKind::Git:
            return "git+" + dependency.git + "#" + dependency.rev;
        case eta::package::ManifestDependencyKind::Tarball:
            return "tarball+" + dependency.tarball + "#sha256=" + dependency.sha256;
    }
    return {};
}

[[nodiscard]] std::string cache_key_from_source(std::string_view source) {
    return to_hex_u64(fnv1a_64(source));
}

CliResult<std::string> compute_sha256_file(const fs::path& file_path) {
#ifdef _WIN32
    auto result = run_external_capture("certutil",
                                       {"-hashfile", file_path.string(), "SHA256"});
    if (!result) return std::unexpected(result.error());
    if (result->exit_code != 0) {
        return std::unexpected("certutil failed while hashing " + file_path.string());
    }

    std::istringstream in(result->output);
    std::string line;
    while (std::getline(in, line)) {
        std::string hex_line;
        for (const char c : line) {
            if (std::isxdigit(static_cast<unsigned char>(c))) {
                hex_line.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
        }
        if (hex_line.size() == 64u) {
            return hex_line;
        }
    }
    return std::unexpected("failed to parse SHA256 digest from certutil output");
#else
    auto parse_sha_output = [](std::string_view text) -> std::optional<std::string> {
        std::istringstream in(std::string(text));
        std::string token;
        in >> token;
        if (!looks_like_hex(token, 64u)) return std::nullopt;
        std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return token;
    };

    if (auto result = run_external_capture("sha256sum", {file_path.string()}); result) {
        if (result->exit_code == 0) {
            if (auto parsed = parse_sha_output(result->output)) return *parsed;
        }
    }
    if (auto result = run_external_capture("shasum", {"-a", "256", file_path.string()}); result) {
        if (result->exit_code == 0) {
            if (auto parsed = parse_sha_output(result->output)) return *parsed;
        }
    }
    return std::unexpected("failed to compute SHA256 for " + file_path.string());
#endif
}

CliResult<fs::path> resolve_tarball_archive_path(const eta::package::ManifestDependency& dependency,
                                                 const fs::path& owner_root,
                                                 const fs::path& cache_root) {
    const std::string source = dependency.tarball;
    auto starts_with = [&](std::string_view prefix) {
        return source.rfind(prefix, 0) == 0;
    };

    if (starts_with("http://") || starts_with("https://")) {
        fs::path downloads_dir = cache_root / "downloads";
        if (auto mk = ensure_directory(downloads_dir); !mk) return std::unexpected(mk.error());
        fs::path archive_path =
            downloads_dir / ("tarball-" + cache_key_from_source(dependency_source_id(dependency)) + ".tar.gz");
        auto download_result = run_external("curl",
                                            {"-L", "--fail", "-o", archive_path.string(), source});
        if (!download_result) return std::unexpected(download_result.error());
        if (*download_result != 0) {
            return std::unexpected("failed to download tarball: " + source);
        }
        return archive_path;
    }

    if (starts_with("file://")) {
        fs::path file_path = fs::path(source.substr(7u));
        return canonicalize_path(file_path);
    }

    fs::path archive_path = fs::path(source);
    if (!archive_path.is_absolute()) {
        archive_path = owner_root / archive_path;
    }
    return canonicalize_path(archive_path);
}

CliResult<fs::path> ensure_git_cache(const eta::package::ManifestDependency& dependency,
                                     const fs::path& cache_root) {
    const std::string source_id = dependency_source_id(dependency);
    const fs::path cache_dir = cache_root / ("git-" + cache_key_from_source(source_id));
    std::error_code ec;
    if (fs::is_regular_file(cache_dir / "eta.toml", ec) && !ec) {
        return cache_dir;
    }

    if (auto mk = ensure_directory(cache_root); !mk) return std::unexpected(mk.error());

    const auto stamp = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path tmp_dir = cache_root / ("tmp-git-" + stamp);
    if (auto rm = remove_path_if_exists(tmp_dir); !rm) return std::unexpected(rm.error());

    auto clone_result = run_external(
        "git", {"clone", "--quiet", "--no-checkout", dependency.git, tmp_dir.string()});
    if (!clone_result) return std::unexpected(clone_result.error());
    if (*clone_result != 0) {
        (void)remove_path_if_exists(tmp_dir);
        return std::unexpected("git clone failed for dependency '" + dependency.name + "'");
    }

    auto checkout_result = run_external(
        "git", {"-C", tmp_dir.string(), "checkout", "--quiet", "--detach", dependency.rev});
    if (!checkout_result) return std::unexpected(checkout_result.error());
    if (*checkout_result != 0) {
        (void)remove_path_if_exists(tmp_dir);
        return std::unexpected("git checkout failed for dependency '" + dependency.name + "'");
    }

    if (!fs::is_regular_file(tmp_dir / "eta.toml", ec) || ec) {
        (void)remove_path_if_exists(tmp_dir);
        return std::unexpected("git dependency does not contain eta.toml at repository root: "
                               + dependency.git);
    }

    if (auto rm = remove_path_if_exists(cache_dir); !rm) {
        (void)remove_path_if_exists(tmp_dir);
        return std::unexpected(rm.error());
    }
    fs::rename(tmp_dir, cache_dir, ec);
    if (ec) {
        auto copy = copy_directory_tree(tmp_dir, cache_dir);
        (void)remove_path_if_exists(tmp_dir);
        if (!copy) return std::unexpected(copy.error());
    }
    return cache_dir;
}

CliResult<fs::path> find_tarball_package_root(const fs::path& extracted_root) {
    std::error_code ec;
    if (fs::is_regular_file(extracted_root / "eta.toml", ec) && !ec) {
        return extracted_root;
    }

    std::vector<fs::path> candidates;
    for (const auto& entry : fs::directory_iterator(extracted_root, ec)) {
        if (ec) break;
        std::error_code entry_ec;
        if (!entry.is_directory(entry_ec) || entry_ec) continue;
        if (fs::is_regular_file(entry.path() / "eta.toml", entry_ec) && !entry_ec) {
            candidates.push_back(entry.path());
        }
    }
    if (candidates.size() == 1u) return candidates.front();
    if (candidates.empty()) {
        return std::unexpected("tarball extraction did not produce an eta.toml package root");
    }
    return std::unexpected("tarball extraction produced multiple package roots containing eta.toml");
}

CliResult<fs::path> ensure_tarball_cache(const eta::package::ManifestDependency& dependency,
                                         const fs::path& owner_root,
                                         const fs::path& cache_root) {
    const std::string source_id = dependency_source_id(dependency);
    const fs::path cache_dir = cache_root / ("tarball-" + cache_key_from_source(source_id));
    std::error_code ec;
    if (fs::is_regular_file(cache_dir / "eta.toml", ec) && !ec) {
        return cache_dir;
    }

    if (auto mk = ensure_directory(cache_root); !mk) return std::unexpected(mk.error());

    auto archive_path_res = resolve_tarball_archive_path(dependency, owner_root, cache_root);
    if (!archive_path_res) return std::unexpected(archive_path_res.error());
    const fs::path archive_path = *archive_path_res;

    if (!fs::is_regular_file(archive_path, ec) || ec) {
        return std::unexpected("tarball file not found: " + archive_path.string());
    }

    auto digest = compute_sha256_file(archive_path);
    if (!digest) return std::unexpected(digest.error());
    auto expected = lower_ascii(dependency.sha256);
    if (*digest != expected) {
        return std::unexpected("tarball sha256 mismatch for dependency '" + dependency.name + "'");
    }

    const auto stamp = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path tmp_extract = cache_root / ("tmp-tarball-" + stamp);
    if (auto rm = remove_path_if_exists(tmp_extract); !rm) return std::unexpected(rm.error());
    if (auto mk = ensure_directory(tmp_extract); !mk) return std::unexpected(mk.error());

    auto extract_result = run_external("tar",
                                       {"-xf", archive_path.string(), "-C", tmp_extract.string()});
    if (!extract_result) {
        (void)remove_path_if_exists(tmp_extract);
        return std::unexpected(extract_result.error());
    }
    if (*extract_result != 0) {
        (void)remove_path_if_exists(tmp_extract);
        return std::unexpected("failed to extract tarball dependency '" + dependency.name + "'");
    }

    auto package_root_res = find_tarball_package_root(tmp_extract);
    if (!package_root_res) {
        (void)remove_path_if_exists(tmp_extract);
        return std::unexpected(package_root_res.error());
    }

    if (auto rm = remove_path_if_exists(cache_dir); !rm) {
        (void)remove_path_if_exists(tmp_extract);
        return std::unexpected(rm.error());
    }
    std::error_code rename_ec;
    fs::rename(*package_root_res, cache_dir, rename_ec);
    if (rename_ec) {
        auto copy = copy_directory_tree(*package_root_res, cache_dir);
        if (!copy) {
            (void)remove_path_if_exists(tmp_extract);
            return std::unexpected(copy.error());
        }
    }
    (void)remove_path_if_exists(tmp_extract);
    return cache_dir;
}

CliResult<eta::package::ResolvedDependencyLocation>
materialize_non_path_for_resolver(const eta::package::Manifest& owner,
                                  const eta::package::ManifestDependency& dependency,
                                  const fs::path& cache_root) {
    fs::path cache_pkg;
    if (dependency.kind == eta::package::ManifestDependencyKind::Git) {
        auto cache = ensure_git_cache(dependency, cache_root);
        if (!cache) return std::unexpected(cache.error());
        cache_pkg = *cache;
    } else if (dependency.kind == eta::package::ManifestDependencyKind::Tarball) {
        auto cache = ensure_tarball_cache(dependency, owner.manifest_path.parent_path(), cache_root);
        if (!cache) return std::unexpected(cache.error());
        cache_pkg = *cache;
    } else {
        return std::unexpected("internal error: non-path materializer received path dependency");
    }

    auto dep_manifest = eta::package::read_manifest(cache_pkg / "eta.toml");
    if (!dep_manifest) {
        return std::unexpected("failed to read dependency manifest: " + dep_manifest.error().message);
    }
    if (dep_manifest->name != dependency.name) {
        return std::unexpected("dependency key '" + dependency.name
                               + "' does not match package.name '" + dep_manifest->name + "'");
    }

    eta::package::ResolvedDependencyLocation location;
    location.manifest_path = canonicalize_path(cache_pkg / "eta.toml");
    location.source = dependency_source_id(dependency);
    return location;
}

CliResult<std::pair<std::string, std::string>> parse_git_source(std::string_view source) {
    if (source.rfind("git+", 0) != 0) {
        return std::unexpected("invalid git source in lockfile: " + std::string(source));
    }
    const std::size_t hash_pos = source.find('#', 4u);
    if (hash_pos == std::string_view::npos || hash_pos + 1u >= source.size()) {
        return std::unexpected("invalid git source in lockfile: " + std::string(source));
    }
    const std::string url(source.substr(4u, hash_pos - 4u));
    const std::string rev(source.substr(hash_pos + 1u));
    if (!looks_like_hex(rev, 40u)) {
        return std::unexpected("invalid git rev in lockfile source: " + std::string(source));
    }
    return std::make_pair(url, rev);
}

CliResult<std::pair<std::string, std::string>> parse_tarball_source(std::string_view source) {
    if (source.rfind("tarball+", 0) != 0) {
        return std::unexpected("invalid tarball source in lockfile: " + std::string(source));
    }
    const std::string marker = "#sha256=";
    const std::size_t marker_pos = source.find(marker, 8u);
    if (marker_pos == std::string_view::npos || marker_pos + marker.size() >= source.size()) {
        return std::unexpected("invalid tarball source in lockfile: " + std::string(source));
    }
    const std::string tarball(source.substr(8u, marker_pos - 8u));
    const std::string sha256(source.substr(marker_pos + marker.size()));
    if (!looks_like_hex(sha256, 64u)) {
        return std::unexpected("invalid tarball sha256 in lockfile source: " + std::string(source));
    }
    return std::make_pair(tarball, sha256);
}

ErrorResult materialize_modules_from_lockfile(const fs::path& project_root,
                                              const eta::package::Lockfile& lockfile,
                                              const fs::path& modules_root) {
    auto cache_root_res = eta_cache_modules_root();
    if (!cache_root_res) return std::unexpected(cache_root_res.error());
    const fs::path cache_root = *cache_root_res;
    if (auto mk = ensure_directory(modules_root); !mk) return mk;

    for (const auto& package : lockfile.packages) {
        if (package.source == "root") continue;

        const fs::path package_dir = modules_root / (package.name + "-" + package.version);
        if (package.source.rfind("path+", 0) == 0) {
            const fs::path source_path = fs::path(package.source.substr(5u));
            if (auto copy = copy_directory_tree(source_path, package_dir); !copy) {
                return copy;
            }
            continue;
        }

        if (package.source.rfind("git+", 0) == 0) {
            auto parsed = parse_git_source(package.source);
            if (!parsed) return std::unexpected(parsed.error());

            eta::package::ManifestDependency dep;
            dep.name = package.name;
            dep.kind = eta::package::ManifestDependencyKind::Git;
            dep.git = parsed->first;
            dep.rev = parsed->second;

            auto cache = ensure_git_cache(dep, cache_root);
            if (!cache) return std::unexpected(cache.error());
            if (auto materialize = materialize_from_cache(*cache, package_dir); !materialize) {
                return materialize;
            }
            continue;
        }

        if (package.source.rfind("tarball+", 0) == 0) {
            eta::package::ManifestDependency dep;
            dep.name = package.name;
            dep.kind = eta::package::ManifestDependencyKind::Tarball;
            auto parsed = parse_tarball_source(package.source);
            if (!parsed) return std::unexpected(parsed.error());
            dep.tarball = parsed->first;
            dep.sha256 = parsed->second;

            const fs::path cache_dir =
                cache_root / ("tarball-" + cache_key_from_source(package.source));
            std::error_code ec;
            fs::path materialized_cache = cache_dir;
            if (!fs::is_regular_file(cache_dir / "eta.toml", ec) || ec) {
                auto cache = ensure_tarball_cache(dep, project_root, cache_root);
                if (!cache) return std::unexpected(cache.error());
                materialized_cache = *cache;
            }
            if (auto materialize = materialize_from_cache(materialized_cache, package_dir); !materialize) {
                return materialize;
            }
            continue;
        }

        return std::unexpected("unsupported lockfile source kind: " + package.source);
    }
    return {};
}

CliResult<ResolvedProjectState> resolve_project_state(const fs::path& manifest_path,
                                                      bool include_dev_dependencies,
                                                      bool write_lockfile,
                                                      bool materialize_modules) {
    const fs::path canonical_manifest = canonicalize_path(manifest_path);
    const fs::path project_root = canonical_manifest.parent_path();

    auto manifest = eta::package::read_manifest(canonical_manifest);
    if (!manifest) {
        return std::unexpected(manifest.error().message);
    }

    std::optional<eta::package::Lockfile> existing_lockfile;
    {
        auto lock = eta::package::read_lockfile(project_root / "eta.lock");
        if (lock) existing_lockfile = std::move(*lock);
    }

    auto cache_root_res = eta_cache_modules_root();
    if (!cache_root_res) return std::unexpected(cache_root_res.error());
    const fs::path cache_root = *cache_root_res;

    eta::package::ResolveOptions options;
    options.include_dev_dependencies = include_dev_dependencies;
    if (existing_lockfile.has_value()) options.lockfile = &*existing_lockfile;
    options.modules_root = project_root / ".eta" / "modules";
    options.dependency_locator =
        [&](const eta::package::Manifest& owner,
            const eta::package::ManifestDependency& dependency)
            -> std::expected<eta::package::ResolvedDependencyLocation, eta::package::ResolveError> {
        auto location = materialize_non_path_for_resolver(owner, dependency, cache_root);
        if (!location) {
            return std::unexpected(eta::package::ResolveError{
                eta::package::ResolveError::Code::MissingDependencyManifest,
                location.error(),
            });
        }
        return *location;
    };

    auto graph = eta::package::resolve_dependencies(canonical_manifest, options);
    if (!graph) return std::unexpected(graph.error().message);

    auto lockfile = eta::package::build_lockfile(*graph);
    if (write_lockfile) {
        auto write_result = eta::package::write_lockfile_file(lockfile, project_root / "eta.lock");
        if (!write_result) return std::unexpected(write_result.error().message);
    }

    if (materialize_modules) {
        auto materialize = materialize_modules_from_lockfile(
            project_root, lockfile, project_root / ".eta" / "modules");
        if (!materialize) return std::unexpected(materialize.error());
    }

    ResolvedProjectState state;
    state.manifest_path = canonical_manifest;
    state.project_root = project_root;
    state.manifest = std::move(*manifest);
    state.graph = std::move(*graph);
    state.lockfile = std::move(lockfile);
    return state;
}

[[nodiscard]] std::string display_source(const eta::package::ResolvedPackage& package,
                                         const fs::path& root_dir) {
    if (package.source == "root") return ".";
    if (package.source.rfind("path+", 0) == 0) {
        const fs::path raw = package.source.substr(5u);
        std::error_code ec;
        auto rel = fs::relative(raw, root_dir, ec);
        if (!ec && !rel.empty()) return rel.generic_string();
        return raw.generic_string();
    }
    return package.source;
}

void print_tree_children(std::ostream& out,
                         const eta::package::ResolvedPackage& node,
                         const fs::path& root_dir,
                         const std::unordered_map<std::string, const eta::package::ResolvedPackage*>& by_name,
                         std::string prefix,
                         std::size_t depth,
                         std::optional<std::size_t> max_depth) {
    if (max_depth.has_value() && depth >= *max_depth) return;

    std::vector<const eta::package::ResolvedPackage*> children;
    children.reserve(node.dependency_names.size());
    for (const auto& dep_name : node.dependency_names) {
        if (auto it = by_name.find(dep_name); it != by_name.end()) {
            children.push_back(it->second);
        }
    }

    for (std::size_t i = 0; i < children.size(); ++i) {
        const bool is_last = i + 1u == children.size();
        const auto* child = children[i];
        out << prefix
            << (is_last ? "`-- " : "|-- ")
            << child->name << " v" << child->version
            << " (" << display_source(*child, root_dir) << ")\n";

        print_tree_children(out,
                            *child,
                            root_dir,
                            by_name,
                            prefix + (is_last ? "    " : "|   "),
                            depth + 1u,
                            max_depth);
    }
}

[[nodiscard]] std::string join_module_path_entries(const std::vector<fs::path>& entries) {
#ifdef _WIN32
    constexpr char separator = ';';
#else
    constexpr char separator = ':';
#endif

    std::ostringstream out;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i != 0u) out << separator;
        out << entries[i].string();
    }
    return out.str();
}

void add_package_layout_dirs(const fs::path& package_root,
                             std::unordered_set<std::string>& seen,
                             std::vector<fs::path>& out) {
    auto add_unique = [&](const fs::path& candidate) {
        std::error_code ec;
        if (!fs::is_directory(candidate, ec) || ec) return;
        const auto key = path_key(candidate);
        if (seen.insert(key).second) out.push_back(canonicalize_path(candidate));
    };

    bool added = false;
    const auto release_dir = package_root / "target" / "release";
    add_unique(release_dir);
    if (seen.contains(path_key(release_dir))) added = true;

    const auto src_dir = package_root / "src";
    add_unique(src_dir);
    if (seen.contains(path_key(src_dir))) added = true;

    if (!added) add_unique(package_root);
}

std::vector<fs::path> module_entries_from_lockfile(const ResolvedProjectState& state) {
    std::vector<fs::path> entries;
    std::unordered_set<std::string> seen;

    const auto root_src = state.project_root / "src";
    std::error_code ec;
    if (fs::is_directory(root_src, ec) && !ec) {
        entries.push_back(canonicalize_path(root_src));
        seen.insert(path_key(root_src));
    } else {
        entries.push_back(canonicalize_path(state.project_root));
        seen.insert(path_key(state.project_root));
    }

    const fs::path modules_root = state.project_root / ".eta" / "modules";
    for (const auto& package : state.lockfile.packages) {
        if (package.source == "root") continue;
        add_package_layout_dirs(modules_root / (package.name + "-" + package.version), seen, entries);
    }
    return entries;
}

std::vector<fs::path> module_entries_from_graph(const ResolvedProjectState& state) {
    std::vector<fs::path> entries;
    std::unordered_set<std::string> seen;

    const auto* root = state.graph.find(state.graph.root_name);
    if (root != nullptr) {
        const auto root_src = root->package_root / "src";
        std::error_code ec;
        if (fs::is_directory(root_src, ec) && !ec) {
            entries.push_back(canonicalize_path(root_src));
            seen.insert(path_key(root_src));
        } else {
            entries.push_back(canonicalize_path(root->package_root));
            seen.insert(path_key(root->package_root));
        }
    }

    for (const auto& package : state.graph.packages) {
        if (package.name == state.graph.root_name) continue;
        add_package_layout_dirs(package.package_root, seen, entries);
    }
    return entries;
}

[[nodiscard]] fs::path resolve_self_path(const char* argv0) {
#ifdef _WIN32
    wchar_t buffer[4096];
    DWORD len = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (len > 0 && len < std::size(buffer)) {
        return canonicalize_path(fs::path(buffer));
    }
#endif
#if defined(__linux__)
    std::error_code ec;
    auto proc_self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return canonicalize_path(proc_self);
#endif
    if (argv0 != nullptr && argv0[0] != '\0') {
        return canonicalize_path(fs::absolute(fs::path(argv0)));
    }
    return {};
}

[[nodiscard]] fs::path find_tool_path(const char* argv0,
                                      std::string_view tool_name,
                                      std::optional<fs::path> configured) {
    std::vector<fs::path> candidates;
    if (configured.has_value()) candidates.push_back(*configured);

    const auto self = resolve_self_path(argv0);
    if (!self.empty()) {
#ifdef _WIN32
        candidates.push_back(self.parent_path() / (std::string(tool_name) + ".exe"));
#else
        candidates.push_back(self.parent_path() / std::string(tool_name));
#endif
    }

    std::error_code ec;
    for (const auto& candidate : candidates) {
        if (candidate.empty()) continue;
        if (fs::is_regular_file(candidate, ec) && !ec) {
            return canonicalize_path(candidate);
        }
        ec.clear();
    }

#ifdef _WIN32
    return fs::path(std::string(tool_name) + ".exe");
#else
    return fs::path(std::string(tool_name));
#endif
}

[[nodiscard]] fs::path find_etai_path(const char* argv0) {
#ifdef ETA_ETAI_PATH
    return find_tool_path(argv0, "etai", fs::path(ETA_ETAI_PATH));
#else
    return find_tool_path(argv0, "etai", std::nullopt);
#endif
}

[[nodiscard]] fs::path find_etac_path(const char* argv0) {
#ifdef ETA_ETAC_PATH
    return find_tool_path(argv0, "etac", fs::path(ETA_ETAC_PATH));
#else
    return find_tool_path(argv0, "etac", std::nullopt);
#endif
}

[[nodiscard]] fs::path find_eta_test_path(const char* argv0) {
#ifdef ETA_ETA_TEST_PATH
    return find_tool_path(argv0, "eta_test", fs::path(ETA_ETA_TEST_PATH));
#else
    return find_tool_path(argv0, "eta_test", std::nullopt);
#endif
}

CliResult<int> run_with_etai(const char* argv0,
                             const std::vector<std::string>& args,
                             const std::optional<fs::path>& cwd = std::nullopt) {
    return run_external(find_etai_path(argv0), args, cwd);
}

CliResult<int> run_with_etac(const char* argv0,
                             const std::vector<std::string>& args,
                             const std::optional<fs::path>& cwd = std::nullopt) {
    return run_external(find_etac_path(argv0), args, cwd);
}

CliResult<int> run_with_eta_test(const char* argv0,
                                 const std::vector<std::string>& args,
                                 const std::optional<fs::path>& cwd = std::nullopt) {
    return run_external(find_eta_test_path(argv0), args, cwd);
}

std::vector<fs::path> collect_eta_sources(const fs::path& root_dir) {
    std::vector<fs::path> files;
    std::error_code ec;
    if (!fs::is_directory(root_dir, ec) || ec) return files;
    for (const auto& entry : fs::recursive_directory_iterator(root_dir, ec)) {
        if (ec) break;
        std::error_code entry_ec;
        if (!entry.is_regular_file(entry_ec) || entry_ec) continue;
        if (entry.path().extension() == ".eta") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

CliResult<int> compile_project_sources(const char* argv0,
                                       const ResolvedProjectState& state,
                                       const BuildOptions& options) {
    const fs::path src_root = state.project_root / "src";
    std::vector<fs::path> source_files;
    if (options.bin_name.has_value()) {
        const fs::path candidate =
            src_root / (module_name_from_package_name(*options.bin_name) + ".eta");
        if (!fs::is_regular_file(candidate)) {
            return std::unexpected("eta build: entry source not found: " + candidate.string());
        }
        source_files.push_back(candidate);
    } else {
        source_files = collect_eta_sources(src_root);
    }
    if (source_files.empty()) {
        return std::unexpected("eta build: no source files found under " + src_root.string());
    }

    const fs::path out_root = state.project_root / ".eta" / "target" / options.profile;
    if (auto mk = ensure_directory(out_root); !mk) return std::unexpected(mk.error());

    auto module_entries = module_entries_from_lockfile(state);
    const std::string module_path = join_module_path_entries(module_entries);

    for (const auto& source : source_files) {
        std::error_code ec;
        auto rel = fs::relative(source, src_root, ec);
        if (ec || rel.empty()) rel = source.filename();
        fs::path output = out_root / rel;
        output.replace_extension(".etac");
        if (auto mk = ensure_directory(output.parent_path()); !mk) {
            return std::unexpected(mk.error());
        }

        std::vector<std::string> etac_args;
        if (options.profile == "release") {
            etac_args.push_back("-O");
            etac_args.push_back("--no-debug");
        } else {
            etac_args.push_back("-O0");
        }
        if (!module_path.empty()) {
            etac_args.push_back("--path");
            etac_args.push_back(module_path);
        }
        etac_args.push_back(source.string());
        etac_args.push_back("-o");
        etac_args.push_back(output.string());

        auto rc = run_with_etac(argv0, etac_args, state.project_root);
        if (!rc) return std::unexpected(rc.error());
        if (*rc != 0) return rc;
    }

    std::cout << "compiled " << source_files.size() << " source file(s) to "
              << out_root.string() << "\n";
    return 0;
}

CliResult<int> run_project_suite(const char* argv0,
                                 const ResolvedProjectState& state,
                                 const fs::path& suite_root,
                                 const PathCommandOptions& options) {
    std::error_code ec;
    if (!fs::is_directory(suite_root, ec) || ec) {
        std::cout << "no suite directory: " << suite_root.string() << "\n";
        return 0;
    }

    auto module_entries = module_entries_from_graph(state);
    const std::string module_path = join_module_path_entries(module_entries);

    std::vector<std::string> test_args;
    if (!module_path.empty()) {
        test_args.push_back("--path");
        test_args.push_back(module_path);
    }

    if (options.filter.has_value()) {
        test_args.push_back(*options.filter);
    } else {
        test_args.push_back(suite_root.string());
    }

    auto rc = run_with_eta_test(argv0, test_args, state.project_root);
    if (!rc) return std::unexpected(rc.error());
    return *rc;
}

CliResult<int> command_new(const char* program,
                           const std::vector<std::string>& args,
                           const fs::path& cwd) {
    ProjectKind kind = ProjectKind::Bin;
    std::optional<std::string> package_name;

    for (const auto& arg : args) {
        if (arg == "--help" || arg == "-h") {
            print_new_usage(program);
            return 0;
        }
        if (arg == "--bin") {
            kind = ProjectKind::Bin;
            continue;
        }
        if (arg == "--lib") {
            kind = ProjectKind::Lib;
            continue;
        }
        if (!arg.empty() && arg.front() == '-') {
            return std::unexpected("unknown option for eta new: " + arg);
        }
        if (package_name.has_value()) {
            return std::unexpected("eta new accepts exactly one <name> argument");
        }
        package_name = arg;
    }

    if (!package_name.has_value()) {
        return std::unexpected("eta new requires a package name");
    }
    if (!is_valid_package_name(*package_name)) {
        return std::unexpected(
            "package name must match [a-z][a-z0-9_-]{0,63}: " + *package_name);
    }

    return scaffold_project(cwd / *package_name, *package_name, kind, false);
}

CliResult<int> command_init(const char* program,
                            const std::vector<std::string>& args,
                            const fs::path& cwd) {
    ProjectKind kind = ProjectKind::Bin;

    for (const auto& arg : args) {
        if (arg == "--help" || arg == "-h") {
            print_init_usage(program);
            return 0;
        }
        if (arg == "--bin") {
            kind = ProjectKind::Bin;
            continue;
        }
        if (arg == "--lib") {
            kind = ProjectKind::Lib;
            continue;
        }
        return std::unexpected("eta init does not accept positional arguments: " + arg);
    }

    auto package_name = cwd.filename().string();
    if (package_name.empty()) {
        return std::unexpected("cannot infer package name from current directory");
    }
    if (!is_valid_package_name(package_name)) {
        return std::unexpected("current directory name is not a valid package name: " + package_name);
    }

    return scaffold_project(cwd, std::move(package_name), kind, true);
}

CliResult<int> command_tree(const char* program,
                            const std::vector<std::string>& args,
                            const fs::path& cwd) {
    std::optional<std::size_t> max_depth;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--help" || arg == "-h") {
            print_tree_usage(program);
            return 0;
        }
        if (arg == "--depth") {
            if (i + 1u >= args.size()) {
                return std::unexpected("eta tree: --depth requires a value");
            }
            auto depth_res = parse_depth(args[++i]);
            if (!depth_res) return std::unexpected("eta tree: " + depth_res.error());
            max_depth = *depth_res;
            continue;
        }
        return std::unexpected("eta tree: unknown option " + arg);
    }

    auto manifest_path = find_manifest_path(cwd);
    if (!manifest_path.has_value()) {
        return std::unexpected("eta tree: could not find eta.toml (searched from "
                               + cwd.string() + ")");
    }

    auto state = resolve_project_state(*manifest_path, false, false, false);
    if (!state) return std::unexpected("eta tree: " + state.error());

    const auto* root = state->graph.find(state->graph.root_name);
    if (root == nullptr) return std::unexpected("eta tree: resolved graph is missing root package");

    std::unordered_map<std::string, const eta::package::ResolvedPackage*> by_name;
    by_name.reserve(state->graph.packages.size());
    for (const auto& package : state->graph.packages) {
        by_name.emplace(package.name, &package);
    }

    const auto root_dir = root->package_root;
    std::cout << root->name << " v" << root->version
              << " (" << display_source(*root, root_dir) << ")\n";
    print_tree_children(std::cout, *root, root_dir, by_name, "", 0u, max_depth);
    return 0;
}

CliResult<RunOptions> parse_run_options(const std::vector<std::string>& args) {
    RunOptions options;
    bool parsing_program_args = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (parsing_program_args) {
            options.program_args.push_back(arg);
            continue;
        }
        if (arg == "--") {
            parsing_program_args = true;
            continue;
        }
        if (arg == "--profile") {
            if (i + 1u >= args.size()) {
                return std::unexpected("eta run: --profile requires a value");
            }
            options.profile = args[++i];
            continue;
        }
        if (arg == "--strict-shadows") {
            options.strict_shadows = true;
            continue;
        }
        if (arg == "--bin") {
            if (i + 1u >= args.size()) {
                return std::unexpected("eta run: --bin requires a value");
            }
            options.bin_name = args[++i];
            continue;
        }
        if (arg == "--example") {
            if (i + 1u >= args.size()) {
                return std::unexpected("eta run: --example requires a value");
            }
            options.example_name = args[++i];
            continue;
        }
        if (!arg.empty() && arg.front() == '-') {
            return std::unexpected("eta run: unknown option " + arg);
        }
        if (options.input_path.has_value()) {
            return std::unexpected("eta run accepts at most one positional file path");
        }
        options.input_path = fs::path(arg);
    }

    if (options.profile != "release" && options.profile != "debug") {
        return std::unexpected("eta run: --profile must be 'release' or 'debug'");
    }
    if (options.input_path.has_value()
        && (options.bin_name.has_value() || options.example_name.has_value())) {
        return std::unexpected("eta run: positional file path cannot be combined with --bin/--example");
    }
    if (options.bin_name.has_value() && options.example_name.has_value()) {
        return std::unexpected("eta run: --bin and --example are mutually exclusive");
    }
    return options;
}

CliResult<int> command_run(const char* program,
                           const char* argv0,
                           const std::vector<std::string>& args,
                           const fs::path& cwd) {
    for (const auto& arg : args) {
        if (arg == "--help" || arg == "-h") {
            print_run_usage(program);
            return 0;
        }
    }

    auto parsed = parse_run_options(args);
    if (!parsed) return std::unexpected(parsed.error());
    RunOptions options = std::move(*parsed);

    if (options.input_path.has_value()) {
        const fs::path input = fs::absolute(*options.input_path);
        if (!fs::exists(input)) {
            return std::unexpected("eta run: file not found: " + input.string());
        }

        std::vector<std::string> etai_args;
        if (options.strict_shadows) etai_args.push_back("--strict-shadows");
        etai_args.push_back(input.string());
        if (!options.program_args.empty()) {
            etai_args.push_back("--");
            etai_args.insert(etai_args.end(), options.program_args.begin(), options.program_args.end());
        }
        return run_with_etai(argv0, etai_args, cwd);
    }

    auto manifest_path = find_manifest_path(cwd);
    if (!manifest_path.has_value()) {
        return std::unexpected("eta run: no eta.toml found (or pass a file path for script mode)");
    }

    auto state = resolve_project_state(*manifest_path, false, true, true);
    if (!state) return std::unexpected("eta run: " + state.error());

    fs::path run_file;
    if (options.example_name.has_value()) {
        fs::path example_rel = *options.example_name;
        if (example_rel.extension().empty()) example_rel += ".eta";
        run_file = state->project_root / "cookbook" / example_rel;
    } else if (options.bin_name.has_value()) {
        run_file = state->project_root / "src" / (module_name_from_package_name(*options.bin_name) + ".eta");
    } else {
        run_file = state->project_root / "src" / (module_name_from_package_name(state->manifest.name) + ".eta");
    }

    if (!fs::is_regular_file(run_file)) {
        return std::unexpected("eta run: entry file not found: " + run_file.string());
    }

    auto module_entries = module_entries_from_lockfile(*state);
    const std::string module_path = join_module_path_entries(module_entries);

    std::vector<std::string> etai_args;
    if (options.strict_shadows) etai_args.push_back("--strict-shadows");
    if (!module_path.empty()) {
        etai_args.push_back("--path");
        etai_args.push_back(module_path);
    }
    etai_args.push_back(fs::absolute(run_file).string());

    if (!options.program_args.empty()) {
        etai_args.push_back("--");
        etai_args.insert(etai_args.end(), options.program_args.begin(), options.program_args.end());
    }
    return run_with_etai(argv0, etai_args, state->project_root);
}

CliResult<BuildOptions> parse_build_options(const std::vector<std::string>& args) {
    BuildOptions options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--profile") {
            if (i + 1u >= args.size()) {
                return std::unexpected("eta build: --profile requires a value");
            }
            options.profile = args[++i];
            continue;
        }
        if (arg == "--bin") {
            if (i + 1u >= args.size()) {
                return std::unexpected("eta build: --bin requires a value");
            }
            options.bin_name = args[++i];
            continue;
        }
        return std::unexpected("eta build: unknown option " + arg);
    }
    if (options.profile != "release" && options.profile != "debug") {
        return std::unexpected("eta build: --profile must be 'release' or 'debug'");
    }
    return options;
}

CliResult<int> command_build(const char* program,
                             const char* argv0,
                             const std::vector<std::string>& args,
                             const fs::path& cwd) {
    for (const auto& arg : args) {
        if (arg == "--help" || arg == "-h") {
            print_build_usage(program);
            return 0;
        }
    }

    auto options = parse_build_options(args);
    if (!options) return std::unexpected(options.error());

    auto manifest_path = find_manifest_path(cwd);
    if (!manifest_path.has_value()) {
        return std::unexpected("eta build: no eta.toml found");
    }

    auto state = resolve_project_state(*manifest_path, false, true, true);
    if (!state) return std::unexpected("eta build: " + state.error());

    return compile_project_sources(argv0, *state, *options);
}

CliResult<PathCommandOptions> parse_path_command_options(const std::string& command,
                                                         const std::vector<std::string>& args) {
    PathCommandOptions options;
    for (const auto& arg : args) {
        if (!arg.empty() && arg.front() == '-') {
            return std::unexpected("eta " + command + ": unknown option " + arg);
        }
        if (options.filter.has_value()) {
            return std::unexpected("eta " + command + ": accepts at most one filter path");
        }
        options.filter = arg;
    }
    return options;
}

CliResult<int> command_test(const char* program,
                            const char* argv0,
                            const std::vector<std::string>& args,
                            const fs::path& cwd) {
    for (const auto& arg : args) {
        if (arg == "--help" || arg == "-h") {
            print_path_command_usage(program, "test", "tests");
            return 0;
        }
    }

    auto options = parse_path_command_options("test", args);
    if (!options) return std::unexpected(options.error());

    auto manifest_path = find_manifest_path(cwd);
    if (!manifest_path.has_value()) {
        return std::unexpected("eta test: no eta.toml found");
    }
    auto state = resolve_project_state(*manifest_path, true, false, true);
    if (!state) return std::unexpected("eta test: " + state.error());

    const fs::path suite_root = state->project_root / "tests";
    return run_project_suite(argv0, *state, suite_root, *options);
}

CliResult<int> command_bench(const char* program,
                             const char* argv0,
                             const std::vector<std::string>& args,
                             const fs::path& cwd) {
    for (const auto& arg : args) {
        if (arg == "--help" || arg == "-h") {
            print_path_command_usage(program, "bench", "bench");
            return 0;
        }
    }

    auto options = parse_path_command_options("bench", args);
    if (!options) return std::unexpected(options.error());

    auto manifest_path = find_manifest_path(cwd);
    if (!manifest_path.has_value()) {
        return std::unexpected("eta bench: no eta.toml found");
    }
    auto state = resolve_project_state(*manifest_path, true, false, true);
    if (!state) return std::unexpected("eta bench: " + state.error());

    const fs::path suite_root = state->project_root / "bench";
    return run_project_suite(argv0, *state, suite_root, *options);
}

CliResult<VendorOptions> parse_vendor_options(const std::vector<std::string>& args) {
    VendorOptions options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--target") {
            if (i + 1u >= args.size()) {
                return std::unexpected("eta vendor: --target requires a value");
            }
            options.target = fs::path(args[++i]);
            continue;
        }
        return std::unexpected("eta vendor: unknown option " + arg);
    }
    return options;
}

CliResult<int> command_vendor(const char* program,
                              const std::vector<std::string>& args,
                              const fs::path& cwd) {
    for (const auto& arg : args) {
        if (arg == "--help" || arg == "-h") {
            print_vendor_usage(program);
            return 0;
        }
    }

    auto options = parse_vendor_options(args);
    if (!options) return std::unexpected(options.error());

    auto manifest_path = find_manifest_path(cwd);
    if (!manifest_path.has_value()) {
        return std::unexpected("eta vendor: no eta.toml found");
    }
    auto state = resolve_project_state(*manifest_path, false, true, true);
    if (!state) return std::unexpected("eta vendor: " + state.error());

    fs::path target = state->project_root / ".eta" / "modules";
    if (options->target.has_value()) {
        target = options->target->is_absolute()
            ? *options->target
            : canonicalize_path(state->project_root / *options->target);
    }

    if (auto materialize = materialize_modules_from_lockfile(state->project_root, state->lockfile, target);
        !materialize) {
        return std::unexpected("eta vendor: " + materialize.error());
    }

    std::cout << "materialized dependencies to " << target.string() << "\n";
    return 0;
}

CliResult<InstallOptions> parse_install_options(const std::vector<std::string>& args) {
    InstallOptions options;
    for (const auto& arg : args) {
        if (arg == "--global") {
            options.global = true;
            continue;
        }
        if (!arg.empty() && arg.front() == '-') {
            return std::unexpected("eta install: unknown option " + arg);
        }
        if (options.package.has_value()) {
            return std::unexpected("eta install: accepts at most one package name");
        }
        options.package = arg;
    }
    return options;
}

CliResult<int> command_install(const char* program,
                               const char* argv0,
                               const std::vector<std::string>& args,
                               const fs::path& cwd) {
    for (const auto& arg : args) {
        if (arg == "--help" || arg == "-h") {
            print_install_usage(program);
            return 0;
        }
    }

    auto options = parse_install_options(args);
    if (!options) return std::unexpected(options.error());

    auto manifest_path = find_manifest_path(cwd);
    if (!manifest_path.has_value()) {
        return std::unexpected("eta install: no eta.toml found");
    }
    auto state = resolve_project_state(*manifest_path, false, true, true);
    if (!state) return std::unexpected("eta install: " + state.error());

    BuildOptions build_opts;
    build_opts.profile = "release";
    auto build_rc = compile_project_sources(argv0, *state, build_opts);
    if (!build_rc) return std::unexpected(build_rc.error());
    if (*build_rc != 0) return build_rc;

    fs::path install_root;
    if (options->global) {
        auto home = home_dir();
        if (!home.has_value()) {
            return std::unexpected("eta install: failed to resolve user home directory");
        }
        install_root = *home / ".eta" / "bin";
    } else {
        install_root = state->project_root / ".eta" / "bin";
    }
    if (auto mk = ensure_directory(install_root); !mk) return std::unexpected(mk.error());

    if (!options->package.has_value()) {
        const std::string root_module = module_name_from_package_name(state->manifest.name);
        const fs::path source = state->project_root / ".eta" / "target" / "release" / (root_module + ".etac");
        if (!fs::is_regular_file(source)) {
            return std::unexpected("eta install: built artifact missing: " + source.string());
        }
        const fs::path destination = install_root / (root_module + ".etac");
        std::error_code ec;
        fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            return std::unexpected("eta install: failed to copy artifact: " + ec.message());
        }
        std::cout << "installed " << destination.string() << "\n";
        return 0;
    }

    const auto it = std::find_if(state->lockfile.packages.begin(),
                                 state->lockfile.packages.end(),
                                 [&](const eta::package::LockfilePackage& pkg) {
                                     return pkg.name == *options->package;
                                 });
    if (it == state->lockfile.packages.end()) {
        return std::unexpected("eta install: package not found in lockfile: " + *options->package);
    }
    const fs::path dep_release =
        state->project_root / ".eta" / "modules" / (it->name + "-" + it->version) / "target" / "release";
    std::error_code ec;
    if (!fs::is_directory(dep_release, ec) || ec) {
        return std::unexpected("eta install: dependency has no release artifacts: " + dep_release.string());
    }
    const fs::path dep_install_root = install_root / it->name;
    if (auto copy = copy_directory_tree(dep_release, dep_install_root); !copy) {
        return std::unexpected(copy.error());
    }
    std::cout << "installed dependency artifacts to " << dep_install_root.string() << "\n";
    return 0;
}

CliResult<AddOptions> parse_add_options(const std::vector<std::string>& args) {
    AddOptions options;
    bool name_seen = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--dev") {
            options.dev = true;
            continue;
        }
        if (arg == "--path") {
            if (i + 1u >= args.size()) return std::unexpected("eta add: --path requires a value");
            options.path = fs::path(args[++i]);
            continue;
        }
        if (arg == "--git") {
            if (i + 1u >= args.size()) return std::unexpected("eta add: --git requires a value");
            options.git = args[++i];
            continue;
        }
        if (arg == "--rev") {
            if (i + 1u >= args.size()) return std::unexpected("eta add: --rev requires a value");
            options.rev = args[++i];
            continue;
        }
        if (arg == "--tarball") {
            if (i + 1u >= args.size()) return std::unexpected("eta add: --tarball requires a value");
            options.tarball = args[++i];
            continue;
        }
        if (arg == "--sha256") {
            if (i + 1u >= args.size()) return std::unexpected("eta add: --sha256 requires a value");
            options.sha256 = args[++i];
            continue;
        }
        if (!arg.empty() && arg.front() == '-') {
            return std::unexpected("eta add: unknown option " + arg);
        }
        if (name_seen) {
            return std::unexpected("eta add: accepts exactly one package argument");
        }
        name_seen = true;
        const auto at = arg.find('@');
        options.name = (at == std::string::npos) ? arg : arg.substr(0u, at);
    }

    if (options.name.empty()) return std::unexpected("eta add: missing package name");
    if (!is_valid_package_name(options.name)) {
        return std::unexpected("eta add: package name must match [a-z][a-z0-9_-]{0,63}");
    }

    const int kinds = (options.path.has_value() ? 1 : 0)
        + (options.git.has_value() ? 1 : 0)
        + (options.tarball.has_value() ? 1 : 0);
    if (kinds != 1) {
        return std::unexpected("eta add: choose exactly one source kind: --path, --git, or --tarball");
    }
    if (options.git.has_value()) {
        if (!options.rev.has_value()) {
            return std::unexpected("eta add: git dependencies require --rev <40-hex>");
        }
        if (!looks_like_hex(*options.rev, 40u)) {
            return std::unexpected("eta add: --rev must be a full 40-hex commit id");
        }
    }
    if (options.tarball.has_value()) {
        if (!options.sha256.has_value()) {
            return std::unexpected("eta add: tarball dependencies require --sha256 <64-hex>");
        }
        if (!looks_like_hex(*options.sha256, 64u)) {
            return std::unexpected("eta add: --sha256 must be 64 hex characters");
        }
    }
    return options;
}

void upsert_dependency(std::vector<eta::package::ManifestDependency>& deps,
                       eta::package::ManifestDependency dep) {
    auto it = std::find_if(deps.begin(),
                           deps.end(),
                           [&](const eta::package::ManifestDependency& existing) {
                               return existing.name == dep.name;
                           });
    if (it != deps.end()) {
        *it = std::move(dep);
    } else {
        deps.push_back(std::move(dep));
    }
    std::sort(deps.begin(),
              deps.end(),
              [](const eta::package::ManifestDependency& lhs,
                 const eta::package::ManifestDependency& rhs) {
                  return lhs.name < rhs.name;
              });
}

CliResult<int> command_add(const char* program,
                           const std::vector<std::string>& args,
                           const fs::path& cwd) {
    for (const auto& arg : args) {
        if (arg == "--help" || arg == "-h") {
            print_add_usage(program);
            return 0;
        }
    }

    auto options = parse_add_options(args);
    if (!options) return std::unexpected(options.error());

    auto manifest_path = find_manifest_path(cwd);
    if (!manifest_path.has_value()) {
        return std::unexpected("eta add: no eta.toml found");
    }

    auto manifest = eta::package::read_manifest(*manifest_path);
    if (!manifest) return std::unexpected(manifest.error().message);

    eta::package::ManifestDependency dep;
    dep.name = options->name;
    if (options->path.has_value()) {
        dep.kind = eta::package::ManifestDependencyKind::Path;
        dep.path = *options->path;
    } else if (options->git.has_value()) {
        dep.kind = eta::package::ManifestDependencyKind::Git;
        dep.git = *options->git;
        dep.rev = *options->rev;
    } else {
        dep.kind = eta::package::ManifestDependencyKind::Tarball;
        dep.tarball = *options->tarball;
        dep.sha256 = lower_ascii(*options->sha256);
    }

    if (options->dev) {
        manifest->dependencies.erase(
            std::remove_if(manifest->dependencies.begin(),
                           manifest->dependencies.end(),
                           [&](const eta::package::ManifestDependency& existing) {
                               return existing.name == dep.name;
                           }),
            manifest->dependencies.end());
        upsert_dependency(manifest->dev_dependencies, std::move(dep));
    } else {
        manifest->dev_dependencies.erase(
            std::remove_if(manifest->dev_dependencies.begin(),
                           manifest->dev_dependencies.end(),
                           [&](const eta::package::ManifestDependency& existing) {
                               return existing.name == dep.name;
                           }),
            manifest->dev_dependencies.end());
        upsert_dependency(manifest->dependencies, std::move(dep));
    }

    auto write_res = eta::package::write_manifest_file(*manifest, *manifest_path);
    if (!write_res) return std::unexpected(write_res.error().message);

    auto state = resolve_project_state(*manifest_path, false, true, true);
    if (!state) return std::unexpected("eta add: " + state.error());

    std::cout << "added dependency " << options->name << "\n";
    return 0;
}

CliResult<int> command_remove(const char* program,
                              const std::vector<std::string>& args,
                              const fs::path& cwd) {
    for (const auto& arg : args) {
        if (arg == "--help" || arg == "-h") {
            print_remove_usage(program);
            return 0;
        }
    }

    if (args.size() != 1u || args.front().empty() || args.front().front() == '-') {
        return std::unexpected("eta remove: requires exactly one package name");
    }
    const std::string package_name = args.front();

    auto manifest_path = find_manifest_path(cwd);
    if (!manifest_path.has_value()) {
        return std::unexpected("eta remove: no eta.toml found");
    }
    auto manifest = eta::package::read_manifest(*manifest_path);
    if (!manifest) return std::unexpected(manifest.error().message);

    auto erase_by_name = [&](std::vector<eta::package::ManifestDependency>& deps) {
        const auto before = deps.size();
        deps.erase(std::remove_if(deps.begin(),
                                  deps.end(),
                                  [&](const eta::package::ManifestDependency& dep) {
                                      return dep.name == package_name;
                                  }),
                   deps.end());
        return before != deps.size();
    };

    const bool removed = erase_by_name(manifest->dependencies) || erase_by_name(manifest->dev_dependencies);
    if (!removed) {
        return std::unexpected("eta remove: dependency not found: " + package_name);
    }

    auto write_res = eta::package::write_manifest_file(*manifest, *manifest_path);
    if (!write_res) return std::unexpected(write_res.error().message);

    auto state = resolve_project_state(*manifest_path, false, true, true);
    if (!state) return std::unexpected("eta remove: " + state.error());

    std::cout << "removed dependency " << package_name << "\n";
    return 0;
}

CliResult<int> command_update(const char* program,
                              const std::vector<std::string>& args,
                              const fs::path& cwd) {
    for (const auto& arg : args) {
        if (arg == "--help" || arg == "-h") {
            print_update_usage(program);
            return 0;
        }
        if (!arg.empty() && arg.front() == '-') {
            return std::unexpected("eta update: unknown option " + arg);
        }
    }

    auto manifest_path = find_manifest_path(cwd);
    if (!manifest_path.has_value()) {
        return std::unexpected("eta update: no eta.toml found");
    }
    auto state = resolve_project_state(*manifest_path, false, true, true);
    if (!state) return std::unexpected("eta update: " + state.error());

    if (!args.empty()) {
        std::cout << "updated lockfile (full graph refresh; package-scoped update is not narrowed yet)\n";
    } else {
        std::cout << "updated lockfile\n";
    }
    return 0;
}

CliResult<CleanOptions> parse_clean_options(const std::vector<std::string>& args) {
    CleanOptions options;
    for (const auto& arg : args) {
        if (arg == "--all") {
            options.all = true;
            continue;
        }
        return std::unexpected("eta clean: unknown option " + arg);
    }
    return options;
}

CliResult<int> command_clean(const char* program,
                             const std::vector<std::string>& args,
                             const fs::path& cwd) {
    for (const auto& arg : args) {
        if (arg == "--help" || arg == "-h") {
            print_clean_usage(program);
            return 0;
        }
    }

    auto options = parse_clean_options(args);
    if (!options) return std::unexpected(options.error());

    auto manifest_path = find_manifest_path(cwd);
    if (!manifest_path.has_value()) {
        return std::unexpected("eta clean: no eta.toml found");
    }
    const fs::path project_root = manifest_path->parent_path();

    if (auto rm = remove_path_if_exists(project_root / ".eta" / "target"); !rm) {
        return std::unexpected("eta clean: " + rm.error());
    }
    if (options->all) {
        if (auto rm = remove_path_if_exists(project_root / ".eta" / "modules"); !rm) {
            return std::unexpected("eta clean: " + rm.error());
        }
    }
    std::cout << "cleaned project artifacts\n";
    return 0;
}

int print_not_yet_implemented(std::string_view command, std::string_view stage) {
    std::cerr << "NotYetImplemented(stage=" << stage << "): eta " << command << "\n";
    return 1;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const std::string command = argv[1];
    if (command == "--help" || command == "-h" || command == "help") {
        print_usage(argv[0]);
        return 0;
    }

    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc - 2));
    for (int i = 2; i < argc; ++i) args.emplace_back(argv[i]);

    const fs::path cwd = canonicalize_path(fs::current_path());

    CliResult<int> result = std::unexpected("unknown command");
    if (command == "new") {
        result = command_new(argv[0], args, cwd);
    } else if (command == "init") {
        result = command_init(argv[0], args, cwd);
    } else if (command == "tree") {
        result = command_tree(argv[0], args, cwd);
    } else if (command == "run") {
        result = command_run(argv[0], argv[0], args, cwd);
    } else if (command == "add") {
        result = command_add(argv[0], args, cwd);
    } else if (command == "remove") {
        result = command_remove(argv[0], args, cwd);
    } else if (command == "update") {
        result = command_update(argv[0], args, cwd);
    } else if (command == "build") {
        result = command_build(argv[0], argv[0], args, cwd);
    } else if (command == "test") {
        result = command_test(argv[0], argv[0], args, cwd);
    } else if (command == "bench") {
        result = command_bench(argv[0], argv[0], args, cwd);
    } else if (command == "vendor") {
        result = command_vendor(argv[0], args, cwd);
    } else if (command == "install") {
        result = command_install(argv[0], argv[0], args, cwd);
    } else if (command == "clean") {
        result = command_clean(argv[0], args, cwd);
    } else {
        static const std::unordered_map<std::string, std::string> kNotYetCommands = {
            {"repl", "S7"},
            {"publish", "S8"},
            {"fmt", "post-v1"},
            {"doc", "post-v1"},
        };
        if (auto it = kNotYetCommands.find(command); it != kNotYetCommands.end()) {
            return print_not_yet_implemented(command, it->second);
        }
        std::cerr << "error: unknown command: " << command << "\n";
        print_usage(argv[0]);
        return 1;
    }

    if (!result) {
        std::cerr << "error: " << result.error() << "\n";
        return 1;
    }
    return *result;
}
