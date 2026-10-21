/// Launching external processes (etai, etac, git, tar, ...).

#include <array>

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

#include "eta/cli/cli_internal.h"

namespace eta::cli {

namespace {

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

} ///< namespace

CliResult<int> run_external(const fs::path& program,
                            const std::vector<std::string>& args,
                            const std::optional<fs::path>& cwd) {
    auto result = run_external_impl(program, args, cwd, false);
    if (!result) return std::unexpected(result.error());
    return result->exit_code;
}

CliResult<ExternalResult> run_external_capture(
    const fs::path& program,
    const std::vector<std::string>& args,
    const std::optional<fs::path>& cwd) {
    return run_external_impl(program, args, cwd, true);
}

} ///< namespace eta::cli
