#define BOOST_TEST_MODULE eta.cli.test
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "eta/native/sidecar_loader.h"

namespace fs = std::filesystem;

#ifndef ETA_CLI_PATH
#error ETA_CLI_PATH must be defined by CMake
#endif

#ifndef ETA_REPL_PATH
#error ETA_REPL_PATH must be defined by CMake
#endif

#ifndef ETA_CLI_TEST_FIXTURES_DIR
#error ETA_CLI_TEST_FIXTURES_DIR must be defined by CMake
#endif

#ifndef ETA_CLI_TEST_NATIVE_SIDECAR_PATH
#error ETA_CLI_TEST_NATIVE_SIDECAR_PATH must be defined by CMake
#endif

namespace {

const fs::path kEtaCliPath{ETA_CLI_PATH};
const fs::path kEtaReplPath{ETA_REPL_PATH};
const fs::path kCliTestFixturesDir{ETA_CLI_TEST_FIXTURES_DIR};
const fs::path kNativeSidecarFixturePath{ETA_CLI_TEST_NATIVE_SIDECAR_PATH};

struct TempDir {
    fs::path path;

    TempDir() {
        const auto suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path = fs::temp_directory_path() / ("eta_cli_test_" + suffix);
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    fs::path write_file(const std::string& rel, const std::string& text) const {
        const auto full = path / rel;
        fs::create_directories(full.parent_path());
        std::ofstream out(full, std::ios::out | std::ios::binary | std::ios::trunc);
        out << text;
        return full;
    }
};

struct CommandResult {
    int exit_code{1};
    std::string output;
};

#ifdef _WIN32
std::wstring quote_windows_arg(const std::wstring& arg) {
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

std::wstring build_windows_command_line(const fs::path& exe_path,
                                        const std::vector<std::string>& args) {
    std::wstring command = quote_windows_arg(exe_path.wstring());
    for (const auto& arg : args) {
        command.push_back(L' ');
        command += quote_windows_arg(fs::path(arg).wstring());
    }
    return command;
}

std::string win32_error_message(DWORD error_code) {
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
std::vector<char*> make_exec_argv(const fs::path& exe_path,
                                  const std::vector<std::string>& args,
                                  std::vector<std::string>& storage) {
    storage.clear();
    storage.reserve(args.size() + 1u);
    storage.push_back(exe_path.string());
    storage.insert(storage.end(), args.begin(), args.end());

    std::vector<char*> argv;
    argv.reserve(storage.size() + 1u);
    for (auto& entry : storage) argv.push_back(entry.data());
    argv.push_back(nullptr);
    return argv;
}
#endif

std::string read_text_file(const fs::path& path) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

fs::path fixture_path(const fs::path& rel) {
    return kCliTestFixturesDir / rel;
}

fs::path copy_fixture_tree(const TempDir& temp, const fs::path& rel) {
    const auto source = fixture_path(rel);
    const auto destination = temp.path / "fixture";
    std::error_code ec;
    fs::copy(source, destination, fs::copy_options::recursive, ec);
    BOOST_REQUIRE_MESSAGE(!ec,
                          "failed to copy fixture tree '" + source.string() + "': " + ec.message());
    return destination;
}

CommandResult run_process(const fs::path& exe_path,
                          const fs::path& cwd,
                          const std::vector<std::string>& args,
                          std::string_view stdin_data = {}) {
    CommandResult result;
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
        result.output = "CreatePipe failed: " + win32_error_message(GetLastError());
        return result;
    }
    (void)SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    HANDLE stdin_read = nullptr;
    HANDLE stdin_write = nullptr;
    if (!stdin_data.empty()) {
        if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0)) {
            result.output = "CreatePipe(stdin) failed: " + win32_error_message(GetLastError());
            CloseHandle(read_pipe);
            CloseHandle(write_pipe);
            return result;
        }
        (void)SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_data.empty() ? GetStdHandle(STD_INPUT_HANDLE) : stdin_read;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;

    PROCESS_INFORMATION pi{};
    std::wstring command_line = build_windows_command_line(exe_path, args);
    std::vector<wchar_t> mutable_cmd(command_line.begin(), command_line.end());
    mutable_cmd.push_back(L'\0');

    std::wstring cwd_w = cwd.wstring();
    const BOOL created = CreateProcessW(
        exe_path.wstring().c_str(),
        mutable_cmd.data(),
        nullptr,
        nullptr,
        TRUE,
        0,
        nullptr,
        cwd_w.c_str(),
        &si,
        &pi);

    CloseHandle(write_pipe);
    if (stdin_read != nullptr) CloseHandle(stdin_read);
    if (!created) {
        result.output = "CreateProcessW failed: " + win32_error_message(GetLastError());
        CloseHandle(read_pipe);
        if (stdin_write != nullptr) CloseHandle(stdin_write);
        return result;
    }

    if (stdin_write != nullptr) {
        std::size_t written_total = 0;
        while (written_total < stdin_data.size()) {
            const std::size_t remaining = stdin_data.size() - written_total;
            const auto chunk_size = static_cast<DWORD>(
                (std::min)(remaining, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD written = 0;
            const BOOL ok = WriteFile(
                stdin_write,
                stdin_data.data() + written_total,
                chunk_size,
                &written,
                nullptr);
            if (!ok) break;
            written_total += static_cast<std::size_t>(written);
            if (written == 0) break;
        }
        CloseHandle(stdin_write);
    }

    std::string output;
    std::array<char, 4096> buffer{};
    while (true) {
        DWORD bytes_read = 0;
        const BOOL ok = ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()),
                                 &bytes_read, nullptr);
        if (!ok || bytes_read == 0) break;
        output.append(buffer.data(), buffer.data() + bytes_read);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(read_pipe);

    result.exit_code = static_cast<int>(exit_code);
    result.output = std::move(output);
    return result;
#else
    int out_pipe[2];
    if (pipe(out_pipe) != 0) {
        result.output = "pipe(stdout) failed";
        return result;
    }

    int in_pipe[2]{-1, -1};
    if (!stdin_data.empty() && pipe(in_pipe) != 0) {
        result.output = "pipe(stdin) failed";
        close(out_pipe[0]);
        close(out_pipe[1]);
        return result;
    }

    std::vector<std::string> argv_storage;
    auto exec_argv = make_exec_argv(exe_path, args, argv_storage);

    const pid_t pid = fork();
    if (pid == -1) {
        result.output = "fork() failed";
        close(out_pipe[0]);
        close(out_pipe[1]);
        if (in_pipe[0] != -1) close(in_pipe[0]);
        if (in_pipe[1] != -1) close(in_pipe[1]);
        return result;
    }

    if (pid == 0) {
        close(out_pipe[0]);
        if (!stdin_data.empty()) {
            close(in_pipe[1]);
            dup2(in_pipe[0], STDIN_FILENO);
            close(in_pipe[0]);
        }
        if (chdir(cwd.c_str()) != 0) _exit(127);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(out_pipe[1]);
        execv(exe_path.c_str(), exec_argv.data());
        _exit(127);
    }

    close(out_pipe[1]);
    if (!stdin_data.empty()) {
        close(in_pipe[0]);
        std::size_t written_total = 0;
        while (written_total < stdin_data.size()) {
            const auto remaining =
                static_cast<std::size_t>(stdin_data.size() - written_total);
            const ssize_t written = write(
                in_pipe[1],
                stdin_data.data() + written_total,
                remaining);
            if (written <= 0) break;
            written_total += static_cast<std::size_t>(written);
        }
        close(in_pipe[1]);
    }

    std::string output;
    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t n = read(out_pipe[0], buffer.data(), buffer.size());
        if (n <= 0) break;
        output.append(buffer.data(), buffer.data() + static_cast<std::size_t>(n));
    }
    close(out_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
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

CommandResult run_eta(const fs::path& cwd, const std::vector<std::string>& args) {
    return run_process(kEtaCliPath, cwd, args);
}

CommandResult run_eta_repl(const fs::path& cwd, std::string_view stdin_data) {
    return run_process(kEtaReplPath, cwd, {}, stdin_data);
}

std::string make_manifest(const std::string& name,
                          const std::string& version,
                          const std::string& deps_table = "") {
    std::string manifest;
    manifest += "[package]\n";
    manifest += "name = \"" + name + "\"\n";
    manifest += "version = \"" + version + "\"\n";
    manifest += "license = \"MIT\"\n\n";
    manifest += "[compatibility]\n";
    manifest += "eta = \">=0.6, <0.8\"\n\n";
    manifest += "[dependencies]\n";
    manifest += deps_table;
    return manifest;
}

[[nodiscard]] std::string normalize_cli_output(std::string text) {
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    while (!text.empty() && text.back() == '\n') {
        text.pop_back();
    }
    return text;
}

[[nodiscard]] bool ascii_digits(std::string_view text) {
    if (text.empty()) return false;
    return std::all_of(
        text.begin(),
        text.end(),
        [](const char ch) {
            return std::isdigit(static_cast<unsigned char>(ch)) != 0;
        });
}

[[nodiscard]] bool version_core_triplet(std::string_view text) {
    const auto first = text.find('.');
    if (first == std::string_view::npos) return false;
    const auto second = text.find('.', first + 1u);
    if (second == std::string_view::npos) return false;
    if (text.find('.', second + 1u) != std::string_view::npos) return false;

    return ascii_digits(text.substr(0u, first))
        && ascii_digits(text.substr(first + 1u, second - first - 1u))
        && ascii_digits(text.substr(second + 1u));
}

[[nodiscard]] bool version_suffix_is_valid(std::string_view suffix) {
    if (suffix.empty()) return true;
    if (suffix.front() != '-' && suffix.front() != '+') return false;
    if (suffix.size() == 1u) return false;
    for (std::size_t i = 1; i < suffix.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(suffix[i]);
        if (std::isalnum(ch) || suffix[i] == '.' || suffix[i] == '-') continue;
        return false;
    }
    return true;
}

[[nodiscard]] bool looks_like_cli_version_token(std::string_view token) {
    if (token.empty()) return false;
    if (token.front() == 'v') token.remove_prefix(1u);

    const auto suffix_pos = token.find_first_of("-+");
    if (suffix_pos == std::string_view::npos) {
        return version_core_triplet(token);
    }

    return version_core_triplet(token.substr(0u, suffix_pos))
        && version_suffix_is_valid(token.substr(suffix_pos));
}

[[nodiscard]] std::string host_native_target_triple() {
#if defined(_WIN32)
#if defined(_M_X64) || defined(__x86_64__)
    return "x86_64-pc-windows-msvc";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "aarch64-pc-windows-msvc";
#else
    return "unknown-pc-windows-msvc";
#endif
#elif defined(__APPLE__)
#if defined(__aarch64__)
    return "aarch64-apple-darwin";
#elif defined(__x86_64__)
    return "x86_64-apple-darwin";
#else
    return "unknown-apple-darwin";
#endif
#elif defined(__linux__)
#if defined(__x86_64__)
    return "x86_64-unknown-linux-gnu";
#elif defined(__aarch64__)
    return "aarch64-unknown-linux-gnu";
#else
    return "unknown-unknown-linux-gnu";
#endif
#else
    return "unknown-unknown-unknown";
#endif
}

[[nodiscard]] std::string host_native_artifact_relpath(std::string_view basename) {
#if defined(_WIN32)
    return "native/windows-x64/eta_" + std::string(basename) + ".dll";
#elif defined(__APPLE__)
#if defined(__aarch64__)
    return "native/macos-arm64/libeta_" + std::string(basename) + ".dylib";
#else
    return "native/macos-x64/libeta_" + std::string(basename) + ".dylib";
#endif
#elif defined(__linux__)
#if defined(__aarch64__)
    return "native/linux-arm64/libeta_" + std::string(basename) + ".so";
#else
    return "native/linux-x64/libeta_" + std::string(basename) + ".so";
#endif
#else
    return "native/unknown/libeta_" + std::string(basename) + ".bin";
#endif
}

[[nodiscard]] std::string compute_fixture_sha256(const fs::path& file_path) {
#ifdef _WIN32
    wchar_t system_dir[MAX_PATH] = {};
    const UINT dir_len = GetSystemDirectoryW(system_dir, MAX_PATH);
    BOOST_REQUIRE_MESSAGE(
        dir_len > 0u && dir_len < MAX_PATH,
        "failed to resolve Windows system directory for certutil");
    const fs::path certutil_path = fs::path(std::wstring(system_dir, dir_len)) / "certutil.exe";

    const auto result = run_process(
        certutil_path,
        file_path.parent_path(),
        {"-hashfile", file_path.string(), "SHA256"});
    BOOST_REQUIRE_MESSAGE(
        result.exit_code == 0,
        "failed to hash fixture sidecar via certutil: " + result.output);

    std::istringstream in(result.output);
    std::string line;
    while (std::getline(in, line)) {
        std::string hex_line;
        bool invalid_line = false;
        for (const char c : line) {
            const auto uc = static_cast<unsigned char>(c);
            if (std::isxdigit(uc)) {
                hex_line.push_back(
                    static_cast<char>(std::tolower(uc)));
                continue;
            }
            if (!std::isspace(uc)) {
                invalid_line = true;
                break;
            }
        }
        if (!invalid_line && hex_line.size() == 64u) {
            return hex_line;
        }
    }

    BOOST_FAIL("failed to parse SHA256 digest from certutil output");
    return {};
#else
    const auto digest = eta::native::compute_sidecar_sha256(file_path);
    if (!digest.has_value()) {
        BOOST_FAIL("failed to hash staged sidecar artifact '" + file_path.string()
                   + "': " + digest.error().message);
    }
    return *digest;
#endif
}

[[nodiscard]] std::string stage_fixture_sidecar_artifact(const fs::path& package_root,
                                                         const std::string& artifact_relpath) {
    BOOST_REQUIRE_MESSAGE(
        fs::is_regular_file(kNativeSidecarFixturePath),
        "missing native sidecar fixture binary: " + kNativeSidecarFixturePath.string());

    const auto artifact_path = package_root / fs::path(artifact_relpath);
    fs::create_directories(artifact_path.parent_path());
    std::error_code copy_ec;
    fs::copy_file(kNativeSidecarFixturePath,
                  artifact_path,
                  fs::copy_options::overwrite_existing,
                  copy_ec);
    BOOST_REQUIRE_MESSAGE(!copy_ec,
                          "failed to copy sidecar fixture to " + artifact_path.string()
                              + ": " + copy_ec.message());

    return compute_fixture_sha256(artifact_path);
}

void configure_fixture_existing_native_sidecar_package(
    const fs::path& package_root,
    std::string_view artifact_basename,
    std::optional<std::string_view> sha_override = std::nullopt) {
    const std::string triple = host_native_target_triple();
    const std::string artifact_relpath = host_native_artifact_relpath(artifact_basename);
    const std::string artifact_sha = stage_fixture_sidecar_artifact(
        package_root, artifact_relpath);
    const std::string manifest_sha = sha_override.has_value()
        ? std::string(*sha_override)
        : artifact_sha;

    const auto manifest_path = package_root / "eta.toml";
    const std::string manifest_text = read_text_file(manifest_path);
    const auto targets_marker = manifest_text.find("[[native.targets]]");
    BOOST_REQUIRE_MESSAGE(
        targets_marker != std::string::npos,
        "manifest is missing [[native.targets]]: " + manifest_path.string());

    std::string prefix = manifest_text.substr(0, targets_marker);
    while (!prefix.empty() && (prefix.back() == '\n' || prefix.back() == '\r')) {
        prefix.pop_back();
    }
    const auto native_id_pos = prefix.find("id = \"");
    BOOST_REQUIRE_MESSAGE(
        native_id_pos != std::string::npos,
        "manifest is missing [native].id: " + manifest_path.string());
    const auto native_id_line_end = prefix.find('\n', native_id_pos);
    BOOST_REQUIRE_MESSAGE(
        native_id_line_end != std::string::npos,
        "manifest [native].id line is unterminated: " + manifest_path.string());
    prefix.replace(
        native_id_pos,
        native_id_line_end - native_id_pos,
        "id = \"eta.test.sidecar\"");

    std::ofstream manifest_out(
        manifest_path, std::ios::out | std::ios::binary | std::ios::trunc);
    BOOST_REQUIRE_MESSAGE(
        manifest_out.is_open(),
        "failed to rewrite native sidecar fixture manifest: " + manifest_path.string());
    manifest_out << prefix << "\n\n"
                 << "[[native.targets]]\n"
                 << "triple = \"" << triple << "\"\n"
                 << "artifact = \"" << artifact_relpath << "\"\n"
                 << "sha256 = \"" << manifest_sha << "\"\n";
}

void configure_fixture_native_sidecar_dependency(
    const fs::path& dependency_root,
    std::optional<std::string_view> sha_override = std::nullopt) {
    const std::string triple = host_native_target_triple();
    const std::string artifact_relpath = host_native_artifact_relpath("standalone_lib_native");
    const std::string artifact_sha = stage_fixture_sidecar_artifact(
        dependency_root, artifact_relpath);
    const std::string manifest_sha = sha_override.has_value()
        ? std::string(*sha_override)
        : artifact_sha;

    std::ofstream manifest_out(
        dependency_root / "eta.toml", std::ios::out | std::ios::binary | std::ios::trunc);
    BOOST_REQUIRE_MESSAGE(
        manifest_out.is_open(),
        "failed to rewrite native sidecar fixture dependency manifest");
    manifest_out << "[package]\n"
                 << "name = \"standalone_lib\"\n"
                 << "version = \"0.1.0\"\n"
                 << "license = \"MIT\"\n\n"
                 << "[compatibility]\n"
                 << "eta = \">=0.6, <0.8\"\n\n"
                 << "[dependencies]\n\n"
                 << "[native]\n"
                 << "kind = \"sidecar\"\n"
                 << "abi = \"eta-native-v1\"\n"
                 << "id = \"eta.test.sidecar\"\n"
                 << "entry = \"eta_register_extension_v1\"\n\n"
                 << "[[native.targets]]\n"
                 << "triple = \"" << triple << "\"\n"
                 << "artifact = \"" << artifact_relpath << "\"\n"
                 << "sha256 = \"" << manifest_sha << "\"\n";
}

void strip_native_section(const fs::path& package_root) {
    const auto manifest_path = package_root / "eta.toml";
    const std::string manifest_text = read_text_file(manifest_path);
    const auto native_section = manifest_text.find("\n[native]\n");
    BOOST_REQUIRE_MESSAGE(
        native_section != std::string::npos,
        "manifest is missing [native] section: " + manifest_path.string());

    std::string trimmed = manifest_text.substr(0, native_section);
    while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r')) {
        trimmed.pop_back();
    }
    trimmed += "\n";

    std::ofstream manifest_out(
        manifest_path, std::ios::out | std::ios::binary | std::ios::trunc);
    BOOST_REQUIRE_MESSAGE(
        manifest_out.is_open(),
        "failed to rewrite manifest without native section: " + manifest_path.string());
    manifest_out << trimmed;
}

} // namespace

BOOST_AUTO_TEST_SUITE(eta_cli_test)

BOOST_AUTO_TEST_CASE(new_scaffolds_expected_layout) {
    TempDir temp;
    const auto result = run_eta(temp.path, {"new", "hello_world", "--lib"});
    BOOST_REQUIRE_MESSAGE(result.exit_code == 0, result.output);

    const auto project_root = temp.path / "hello_world";
    BOOST_TEST(fs::is_directory(project_root));
    BOOST_TEST(fs::is_regular_file(project_root / "eta.toml"));
    BOOST_TEST(fs::is_regular_file(project_root / "src" / "hello_world.eta"));
    BOOST_TEST(fs::is_regular_file(project_root / "tests" / "smoke.test.eta"));
    BOOST_TEST(fs::is_regular_file(project_root / ".gitignore"));
    BOOST_TEST(fs::is_regular_file(project_root / "README.md"));

    const auto manifest = read_text_file(project_root / "eta.toml");
    BOOST_TEST(manifest.find("name = \"hello_world\"") != std::string::npos);
    BOOST_TEST(manifest.find("[dependencies]") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(init_scaffolds_current_directory) {
    TempDir temp;
    const auto result = run_eta(temp.path, {"init", "--bin"});
    BOOST_REQUIRE_MESSAGE(result.exit_code == 0, result.output);

    const auto inferred_name = temp.path.filename().string();
    BOOST_TEST(fs::is_regular_file(temp.path / "eta.toml"));
    BOOST_TEST(fs::is_regular_file(temp.path / "src" / (inferred_name + ".eta")));
    BOOST_TEST(fs::is_regular_file(temp.path / "tests" / "smoke.test.eta"));
}

BOOST_AUTO_TEST_CASE(version_flag_prints_output) {
    TempDir temp;
    const auto result = run_eta(temp.path, {"--version"});
    BOOST_REQUIRE_MESSAGE(result.exit_code == 0, result.output);

    const auto normalized = normalize_cli_output(result.output);
    BOOST_TEST(!normalized.empty());
    BOOST_TEST(normalized.rfind("eta ", 0u) == 0u);
}

BOOST_AUTO_TEST_CASE(version_flag_matches_expected_tag_format) {
    TempDir temp;
    const auto result = run_eta(temp.path, {"--version"});
    BOOST_REQUIRE_MESSAGE(result.exit_code == 0, result.output);

    const auto normalized = normalize_cli_output(result.output);
    BOOST_REQUIRE_MESSAGE(normalized.rfind("eta ", 0u) == 0u, normalized);

    const auto version_token = std::string_view(normalized).substr(4u);
    BOOST_TEST(looks_like_cli_version_token(version_token));
}

BOOST_AUTO_TEST_CASE(version_flag_is_stable_across_layouts) {
    TempDir temp;
    const auto workspace_root = copy_fixture_tree(
        temp,
        fs::path("workspace") / "virtual_root");

    const auto first = run_eta(temp.path, {"--version"});
    const auto second = run_eta(workspace_root / "packages" / "app", {"--version"});
    BOOST_REQUIRE_MESSAGE(first.exit_code == 0, first.output);
    BOOST_REQUIRE_MESSAGE(second.exit_code == 0, second.output);

    BOOST_TEST(normalize_cli_output(first.output) == normalize_cli_output(second.output));
}

BOOST_AUTO_TEST_CASE(tree_output_is_deterministic_for_path_deps) {
    TempDir temp;
    temp.write_file("app/eta.toml",
                    make_manifest(
                        "app",
                        "1.0.0",
                        "beta = { path = \"../beta\" }\n"
                        "alpha = { path = \"../alpha\" }\n"));

    temp.write_file("alpha/eta.toml",
                    make_manifest("alpha", "0.1.0",
                                  "gamma = { path = \"../gamma\" }\n"));
    temp.write_file("beta/eta.toml", make_manifest("beta", "0.2.0"));
    temp.write_file("gamma/eta.toml", make_manifest("gamma", "0.3.0"));

    const auto first = run_eta(temp.path / "app", {"tree"});
    const auto second = run_eta(temp.path / "app", {"tree"});
    BOOST_REQUIRE_MESSAGE(first.exit_code == 0, first.output);
    BOOST_REQUIRE_MESSAGE(second.exit_code == 0, second.output);
    BOOST_TEST(first.output == second.output);

    const auto app_pos = first.output.find("app v1.0.0");
    const auto alpha_pos = first.output.find("alpha v0.1.0");
    const auto beta_pos = first.output.find("beta v0.2.0");
    const auto gamma_pos = first.output.find("gamma v0.3.0");
    BOOST_TEST(app_pos != std::string::npos);
    BOOST_TEST(alpha_pos != std::string::npos);
    BOOST_TEST(beta_pos != std::string::npos);
    BOOST_TEST(gamma_pos != std::string::npos);
    BOOST_TEST(app_pos < alpha_pos);
    BOOST_TEST(alpha_pos < gamma_pos);
    BOOST_TEST(gamma_pos < beta_pos);
}

BOOST_AUTO_TEST_CASE(tree_from_native_sidecar_standalone_fixture_uses_package_manifest) {
    TempDir temp;
    const auto fixture_root = copy_fixture_tree(
        temp,
        fs::path("native_sidecar") / "standalone_app");
    const auto lib_source = fixture_path(
        fs::path("native_sidecar") / "standalone_lib");
    const auto lib_destination = fixture_root.parent_path() / "standalone_lib";
    std::error_code ec;
    fs::copy(lib_source, lib_destination, fs::copy_options::recursive, ec);
    BOOST_REQUIRE_MESSAGE(!ec,
                          "failed to copy standalone lib fixture '"
                              + lib_source.string() + "': " + ec.message());

    const auto result = run_eta(fixture_root, {"tree"});
    BOOST_REQUIRE_MESSAGE(result.exit_code == 0, result.output);
    BOOST_TEST(result.output.find("standalone_app v0.1.0") != std::string::npos);
    BOOST_TEST(result.output.find("standalone_lib v0.1.0") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(tree_from_workspace_member_fixture_uses_member_manifest) {
    TempDir temp;
    const auto workspace_root = copy_fixture_tree(
        temp,
        fs::path("workspace") / "virtual_root");

    const auto result = run_eta(workspace_root / "packages" / "app", {"tree"});
    BOOST_REQUIRE_MESSAGE(result.exit_code == 0, result.output);
    BOOST_TEST(result.output.find("app v0.1.0") != std::string::npos);
    BOOST_TEST(result.output.find("lib v0.1.0") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(tree_from_nested_workspace_member_fixture_uses_member_manifest) {
    TempDir temp;
    const auto workspace_root = copy_fixture_tree(
        temp,
        fs::path("workspace") / "virtual_root");

    const auto result = run_eta(workspace_root / "packages" / "app" / "src", {"tree"});
    BOOST_REQUIRE_MESSAGE(result.exit_code == 0, result.output);
    BOOST_TEST(result.output.find("app v0.1.0") != std::string::npos);
    BOOST_TEST(result.output.find("lib v0.1.0") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(tree_from_virtual_workspace_root_fixture_uses_workspace_defaults) {
    TempDir temp;
    const auto workspace_root = copy_fixture_tree(
        temp,
        fs::path("workspace") / "virtual_root");

    const auto result = run_eta(workspace_root, {"tree"});
    BOOST_REQUIRE_MESSAGE(result.exit_code == 0, result.output);
    BOOST_TEST(result.output.find("app v0.1.0") != std::string::npos);
    BOOST_TEST(result.output.find("lib v0.1.0") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(tree_from_native_sidecar_virtual_workspace_root_uses_workspace_defaults) {
    TempDir temp;
    const auto workspace_root = copy_fixture_tree(
        temp,
        fs::path("native_sidecar") / "workspace" / "virtual_root");

    const auto result = run_eta(workspace_root, {"tree"});
    BOOST_REQUIRE_MESSAGE(result.exit_code == 0, result.output);
    BOOST_TEST(result.output.find("app v0.1.0") != std::string::npos);
    BOOST_TEST(result.output.find("lib v0.1.0") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(tree_from_workspace_non_member_dir_uses_workspace_defaults) {
    TempDir temp;
    const auto workspace_root = copy_fixture_tree(
        temp,
        fs::path("workspace") / "virtual_root");
    fs::create_directories(workspace_root / "notes" / "drafts");

    const auto result = run_eta(workspace_root / "notes" / "drafts", {"tree"});
    BOOST_REQUIRE_MESSAGE(result.exit_code == 0, result.output);
    BOOST_TEST(result.output.find("app v0.1.0") != std::string::npos);
    BOOST_TEST(result.output.find("lib v0.1.0") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(tree_with_manifest_path_selects_workspace_member) {
    TempDir temp;
    const auto workspace_root = copy_fixture_tree(
        temp,
        fs::path("workspace") / "virtual_root");
    const auto workspace_manifest = workspace_root / "eta.toml";

    const auto result = run_eta(temp.path,
                                {"tree",
                                 "--manifest-path",
                                 workspace_manifest.string(),
                                 "-p",
                                 "app"});
    BOOST_REQUIRE_MESSAGE(result.exit_code == 0, result.output);
    BOOST_TEST(result.output.find("app v0.1.0") != std::string::npos);
    BOOST_TEST(result.output.find("lib v0.1.0") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(build_from_rooted_workspace_root_fixture_keeps_root_package_behavior) {
    TempDir temp;
    const auto workspace_root = copy_fixture_tree(
        temp,
        fs::path("workspace") / "rooted_root");

    const auto build = run_eta(workspace_root, {"build"});
    BOOST_REQUIRE_MESSAGE(build.exit_code == 0, build.output);

    const auto root_artifact =
        workspace_root / ".eta" / "target" / "release" / "root_tools" / "root_tools.etac";
    const auto member_artifact =
        workspace_root / ".eta" / "target" / "release" / "helper" / "helper.etac";
    BOOST_TEST(fs::is_regular_file(root_artifact));
    BOOST_TEST(!fs::exists(member_artifact));
}

BOOST_AUTO_TEST_CASE(build_from_native_sidecar_rooted_workspace_root_keeps_root_package_behavior) {
    TempDir temp;
    const auto workspace_root = copy_fixture_tree(
        temp,
        fs::path("native_sidecar") / "workspace" / "rooted_root");
    configure_fixture_existing_native_sidecar_package(
        workspace_root, "root_tools_native");

    const auto build = run_eta(workspace_root, {"build"});
    BOOST_REQUIRE_MESSAGE(build.exit_code == 0, build.output);

    const auto root_artifact =
        workspace_root / ".eta" / "target" / "release" / "root_tools" / "root_tools.etac";
    const auto member_artifact =
        workspace_root / ".eta" / "target" / "release" / "helper" / "helper.etac";
    BOOST_TEST(fs::is_regular_file(root_artifact));
    BOOST_TEST(!fs::exists(member_artifact));
}

BOOST_AUTO_TEST_CASE(build_from_workspace_member_writes_workspace_lockfile_and_shared_modules_root) {
    TempDir temp;
    const auto workspace_root = copy_fixture_tree(
        temp,
        fs::path("workspace") / "virtual_root");

    const auto build = run_eta(workspace_root / "packages" / "app", {"build"});
    BOOST_REQUIRE_MESSAGE(build.exit_code == 0, build.output);

    const auto app_artifact =
        workspace_root / ".eta" / "target" / "release" / "app" / "app.etac";
    BOOST_TEST(fs::is_regular_file(app_artifact));
    BOOST_TEST(!fs::exists(workspace_root / "packages" / "app" / ".eta" / "target"));

    const auto workspace_lock = workspace_root / "eta.lock";
    const auto member_lock = workspace_root / "packages" / "app" / "eta.lock";
    BOOST_TEST(fs::is_regular_file(workspace_lock));
    BOOST_TEST(!fs::exists(member_lock));

    const auto lock_text = read_text_file(workspace_lock);
    BOOST_TEST(lock_text.find("source = \"workspace+packages/app\"") != std::string::npos);
    BOOST_TEST(lock_text.find("source = \"workspace+packages/lib\"") != std::string::npos);

    BOOST_TEST(fs::is_directory(workspace_root / ".eta" / "modules"));
    BOOST_TEST(!fs::exists(workspace_root / "packages" / "app" / ".eta" / "modules"));
}

BOOST_AUTO_TEST_CASE(
    build_from_native_sidecar_workspace_member_writes_workspace_lockfile_and_shared_modules_root) {
    TempDir temp;
    const auto workspace_root = copy_fixture_tree(
        temp,
        fs::path("native_sidecar") / "workspace" / "virtual_root");
    configure_fixture_existing_native_sidecar_package(
        workspace_root / "packages" / "app", "app_native");

    const auto build = run_eta(workspace_root / "packages" / "app", {"build"});
    BOOST_REQUIRE_MESSAGE(build.exit_code == 0, build.output);

    const auto app_artifact =
        workspace_root / ".eta" / "target" / "release" / "app" / "app.etac";
    BOOST_TEST(fs::is_regular_file(app_artifact));
    BOOST_TEST(!fs::exists(workspace_root / "packages" / "app" / ".eta" / "target"));

    const auto workspace_lock = workspace_root / "eta.lock";
    const auto member_lock = workspace_root / "packages" / "app" / "eta.lock";
    BOOST_TEST(fs::is_regular_file(workspace_lock));
    BOOST_TEST(!fs::exists(member_lock));

    const auto lock_text = read_text_file(workspace_lock);
    BOOST_TEST(lock_text.find("source = \"workspace+packages/app\"") != std::string::npos);
    BOOST_TEST(lock_text.find("source = \"workspace+packages/lib\"") != std::string::npos);

    BOOST_TEST(fs::is_directory(workspace_root / ".eta" / "modules"));
    BOOST_TEST(!fs::exists(workspace_root / "packages" / "app" / ".eta" / "modules"));
}

BOOST_AUTO_TEST_CASE(build_from_virtual_workspace_root_with_workspace_flag_builds_all_members) {
    TempDir temp;
    const auto workspace_root = copy_fixture_tree(
        temp,
        fs::path("workspace") / "virtual_root");

    const auto build = run_eta(workspace_root, {"build", "--workspace"});
    BOOST_REQUIRE_MESSAGE(build.exit_code == 0, build.output);

    const auto app_artifact =
        workspace_root / ".eta" / "target" / "release" / "app" / "app.etac";
    const auto lib_artifact =
        workspace_root / ".eta" / "target" / "release" / "lib" / "lib.etac";
    BOOST_TEST(fs::is_regular_file(app_artifact));
    BOOST_TEST(fs::is_regular_file(lib_artifact));
}

BOOST_AUTO_TEST_CASE(run_from_virtual_workspace_root_requires_explicit_package) {
    TempDir temp;
    const auto workspace_root = copy_fixture_tree(
        temp,
        fs::path("workspace") / "virtual_root");

    const auto run = run_eta(workspace_root, {"run"});
    BOOST_REQUIRE_NE(run.exit_code, 0);
    BOOST_TEST(run.output.find("virtual workspace commands require -p/--package") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(run_from_workspace_non_member_dir_requires_explicit_package) {
    TempDir temp;
    const auto workspace_root = copy_fixture_tree(
        temp,
        fs::path("workspace") / "virtual_root");
    fs::create_directories(workspace_root / "notes" / "drafts");

    const auto run = run_eta(workspace_root / "notes" / "drafts", {"run"});
    BOOST_REQUIRE_NE(run.exit_code, 0);
    BOOST_TEST(run.output.find("workspace non-member directories require -p/--package")
               != std::string::npos);
}

BOOST_AUTO_TEST_CASE(run_from_virtual_workspace_root_with_package_target_runs_member) {
    TempDir temp;
    const auto workspace_root = copy_fixture_tree(
        temp,
        fs::path("workspace") / "virtual_root");

    const auto run = run_eta(workspace_root, {"run", "-p", "app"});
    BOOST_REQUIRE_MESSAGE(run.exit_code == 0, run.output);
    BOOST_TEST(run.output.find("42") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(install_from_virtual_workspace_root_requires_explicit_package_target) {
    TempDir temp;
    const auto workspace_root = copy_fixture_tree(
        temp,
        fs::path("workspace") / "virtual_root");

    const auto install = run_eta(workspace_root, {"install"});
    BOOST_REQUIRE_NE(install.exit_code, 0);
    BOOST_TEST(install.output.find("virtual workspace commands require -p/--package")
               != std::string::npos);
}

BOOST_AUTO_TEST_CASE(install_from_virtual_workspace_root_with_package_target_installs_member_artifact) {
    TempDir temp;
    const auto workspace_root = copy_fixture_tree(
        temp,
        fs::path("workspace") / "virtual_root");

    const auto install = run_eta(workspace_root, {"install", "-p", "app"});
    BOOST_REQUIRE_MESSAGE(install.exit_code == 0, install.output);

    const fs::path shared_artifact =
        workspace_root / ".eta" / "target" / "release" / "app" / "app.etac";
    const fs::path installed_artifact =
        workspace_root / "packages" / "app" / ".eta" / "bin" / "app.etac";
    BOOST_TEST(fs::is_regular_file(shared_artifact));
    BOOST_TEST(fs::is_regular_file(installed_artifact));
}

BOOST_AUTO_TEST_CASE(run_without_manifest_degrades_to_etai) {
    TempDir temp;
    temp.write_file("standalone.eta", R"eta(
(module eta.run.compat
  (begin
    (display "eta-run-ok")
    (newline)))
)eta");

    const auto result = run_eta(temp.path, {"run", "standalone.eta"});
    BOOST_REQUIRE_MESSAGE(result.exit_code == 0, result.output);
    BOOST_TEST(result.output.find("eta-run-ok") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(prof_run_defaults_to_sampling_speedscope_output) {
    TempDir temp;
    temp.write_file("sampled.eta", R"eta(
(module eta.prof.sample
  (begin
    (define (slow n)
      (if (= n 0)
          0
          (begin
            (%time-sleep-ms 2)
            (slow (- n 1)))))
    (define result (slow 4))))
)eta");

    const auto result = run_eta(temp.path, {"prof", "run", "sampled.eta"});
    BOOST_REQUIRE_MESSAGE(result.exit_code == 0, result.output);
    BOOST_TEST(result.output.find("\"$schema\":\"https://www.speedscope.app/file-format-schema.json\"")
               != std::string::npos);
    BOOST_TEST(result.output.find("\"type\":\"sampled\"") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(run_prof_flag_defaults_to_sampling_speedscope_output) {
    TempDir temp;
    temp.write_file("sampled.eta", R"eta(
(module eta.run.prof.sample
  (begin
    (define (slow n)
      (if (= n 0)
          0
          (begin
            (%time-sleep-ms 2)
            (slow (- n 1)))))
    (define result (slow 4))))
)eta");

    const auto result = run_eta(temp.path, {"run", "--prof", "sampled.eta"});
    BOOST_REQUIRE_MESSAGE(result.exit_code == 0, result.output);
    BOOST_TEST(result.output.find("\"$schema\":\"https://www.speedscope.app/file-format-schema.json\"")
               != std::string::npos);
    BOOST_TEST(result.output.find("\"type\":\"sampled\"") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(prof_run_can_write_eta_prof_archive) {
    TempDir temp;
    temp.write_file("trace.eta", R"eta(
(module eta.prof.archive
  (begin
    (define (countdown n)
      (if (= n 0) 0 (+ 1 (countdown (- n 1)))))
    (define result (countdown 8))))
)eta");

    const fs::path out_file = temp.path / "trace.eta-prof";
    const auto run = run_eta(temp.path,
                             {"prof", "run",
                              "--mode", "trace",
                              "--format", "eta-prof",
                              "--out", out_file.string(),
                              "trace.eta"});
    BOOST_REQUIRE_MESSAGE(run.exit_code == 0, run.output);
    BOOST_TEST(fs::is_regular_file(out_file));

    const auto archive = read_text_file(out_file);
    BOOST_TEST(archive.find("\"format\":\"eta-prof\"") != std::string::npos);
    BOOST_TEST(archive.find("\"mode\":\"trace\"") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(prof_report_reads_eta_prof_archive) {
    TempDir temp;
    temp.write_file("trace.eta", R"eta(
(module eta.prof.report
  (begin
    (define (work n)
      (if (= n 0) 0 (+ 1 (work (- n 1)))))
    (define result (work 6))))
)eta");

    const fs::path out_file = temp.path / "trace.eta-prof";
    const auto run = run_eta(temp.path,
                             {"prof", "run",
                              "--mode", "trace",
                              "--format", "eta-prof",
                              "--out", out_file.string(),
                              "trace.eta"});
    BOOST_REQUIRE_MESSAGE(run.exit_code == 0, run.output);

    const auto report = run_eta(temp.path, {"prof", "report", out_file.string()});
    BOOST_REQUIRE_MESSAGE(report.exit_code == 0, report.output);
    BOOST_TEST(report.output.find("Profiler summary") != std::string::npos);
    BOOST_TEST(report.output.find("eta.prof.report:work") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(prof_merge_combines_archive_counters) {
    TempDir temp;
    temp.write_file("trace.eta", R"eta(
(module eta.prof.merge
  (begin
    (%prof-counter "runs" 1)
    (define (work n)
      (if (= n 0) 0 (+ 1 (work (- n 1)))))
    (define result (work 4))))
)eta");

    const fs::path first = temp.path / "first.eta-prof";
    const fs::path second = temp.path / "second.eta-prof";
    const fs::path merged = temp.path / "merged.eta-prof";

    const auto run_first = run_eta(temp.path,
                                   {"prof", "run",
                                    "--mode", "trace",
                                    "--format", "eta-prof",
                                    "--out", first.string(),
                                    "trace.eta"});
    BOOST_REQUIRE_MESSAGE(run_first.exit_code == 0, run_first.output);

    const auto run_second = run_eta(temp.path,
                                    {"prof", "run",
                                     "--mode", "trace",
                                     "--format", "eta-prof",
                                     "--out", second.string(),
                                     "trace.eta"});
    BOOST_REQUIRE_MESSAGE(run_second.exit_code == 0, run_second.output);

    const auto merge = run_eta(temp.path,
                               {"prof", "merge",
                                "--out", merged.string(),
                                first.string(),
                                second.string()});
    BOOST_REQUIRE_MESSAGE(merge.exit_code == 0, merge.output);
    BOOST_TEST(fs::is_regular_file(merged));

    const auto merged_json = run_eta(temp.path,
                                     {"prof", "report",
                                      "--format", "json",
                                      merged.string()});
    BOOST_REQUIRE_MESSAGE(merged_json.exit_code == 0, merged_json.output);
    BOOST_TEST(merged_json.output.find("\"runs\":2") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(repl_prof_profiles_next_submission) {
    TempDir temp;
    const std::string input = ":prof trace --format pretty\n"
                              "(+ 1 2)\n"
                              "(exit)\n";
    const auto result = run_eta_repl(temp.path, input);
    BOOST_REQUIRE_MESSAGE(result.exit_code == 0, result.output);
    BOOST_TEST(result.output.find("[prof] next submission mode=trace hz=1000 format=pretty")
               != std::string::npos);
    BOOST_TEST(result.output.find("Profiler summary") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(repl_prof_rejects_invalid_hz) {
    TempDir temp;
    const auto result = run_eta_repl(temp.path, ":prof --hz 0\n(exit)\n");
    BOOST_REQUIRE_MESSAGE(result.exit_code == 0, result.output);
    BOOST_TEST(result.output.find(":prof: --hz must be a positive integer") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(run_manifest_mode_keeps_first_hit_semantics_by_default) {
    TempDir temp;

    temp.write_file("app/eta.toml",
                    make_manifest("app", "1.0.0", "dep = { path = \"../dep\" }\n"));
    temp.write_file("dep/eta.toml", make_manifest("dep", "0.1.0"));

    temp.write_file("app/src/app.eta", R"eta(
(module app
  (import std.io)
  (import dup.mod)
  (begin
    (display origin)
    (newline)))
)eta");
    temp.write_file("app/src/dup/mod.eta", R"eta(
(module dup.mod
  (export origin)
  (begin
    (define origin "project")))
)eta");
    temp.write_file("dep/src/dup/mod.eta", R"eta(
(module dup.mod
  (export origin)
  (begin
    (define origin "dependency")))
)eta");

    const auto result = run_eta(temp.path / "app", {"run"});
    BOOST_REQUIRE_MESSAGE(result.exit_code == 0, result.output);
    BOOST_TEST(result.output.find("project") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(run_manifest_mode_strict_shadow_scan_reports_duplicate_modules) {
    TempDir temp;

    temp.write_file("app/eta.toml",
                    make_manifest("app", "1.0.0", "dep = { path = \"../dep\" }\n"));
    temp.write_file("dep/eta.toml", make_manifest("dep", "0.1.0"));

    temp.write_file("app/src/app.eta", R"eta(
(module app
  (import std.io)
  (import dup.mod)
  (begin
    (display origin)
    (newline)))
)eta");
    temp.write_file("app/src/dup/mod.eta", R"eta(
(module dup.mod
  (export origin)
  (begin
    (define origin "project")))
)eta");
    temp.write_file("dep/src/dup/mod.eta", R"eta(
(module dup.mod
  (export origin)
  (begin
    (define origin "dependency")))
)eta");

    const auto result = run_eta(temp.path / "app", {"run", "--strict-shadows"});
    BOOST_REQUIRE_NE(result.exit_code, 0);
    BOOST_TEST(result.output.find("strict shadow mode") != std::string::npos);
    BOOST_TEST(result.output.find("dup.mod") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(add_from_virtual_workspace_root_requires_explicit_package_target) {
    TempDir temp;
    const auto workspace_root = copy_fixture_tree(
        temp,
        fs::path("workspace") / "virtual_root");

    const auto add = run_eta(workspace_root, {"add", "lib", "--path", "../lib"});
    BOOST_REQUIRE_NE(add.exit_code, 0);
    BOOST_TEST(add.output.find("virtual workspace commands require -p/--package")
               != std::string::npos);
}

BOOST_AUTO_TEST_CASE(add_from_virtual_workspace_root_with_package_target_updates_member_manifest) {
    TempDir temp;
    temp.write_file("ws/eta.toml", R"toml(
[workspace]
members = ["packages/*"]
)toml");
    temp.write_file("ws/packages/app/eta.toml", make_manifest("app", "0.1.0"));
    temp.write_file("ws/packages/lib/eta.toml", make_manifest("lib", "0.1.0"));
    temp.write_file("ws/packages/app/src/app.eta", R"eta(
(module app
  (begin
    (display "app")
    (newline)))
)eta");
    temp.write_file("ws/packages/lib/src/lib.eta", R"eta(
(module lib
  (export meaning)
  (begin
    (define meaning 7)))
)eta");

    const auto add = run_eta(temp.path / "ws",
                             {"add", "lib", "--path", "../lib", "-p", "app"});
    BOOST_REQUIRE_MESSAGE(add.exit_code == 0, add.output);

    const auto manifest_after_add =
        read_text_file(temp.path / "ws" / "packages" / "app" / "eta.toml");
    BOOST_TEST(manifest_after_add.find("lib = { path = \"../lib\" }") != std::string::npos);
    BOOST_TEST(fs::is_regular_file(temp.path / "ws" / "eta.lock"));
}

BOOST_AUTO_TEST_CASE(add_and_remove_path_dependency_updates_manifest_and_lockfile) {
    TempDir temp;
    temp.write_file("app/eta.toml", make_manifest("app", "1.0.0"));
    temp.write_file("app/src/app.eta", R"eta(
(module app
  (begin
    (display "app")
    (newline)))
)eta");
    temp.write_file("dep/eta.toml", make_manifest("dep", "0.1.0"));
    temp.write_file("dep/src/dep.eta", R"eta(
(module dep
  (export meaning)
  (begin
    (define meaning 42)))
)eta");

    const auto add = run_eta(temp.path / "app", {"add", "dep", "--path", "../dep"});
    BOOST_REQUIRE_MESSAGE(add.exit_code == 0, add.output);

    const auto manifest_after_add = read_text_file(temp.path / "app" / "eta.toml");
    const auto lock_after_add = read_text_file(temp.path / "app" / "eta.lock");
    BOOST_TEST(manifest_after_add.find("dep = { path = \"../dep\" }") != std::string::npos);
    BOOST_TEST(lock_after_add.find("name = \"dep\"") != std::string::npos);
    BOOST_TEST(lock_after_add.find("source = \"path+") != std::string::npos);

    const auto remove = run_eta(temp.path / "app", {"remove", "dep"});
    BOOST_REQUIRE_MESSAGE(remove.exit_code == 0, remove.output);
    const auto manifest_after_remove = read_text_file(temp.path / "app" / "eta.toml");
    BOOST_TEST(manifest_after_remove.find("dep = { path = \"../dep\" }") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(build_writes_release_etac_artifacts) {
    TempDir temp;
    temp.write_file("app/eta.toml", make_manifest("app", "1.0.0"));
    temp.write_file("app/src/app.eta", R"eta(
(module app
  (import std.io)
  (begin
    (println "build-smoke")))
)eta");

    const auto build = run_eta(temp.path / "app", {"build"});
    BOOST_REQUIRE_MESSAGE(build.exit_code == 0, build.output);

    const fs::path artifact = temp.path / "app" / ".eta" / "target" / "release" / "app.etac";
    BOOST_TEST(fs::is_regular_file(artifact));
}

BOOST_AUTO_TEST_CASE(test_command_runs_project_tests) {
    TempDir temp;
    temp.write_file("app/eta.toml", make_manifest("app", "1.0.0"));
    temp.write_file("app/src/app.eta", R"eta(
(module app
  (export answer)
  (begin
    (define answer 42)))
)eta");
    temp.write_file("app/tests/smoke.test.eta", R"eta(
(module app.tests.smoke
  (import std.test app)
  (begin
    (define suite
      (make-group "app"
        (list
          (make-test "answer"
            (lambda ()
              (assert-equal 42 answer))))))
    (print-tap (run suite))))
)eta");

    const auto test_run = run_eta(temp.path / "app", {"test"});
    BOOST_REQUIRE_MESSAGE(test_run.exit_code == 0, test_run.output);
    BOOST_TEST(test_run.output.find("TAP version 13") != std::string::npos);
    BOOST_TEST(test_run.output.find("ok 1 - answer") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(vendor_materializes_path_dependency_modules) {
    TempDir temp;
    temp.write_file("app/eta.toml",
                    make_manifest("app", "1.0.0", "dep = { path = \"../dep\" }\n"));
    temp.write_file("app/src/app.eta", R"eta(
(module app
  (import dep)
  (begin
    (display meaning)
    (newline)))
)eta");

    temp.write_file("dep/eta.toml", make_manifest("dep", "0.1.0"));
    temp.write_file("dep/src/dep.eta", R"eta(
(module dep
  (export meaning)
  (begin
    (define meaning 7)))
)eta");

    const auto vendor = run_eta(temp.path / "app", {"vendor"});
    BOOST_REQUIRE_MESSAGE(vendor.exit_code == 0, vendor.output);

    const fs::path dep_materialized = temp.path / "app" / ".eta" / "modules" / "dep-0.1.0" / "eta.toml";
    BOOST_TEST(fs::is_regular_file(dep_materialized));
}

BOOST_AUTO_TEST_CASE(
    vendor_build_and_run_with_native_sidecar_fixture_materializes_native_dependency) {
    TempDir temp;
    const auto fixture_root = copy_fixture_tree(
        temp,
        fs::path("native_sidecar") / "standalone_app");
    const auto lib_source = fixture_path(
        fs::path("native_sidecar") / "standalone_lib");
    const auto lib_destination = fixture_root.parent_path() / "standalone_lib";
    std::error_code ec;
    fs::copy(lib_source, lib_destination, fs::copy_options::recursive, ec);
    BOOST_REQUIRE_MESSAGE(!ec,
                          "failed to copy standalone lib fixture '"
                              + lib_source.string() + "': " + ec.message());

    strip_native_section(fixture_root);
    configure_fixture_native_sidecar_dependency(lib_destination);

    const auto vendor = run_eta(fixture_root, {"vendor"});
    BOOST_REQUIRE_MESSAGE(vendor.exit_code == 0, vendor.output);

    const std::string artifact_relpath = host_native_artifact_relpath("standalone_lib_native");
    const fs::path materialized_artifact =
        fixture_root / ".eta" / "modules" / "standalone_lib-0.1.0" / fs::path(artifact_relpath);
    BOOST_TEST(fs::is_regular_file(materialized_artifact));

    const auto lockfile_text = read_text_file(fixture_root / "eta.lock");
    BOOST_TEST(lockfile_text.find("name = \"standalone_lib\"") != std::string::npos);
    BOOST_TEST(lockfile_text.find("native_id = \"eta.test.sidecar\"") != std::string::npos);
    BOOST_TEST(lockfile_text.find("native_target_triple = \"" + host_native_target_triple() + "\"")
               != std::string::npos);
    BOOST_TEST(lockfile_text.find("native_artifact_relpath = \"" + artifact_relpath + "\"")
               != std::string::npos);

    const auto build = run_eta(fixture_root, {"build"});
    BOOST_REQUIRE_MESSAGE(build.exit_code == 0, build.output);
    BOOST_TEST(fs::is_regular_file(
        fixture_root / ".eta" / "target" / "release" / "standalone_app.etac"));

    const auto run = run_eta(fixture_root, {"run"});
    BOOST_REQUIRE_MESSAGE(run.exit_code == 0, run.output);
    BOOST_TEST(run.output.find("42") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(vendor_reports_native_sidecar_checksum_mismatch) {
    constexpr std::string_view kMismatchedSha256 =
        "0000000000000000000000000000000000000000000000000000000000000000";

    TempDir temp;
    const auto fixture_root = copy_fixture_tree(
        temp,
        fs::path("native_sidecar") / "standalone_app");
    const auto lib_source = fixture_path(
        fs::path("native_sidecar") / "standalone_lib");
    const auto lib_destination = fixture_root.parent_path() / "standalone_lib";
    std::error_code ec;
    fs::copy(lib_source, lib_destination, fs::copy_options::recursive, ec);
    BOOST_REQUIRE_MESSAGE(!ec,
                          "failed to copy standalone lib fixture '"
                              + lib_source.string() + "': " + ec.message());

    strip_native_section(fixture_root);
    configure_fixture_native_sidecar_dependency(
        lib_destination, kMismatchedSha256);

    const auto vendor = run_eta(fixture_root, {"vendor"});
    BOOST_REQUIRE_NE(vendor.exit_code, 0);
    BOOST_TEST(vendor.output.find("native sidecar checksum mismatch for package 'standalone_lib'")
               != std::string::npos);
}

BOOST_AUTO_TEST_CASE(vendor_from_workspace_root_resolves_relative_target_from_workspace_root) {
    TempDir temp;
    const auto workspace_root = copy_fixture_tree(
        temp,
        fs::path("workspace") / "virtual_root");

    const auto vendor = run_eta(workspace_root, {"vendor", "--target", "vendor-out"});
    BOOST_REQUIRE_MESSAGE(vendor.exit_code == 0, vendor.output);

    const fs::path workspace_target = workspace_root / "vendor-out";
    BOOST_TEST(fs::is_directory(workspace_target));
    BOOST_TEST(!fs::exists(workspace_root / "packages" / "app" / "vendor-out"));
}

BOOST_AUTO_TEST_CASE(clean_from_workspace_root_removes_selected_member_shared_target_artifacts) {
    TempDir temp;
    const auto workspace_root = copy_fixture_tree(
        temp,
        fs::path("workspace") / "virtual_root");

    const auto build = run_eta(workspace_root, {"build", "--workspace"});
    BOOST_REQUIRE_MESSAGE(build.exit_code == 0, build.output);

    const fs::path app_artifact =
        workspace_root / ".eta" / "target" / "release" / "app" / "app.etac";
    const fs::path lib_artifact =
        workspace_root / ".eta" / "target" / "release" / "lib" / "lib.etac";
    BOOST_TEST(fs::is_regular_file(app_artifact));
    BOOST_TEST(fs::is_regular_file(lib_artifact));

    const auto clean = run_eta(workspace_root, {"clean"});
    BOOST_REQUIRE_MESSAGE(clean.exit_code == 0, clean.output);

    BOOST_TEST(!fs::exists(app_artifact));
    BOOST_TEST(fs::is_regular_file(lib_artifact));
}

BOOST_AUTO_TEST_CASE(clean_all_removes_target_and_modules) {
    TempDir temp;
    temp.write_file("app/eta.toml",
                    make_manifest("app", "1.0.0", "dep = { path = \"../dep\" }\n"));
    temp.write_file("app/src/app.eta", R"eta(
(module app
  (import dep)
  (begin
    (display meaning)
    (newline)))
)eta");

    temp.write_file("dep/eta.toml", make_manifest("dep", "0.1.0"));
    temp.write_file("dep/src/dep.eta", R"eta(
(module dep
  (export meaning)
  (begin
    (define meaning 7)))
)eta");

    const auto build = run_eta(temp.path / "app", {"build"});
    BOOST_REQUIRE_MESSAGE(build.exit_code == 0, build.output);
    const auto vendor = run_eta(temp.path / "app", {"vendor"});
    BOOST_REQUIRE_MESSAGE(vendor.exit_code == 0, vendor.output);

    BOOST_TEST(fs::is_directory(temp.path / "app" / ".eta" / "target"));
    BOOST_TEST(fs::is_directory(temp.path / "app" / ".eta" / "modules"));

    const auto clean = run_eta(temp.path / "app", {"clean", "--all"});
    BOOST_REQUIRE_MESSAGE(clean.exit_code == 0, clean.output);

    BOOST_TEST(!fs::exists(temp.path / "app" / ".eta" / "target"));
    BOOST_TEST(!fs::exists(temp.path / "app" / ".eta" / "modules"));
}

BOOST_AUTO_TEST_SUITE_END()
