#define BOOST_TEST_MODULE eta.cli.test
#include <boost/test/unit_test.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

namespace fs = std::filesystem;

#ifndef ETA_CLI_PATH
#error ETA_CLI_PATH must be defined by CMake
#endif

namespace {

const fs::path kEtaCliPath{ETA_CLI_PATH};

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

CommandResult run_eta(const fs::path& cwd, const std::vector<std::string>& args) {
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

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;

    PROCESS_INFORMATION pi{};
    std::wstring command_line = build_windows_command_line(kEtaCliPath, args);
    std::vector<wchar_t> mutable_cmd(command_line.begin(), command_line.end());
    mutable_cmd.push_back(L'\0');

    std::wstring cwd_w = cwd.wstring();
    const BOOL created = CreateProcessW(
        kEtaCliPath.wstring().c_str(),
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
    if (!created) {
        result.output = "CreateProcessW failed: " + win32_error_message(GetLastError());
        CloseHandle(read_pipe);
        return result;
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
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        result.output = "pipe() failed";
        return result;
    }

    std::vector<std::string> argv_storage;
    auto exec_argv = make_exec_argv(kEtaCliPath, args, argv_storage);

    const pid_t pid = fork();
    if (pid == -1) {
        result.output = "fork() failed";
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return result;
    }

    if (pid == 0) {
        close(pipe_fds[0]);
        if (chdir(cwd.c_str()) != 0) _exit(127);
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[1]);
        execv(kEtaCliPath.c_str(), exec_argv.data());
        _exit(127);
    }

    close(pipe_fds[1]);
    std::string output;
    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t n = read(pipe_fds[0], buffer.data(), buffer.size());
        if (n <= 0) break;
        output.append(buffer.data(), buffer.data() + static_cast<std::size_t>(n));
    }
    close(pipe_fds[0]);

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
