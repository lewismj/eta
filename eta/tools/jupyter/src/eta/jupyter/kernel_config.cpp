#include "eta/jupyter/kernel_config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace eta::jupyter {

namespace {

namespace fs = std::filesystem;

[[nodiscard]] std::string trim(std::string_view value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1u]))) {
        --end;
    }
    return std::string(value.substr(start, end - start));
}

[[nodiscard]] std::string strip_comment(std::string_view line) {
    bool in_string = false;
    bool escape = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (escape) {
            escape = false;
            continue;
        }
        if (c == '\\' && in_string) {
            escape = true;
            continue;
        }
        if (c == '"') {
            in_string = !in_string;
            continue;
        }
        if (c == '#' && !in_string) {
            return std::string(line.substr(0, i));
        }
    }
    return std::string(line);
}

[[nodiscard]] std::optional<long long> parse_integer(std::string_view text) {
    const auto t = trim(text);
    if (t.empty()) return std::nullopt;
    char* end = nullptr;
    const long long v = std::strtoll(t.c_str(), &end, 10);
    if (!end) return std::nullopt;
    while (*end != '\0') {
        if (!std::isspace(static_cast<unsigned char>(*end))) return std::nullopt;
        ++end;
    }
    return v;
}

[[nodiscard]] std::optional<std::string> parse_quoted_string(std::string_view text) {
    auto t = trim(text);
    if (t.size() < 2u || t.front() != '"' || t.back() != '"') return std::nullopt;
    std::string out;
    out.reserve(t.size() - 2u);
    bool escape = false;
    for (std::size_t i = 1; i + 1u < t.size(); ++i) {
        const char c = t[i];
        if (escape) {
            switch (c) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case '\\': out.push_back('\\'); break;
                case '"': out.push_back('"'); break;
                default: out.push_back(c); break;
            }
            escape = false;
            continue;
        }
        if (c == '\\') {
            escape = true;
            continue;
        }
        out.push_back(c);
    }
    if (escape) return std::nullopt;
    return out;
}

[[nodiscard]] std::optional<std::vector<std::string>> parse_string_list(std::string_view text) {
    const auto t = trim(text);
    if (t.size() < 2u || t.front() != '[' || t.back() != ']') return std::nullopt;

    std::vector<std::string> out;
    std::size_t i = 1;
    while (i + 1u < t.size()) {
        while (i + 1u < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) ++i;
        if (i + 1u >= t.size()) break;
        if (t[i] == ']') break;

        if (t[i] != '"') return std::nullopt;
        std::size_t j = i + 1u;
        bool escape = false;
        std::string item;
        while (j < t.size()) {
            const char c = t[j++];
            if (escape) {
                switch (c) {
                    case 'n': item.push_back('\n'); break;
                    case 'r': item.push_back('\r'); break;
                    case 't': item.push_back('\t'); break;
                    case '\\': item.push_back('\\'); break;
                    case '"': item.push_back('"'); break;
                    default: item.push_back(c); break;
                }
                escape = false;
                continue;
            }
            if (c == '\\') {
                escape = true;
                continue;
            }
            if (c == '"') break;
            item.push_back(c);
        }
        if (escape || j > t.size()) return std::nullopt;
        out.push_back(std::move(item));
        i = j;

        while (i + 1u < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) ++i;
        if (i + 1u < t.size() && t[i] == ',') {
            ++i;
            continue;
        }
        if (i + 1u < t.size() && t[i] == ']') break;
    }

    return out;
}

void ensure_autoimport_defaults(KernelConfig& config) {
    std::vector<std::string> ordered;
    ordered.reserve(config.autoimport_modules.size() + 1u);
    ordered.push_back("std.io");
    for (const auto& module : config.autoimport_modules) {
        if (module.empty() || module == "std.io") continue;
        ordered.push_back(module);
    }

    std::unordered_set<std::string> seen;
    config.autoimport_modules.clear();
    for (auto& module : ordered) {
        if (seen.insert(module).second) {
            config.autoimport_modules.push_back(std::move(module));
        }
    }
}

KernelConfigLoadResult parse_kernel_config_file(const fs::path& file_path) {
    KernelConfigLoadResult out;
    out.path = file_path;

    std::ifstream in(file_path, std::ios::in | std::ios::binary);
    if (!in) {
        out.warnings.push_back("could not open kernel config: " + file_path.string());
        ensure_autoimport_defaults(out.config);
        return out;
    }

    std::string section;
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        const auto stripped = trim(strip_comment(line));
        if (stripped.empty()) continue;

        if (stripped.front() == '[' && stripped.back() == ']') {
            section = trim(std::string_view(stripped).substr(1u, stripped.size() - 2u));
            continue;
        }

        const auto eq = stripped.find('=');
        if (eq == std::string::npos) {
            out.warnings.push_back("kernel.toml:" + std::to_string(line_no) + ": missing '='");
            continue;
        }

        const auto key = trim(std::string_view(stripped).substr(0u, eq));
        const auto value = trim(std::string_view(stripped).substr(eq + 1u));

        if (section == "autoimport" && key == "modules") {
            auto modules = parse_string_list(value);
            if (!modules) {
                out.warnings.push_back("kernel.toml:" + std::to_string(line_no) + ": invalid modules list");
                continue;
            }
            out.config.autoimport_modules = std::move(*modules);
            continue;
        }

        if (section == "display" && key == "table_max_rows") {
            auto n = parse_integer(value);
            if (!n || *n <= 0) {
                out.warnings.push_back("kernel.toml:" + std::to_string(line_no) + ": invalid table_max_rows");
                continue;
            }
            out.config.display.table_max_rows = static_cast<std::size_t>(*n);
            continue;
        }

        if (section == "display" && key == "tensor_preview") {
            auto n = parse_integer(value);
            if (!n || *n <= 0) {
                out.warnings.push_back("kernel.toml:" + std::to_string(line_no) + ": invalid tensor_preview");
                continue;
            }
            out.config.display.tensor_preview = static_cast<std::size_t>(*n);
            continue;
        }

        if (section == "display" && key == "plot_theme") {
            auto s = parse_quoted_string(value);
            if (!s) {
                out.warnings.push_back("kernel.toml:" + std::to_string(line_no) + ": invalid plot_theme");
                continue;
            }
            out.config.display.plot_theme = std::move(*s);
            continue;
        }

        if (section == "interrupt" && key == "hard_kill_after_seconds") {
            auto n = parse_integer(value);
            if (!n || *n <= 0) {
                out.warnings.push_back("kernel.toml:" + std::to_string(line_no) + ": invalid hard_kill_after_seconds");
                continue;
            }
            out.config.hard_kill_after_seconds = static_cast<int>(*n);
            continue;
        }
    }

    ensure_autoimport_defaults(out.config);
    return out;
}

[[nodiscard]] std::vector<fs::path> config_candidates() {
    std::vector<fs::path> out;
    out.reserve(5);

    if (const char* explicit_config = std::getenv("ETA_KERNEL_CONFIG"); explicit_config && explicit_config[0] != '\0') {
        out.emplace_back(explicit_config);
        return out;
    }

    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && xdg[0] != '\0') {
        out.emplace_back(fs::path(xdg) / "eta" / "kernel.toml");
    }
    if (const char* home = std::getenv("HOME"); home && home[0] != '\0') {
        out.emplace_back(fs::path(home) / ".config" / "eta" / "kernel.toml");
    }
    if (const char* user_profile = std::getenv("USERPROFILE"); user_profile && user_profile[0] != '\0') {
        out.emplace_back(fs::path(user_profile) / ".config" / "eta" / "kernel.toml");
    }
    if (const char* appdata = std::getenv("APPDATA"); appdata && appdata[0] != '\0') {
        out.emplace_back(fs::path(appdata) / "eta" / "kernel.toml");
    }

    return out;
}

} // namespace

KernelConfigLoadResult load_kernel_config() {
    const auto candidates = config_candidates();
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (fs::exists(candidate, ec) && !ec && fs::is_regular_file(candidate, ec) && !ec) {
            return parse_kernel_config_file(candidate);
        }
    }

    KernelConfigLoadResult out;
    ensure_autoimport_defaults(out.config);
    if (const char* explicit_config = std::getenv("ETA_KERNEL_CONFIG");
        explicit_config && explicit_config[0] != '\0') {
        out.path = fs::path(explicit_config);
        out.warnings.push_back("ETA_KERNEL_CONFIG points to a missing file: " + out.path.string());
    }
    return out;
}

} // namespace eta::jupyter
