#include "eta/jupyter/eta_interpreter.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <cstdlib>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "xeus/xhelper.hpp"
#include "xeus/xkernel.hpp"
#include "xeus/xkernel_configuration.hpp"
#include "xeus-zmq/xserver_zmq.hpp"
#include "xeus-zmq/xzmq_context.hpp"

namespace {

namespace fs = std::filesystem;
namespace nl = nlohmann;

enum class InstallMode {
    User,
    Prefix,
    SysPrefix,
};

struct InstallOptions {
    InstallMode mode{InstallMode::User};
    fs::path prefix{};
};

/**
 * @brief Poll signal notifications and route them into interpreter interrupts.
 *
 * Jupyter servers interrupt kernels by sending process-level signals.
 * The signal handler only flips an atomic flag; the polling thread forwards
 * the request to the interpreter in normal C++ context.
 */
class InterruptPump {
public:
    explicit InterruptPump(eta::jupyter::EtaInterpreter* interpreter)
        : interpreter_(interpreter),
          previous_sigint_(std::signal(SIGINT, &InterruptPump::signal_handler))
#if defined(SIGTERM)
          ,
          previous_sigterm_(std::signal(SIGTERM, &InterruptPump::signal_handler))
#endif
    {
        pump_thread_ = std::thread([this]() { run(); });
    }

    ~InterruptPump() {
        stop_.store(true, std::memory_order_release);
        if (pump_thread_.joinable()) {
            pump_thread_.join();
        }
        if (previous_sigint_ != SIG_ERR) {
            std::signal(SIGINT, previous_sigint_);
        }
#if defined(SIGTERM)
        if (previous_sigterm_ != SIG_ERR) {
            std::signal(SIGTERM, previous_sigterm_);
        }
#endif
    }

    InterruptPump(const InterruptPump&) = delete;
    InterruptPump& operator=(const InterruptPump&) = delete;
    InterruptPump(InterruptPump&&) = delete;
    InterruptPump& operator=(InterruptPump&&) = delete;

private:
    static void signal_handler(int /*signum*/) noexcept {
        interrupt_requested_.store(true, std::memory_order_release);
    }

    void run() {
        using namespace std::chrono_literals;
        while (!stop_.load(std::memory_order_acquire)) {
            if (interrupt_requested_.exchange(false, std::memory_order_acq_rel)) {
                if (interpreter_) {
                    interpreter_->request_interrupt();
                }
            }
            std::this_thread::sleep_for(5ms);
        }
    }

    using SignalHandler = void (*)(int);

    eta::jupyter::EtaInterpreter* interpreter_{nullptr};
    std::atomic<bool> stop_{false};
    std::thread pump_thread_{};
    SignalHandler previous_sigint_{SIG_DFL};
#if defined(SIGTERM)
    SignalHandler previous_sigterm_{SIG_DFL};
#endif
    static inline std::atomic<bool> interrupt_requested_{false};
};

[[nodiscard]] bool arg_eq(const std::string& arg, std::string_view expected) {
    return arg == expected;
}

[[nodiscard]] std::optional<InstallOptions> parse_install_options(int argc, char* argv[]) {
    bool has_install = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--install") {
            has_install = true;
            break;
        }
    }
    if (!has_install) return std::nullopt;

    InstallOptions out;
    bool mode_explicit = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg_eq(arg, "--install")) continue;
        if (arg_eq(arg, "--version")) continue;

        if (arg_eq(arg, "--user")) {
            out.mode = InstallMode::User;
            mode_explicit = true;
            continue;
        }
        if (arg_eq(arg, "--sys-prefix")) {
            out.mode = InstallMode::SysPrefix;
            mode_explicit = true;
            continue;
        }
        if (arg_eq(arg, "--prefix")) {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for --prefix");
            }
            out.mode = InstallMode::Prefix;
            out.prefix = argv[++i];
            mode_explicit = true;
            continue;
        }
        if (arg.rfind("--prefix=", 0) == 0) {
            out.mode = InstallMode::Prefix;
            out.prefix = arg.substr(std::string("--prefix=").size());
            mode_explicit = true;
            continue;
        }
        if (arg_eq(arg, "-f")) {
            ++i;
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            throw std::runtime_error("unknown option for --install: " + arg);
        }
    }

    if (out.mode == InstallMode::Prefix && out.prefix.empty()) {
        throw std::runtime_error("empty --prefix value");
    }
    if (!mode_explicit) out.mode = InstallMode::User;
    return out;
}

[[nodiscard]] fs::path user_kernelspec_dir() {
#if defined(_WIN32)
    if (const char* appdata = std::getenv("APPDATA"); appdata && appdata[0] != '\0') {
        return fs::path(appdata) / "jupyter" / "kernels" / "eta";
    }
    if (const char* home = std::getenv("USERPROFILE"); home && home[0] != '\0') {
        return fs::path(home) / "AppData" / "Roaming" / "jupyter" / "kernels" / "eta";
    }
#else
    if (const char* home = std::getenv("HOME"); home && home[0] != '\0') {
        return fs::path(home) / ".local" / "share" / "jupyter" / "kernels" / "eta";
    }
#endif
    throw std::runtime_error("unable to determine user kernelspec directory");
}

[[nodiscard]] std::optional<fs::path> sys_prefix_path() {
    for (const char* env_name : {"CONDA_PREFIX", "VIRTUAL_ENV", "PYTHONHOME"}) {
        if (const char* val = std::getenv(env_name); val && val[0] != '\0') {
            return fs::path(val);
        }
    }
    return std::nullopt;
}

[[nodiscard]] fs::path resolve_install_dir(const InstallOptions& options) {
    switch (options.mode) {
        case InstallMode::User:
            return user_kernelspec_dir();
        case InstallMode::Prefix:
            return options.prefix / "share" / "jupyter" / "kernels" / "eta";
        case InstallMode::SysPrefix: {
            const auto prefix = sys_prefix_path();
            if (!prefix) {
                throw std::runtime_error(
                    "--sys-prefix requested but no CONDA_PREFIX/VIRTUAL_ENV/PYTHONHOME is set");
            }
            return *prefix / "share" / "jupyter" / "kernels" / "eta";
        }
    }
    throw std::runtime_error("invalid install mode");
}

[[nodiscard]] bool is_stdlib_root(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || ec) return false;

    auto has_module_artifact = [&](std::string_view module_leaf) {
        const auto module_eta = dir / "std" / (std::string(module_leaf) + ".eta");
        const auto module_etac = dir / "std" / (std::string(module_leaf) + ".etac");
        ec.clear();
        const bool has_eta = fs::is_regular_file(module_eta, ec) && !ec;
        ec.clear();
        const bool has_etac = fs::is_regular_file(module_etac, ec) && !ec;
        return has_eta || has_etac;
    };

    return has_module_artifact("core") && has_module_artifact("jupyter");
}

[[nodiscard]] std::optional<std::string> detect_module_path_env(const fs::path& exe_path) {
#ifdef ETA_STDLIB_DIR
    {
        fs::path compiled(ETA_STDLIB_DIR);
        if (!compiled.empty() && is_stdlib_root(compiled)) {
            return fs::weakly_canonical(compiled).string();
        }
    }
#endif

    auto cur = exe_path.parent_path();
    for (int i = 0; i < 8 && !cur.empty(); ++i) {
        const auto candidate = cur / "stdlib";
        if (is_stdlib_root(candidate)) {
            return fs::weakly_canonical(candidate).string();
        }
        cur = cur.parent_path();
    }

    if (const char* env = std::getenv("ETA_MODULE_PATH"); env && env[0] != '\0') {
        return std::string(env);
    }

    return std::nullopt;
}

void write_kernel_json(const fs::path& destination,
                       const fs::path& exe_path,
                       const std::optional<std::string>& module_path_env) {
    nl::json j = {
        {"argv", nl::json::array({exe_path.string(), "-f", "{connection_file}"})},
        {"display_name", "Eta"},
        {"language", "eta"},
        {"metadata", {
            {"language_info", {
                {"name", "eta"},
                {"mimetype", "text/x-eta"},
                {"file_extension", ".eta"},
                {"pygments_lexer", "scheme"},
            }},
        }},
    };
    if (module_path_env && !module_path_env->empty()) {
        j["env"] = nl::json::object({
            {"ETA_MODULE_PATH", *module_path_env},
        });
    }

    std::ofstream out(destination / "kernel.json", std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to write kernel.json");
    }
    out << j.dump(2) << '\n';
}

void copy_if_exists(const fs::path& source, const fs::path& destination) {
    std::error_code ec;
    if (!fs::exists(source, ec) || ec) return;
    fs::copy_file(source, destination / source.filename(),
                  fs::copy_options::overwrite_existing, ec);
}

void copy_kernel_assets(const fs::path& destination, const fs::path& exe_path) {
    std::vector<fs::path> resource_roots;
    resource_roots.emplace_back(exe_path.parent_path() / "resources");
#ifdef ETA_JUPYTER_SOURCE_RESOURCE_DIR
    resource_roots.emplace_back(fs::path(ETA_JUPYTER_SOURCE_RESOURCE_DIR));
#endif

    for (const auto& root : resource_roots) {
        copy_if_exists(root / "logo-32x32.png", destination);
        copy_if_exists(root / "logo-64x64.png", destination);
    }
}

int install_kernelspec(const InstallOptions& options, const fs::path& exe_path) {
    const auto destination = resolve_install_dir(options);
    std::error_code ec;
    fs::create_directories(destination, ec);
    if (ec) {
        throw std::runtime_error("failed to create kernelspec directory: " + destination.string());
    }

    write_kernel_json(destination, exe_path, detect_module_path_env(exe_path));
    copy_kernel_assets(destination, exe_path);
    std::cout << "installed kernelspec to " << destination.string() << '\n';
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        if (xeus::should_print_version(argc, argv)) {
            std::cout << "eta_jupyter 0.1.0\n";
            return 0;
        }

        const auto install_options = parse_install_options(argc, argv);
        if (install_options) {
            const auto exe_path = fs::weakly_canonical(fs::path(argv[0]));
            return install_kernelspec(*install_options, exe_path);
        }

        const std::string connection_file = xeus::extract_filename(argc, argv);
        if (connection_file.empty()) {
            std::cerr << "error: missing Jupyter connection file (-f <path>)\n";
            return 2;
        }

        xeus::xconfiguration config = xeus::load_configuration(connection_file);
        auto context = xeus::make_zmq_context();
        auto interpreter = std::make_unique<eta::jupyter::EtaInterpreter>();
        auto* interpreter_raw = interpreter.get();

        xeus::xkernel kernel(
            config,
            xeus::get_user_name(),
            std::move(context),
            std::move(interpreter),
            xeus::make_xserver_default);
        InterruptPump interrupt_pump(interpreter_raw);
        kernel.start();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
