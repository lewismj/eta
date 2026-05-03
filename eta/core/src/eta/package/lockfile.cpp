#include "eta/package/lockfile.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace eta::package {

namespace {

enum class Section {
    Root,
    Package,
};

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

[[nodiscard]] std::string strip_comment(std::string_view value) {
    bool in_string = false;
    bool escaped = false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (in_string && c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            in_string = !in_string;
            continue;
        }
        if (!in_string && c == '#') {
            return std::string(value.substr(0, i));
        }
    }
    return std::string(value);
}

[[nodiscard]] std::size_t find_unquoted_char(std::string_view value, char needle) {
    bool in_string = false;
    bool escaped = false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (in_string && c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            in_string = !in_string;
            continue;
        }
        if (!in_string && c == needle) return i;
    }
    return std::string_view::npos;
}

[[nodiscard]] std::expected<std::string, LockfileError>
parse_quoted_string(std::string_view value, std::size_t line_no) {
    auto text = trim(value);
    if (text.size() < 2u || text.front() != '"' || text.back() != '"') {
        return std::unexpected(LockfileError{
            LockfileError::Code::ParseError,
            "expected quoted string",
            line_no,
        });
    }

    std::string out;
    out.reserve(text.size() - 2u);
    bool escaped = false;
    for (std::size_t i = 1u; i + 1u < text.size(); ++i) {
        const char c = text[i];
        if (escaped) {
            switch (c) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case '\\': out.push_back('\\'); break;
                case '"': out.push_back('"'); break;
                default:
                    return std::unexpected(LockfileError{
                        LockfileError::Code::ParseError,
                        "unsupported escape sequence in string",
                        line_no,
                    });
            }
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        out.push_back(c);
    }
    if (escaped) {
        return std::unexpected(LockfileError{
            LockfileError::Code::ParseError,
            "unterminated escape in string",
            line_no,
        });
    }
    return out;
}

[[nodiscard]] std::expected<int, LockfileError>
parse_integer(std::string_view value, std::size_t line_no) {
    auto text = trim(value);
    if (text.empty()) {
        return std::unexpected(LockfileError{
            LockfileError::Code::ParseError,
            "expected integer",
            line_no,
        });
    }

    int number = 0;
    for (const char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return std::unexpected(LockfileError{
                LockfileError::Code::ParseError,
                "expected integer",
                line_no,
            });
        }
        number = number * 10 + (c - '0');
    }
    return number;
}

[[nodiscard]] std::expected<std::vector<LockedDependency>, LockfileError>
parse_dependency_list(std::string_view value, std::size_t line_no) {
    auto text = trim(value);
    if (text.size() < 2u || text.front() != '[' || text.back() != ']') {
        return std::unexpected(LockfileError{
            LockfileError::Code::ParseError,
            "dependencies must be an array of \"name@version\" strings",
            line_no,
        });
    }

    std::vector<LockedDependency> dependencies;
    auto body = std::string_view(text).substr(1u, text.size() - 2u);
    std::size_t cursor = 0;
    while (cursor < body.size()) {
        std::size_t next = cursor;
        bool in_string = false;
        bool escaped = false;
        while (next < body.size()) {
            const char c = body[next];
            if (escaped) {
                escaped = false;
                ++next;
                continue;
            }
            if (in_string && c == '\\') {
                escaped = true;
                ++next;
                continue;
            }
            if (c == '"') {
                in_string = !in_string;
                ++next;
                continue;
            }
            if (!in_string && c == ',') break;
            ++next;
        }

        auto entry = trim(body.substr(cursor, next - cursor));
        if (!entry.empty()) {
            auto parsed = parse_quoted_string(entry, line_no);
            if (!parsed) return std::unexpected(parsed.error());

            const auto at = parsed->find('@');
            if (at == std::string::npos || at == 0u || at + 1u >= parsed->size()) {
                return std::unexpected(LockfileError{
                    LockfileError::Code::ParseError,
                    "dependency entries must use \"name@version\"",
                    line_no,
                });
            }

            dependencies.push_back(LockedDependency{
                parsed->substr(0u, at),
                parsed->substr(at + 1u),
            });
        }

        if (next == body.size()) break;
        cursor = next + 1u;
    }

    return dependencies;
}

[[nodiscard]] bool semverish(std::string_view value) {
    if (value.empty()) return false;
    bool has_digit = false;
    for (const char c : value) {
        const bool allowed = std::isdigit(static_cast<unsigned char>(c))
            || std::isalpha(static_cast<unsigned char>(c))
            || c == '.' || c == '-' || c == '+';
        if (!allowed) return false;
        if (std::isdigit(static_cast<unsigned char>(c))) has_digit = true;
    }
    return has_digit;
}

[[nodiscard]] std::string escape_string(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 4u);
    for (const char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

} // namespace

LockfileResult parse_lockfile(std::string_view text) {
    Lockfile lockfile;
    Section current_section = Section::Root;
    std::optional<std::size_t> current_package_index;

    std::istringstream stream{std::string(text)};
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(stream, line)) {
        ++line_no;
        const auto stripped = trim(strip_comment(line));
        if (stripped.empty()) continue;

        if (stripped.front() == '[') {
            if (stripped == "[[package]]") {
                lockfile.packages.emplace_back();
                current_package_index = lockfile.packages.size() - 1u;
                current_section = Section::Package;
                continue;
            }
            return std::unexpected(LockfileError{
                LockfileError::Code::UnsupportedValue,
                "unsupported lockfile section: " + stripped,
                line_no,
            });
        }

        const auto eq = find_unquoted_char(stripped, '=');
        if (eq == std::string::npos) {
            return std::unexpected(LockfileError{
                LockfileError::Code::ParseError,
                "expected key/value assignment",
                line_no,
            });
        }

        const auto key = trim(std::string_view(stripped).substr(0u, eq));
        const auto value = std::string_view(stripped).substr(eq + 1u);
        if (key.empty()) {
            return std::unexpected(LockfileError{
                LockfileError::Code::ParseError,
                "empty key in assignment",
                line_no,
            });
        }

        if (current_section == Section::Root) {
            if (key == "version") {
                auto parsed = parse_integer(value, line_no);
                if (!parsed) return std::unexpected(parsed.error());
                lockfile.schema_version = *parsed;
            }
            continue;
        }

        if (!current_package_index.has_value()) {
            return std::unexpected(LockfileError{
                LockfileError::Code::ParseError,
                "package field declared outside [[package]] table",
                line_no,
            });
        }

        auto& pkg = lockfile.packages[*current_package_index];
        if (key == "name") {
            auto parsed = parse_quoted_string(value, line_no);
            if (!parsed) return std::unexpected(parsed.error());
            pkg.name = std::move(*parsed);
            continue;
        }
        if (key == "version") {
            auto parsed = parse_quoted_string(value, line_no);
            if (!parsed) return std::unexpected(parsed.error());
            pkg.version = std::move(*parsed);
            continue;
        }
        if (key == "source") {
            auto parsed = parse_quoted_string(value, line_no);
            if (!parsed) return std::unexpected(parsed.error());
            pkg.source = std::move(*parsed);
            continue;
        }
        if (key == "dependencies") {
            auto parsed = parse_dependency_list(value, line_no);
            if (!parsed) return std::unexpected(parsed.error());
            pkg.dependencies = std::move(*parsed);
            continue;
        }
    }

    if (lockfile.schema_version != 1) {
        return std::unexpected(LockfileError{
            LockfileError::Code::InvalidValue,
            "unsupported eta.lock schema version: " + std::to_string(lockfile.schema_version),
            0,
        });
    }

    for (const auto& pkg : lockfile.packages) {
        if (pkg.name.empty()) {
            return std::unexpected(LockfileError{
                LockfileError::Code::MissingRequiredField,
                "lockfile package missing name",
                0,
            });
        }
        if (pkg.version.empty()) {
            return std::unexpected(LockfileError{
                LockfileError::Code::MissingRequiredField,
                "lockfile package missing version",
                0,
            });
        }
        if (pkg.source.empty()) {
            return std::unexpected(LockfileError{
                LockfileError::Code::MissingRequiredField,
                "lockfile package missing source",
                0,
            });
        }
        if (!semverish(pkg.version)) {
            return std::unexpected(LockfileError{
                LockfileError::Code::InvalidValue,
                "lockfile package has invalid semver: " + pkg.version,
                0,
            });
        }
        for (const auto& dep : pkg.dependencies) {
            if (dep.name.empty() || dep.version.empty()) {
                return std::unexpected(LockfileError{
                    LockfileError::Code::InvalidValue,
                    "dependency entry must contain name and version",
                    0,
                });
            }
            if (!semverish(dep.version)) {
                return std::unexpected(LockfileError{
                    LockfileError::Code::InvalidValue,
                    "dependency has invalid semver: " + dep.version,
                    0,
                });
            }
        }
    }

    return lockfile;
}

LockfileResult read_lockfile(const fs::path& lockfile_path) {
    std::ifstream in(lockfile_path, std::ios::in | std::ios::binary);
    if (!in) {
        return std::unexpected(LockfileError{
            LockfileError::Code::IoError,
            "cannot open lockfile: " + lockfile_path.string(),
            0,
        });
    }

    std::ostringstream buffer;
    buffer << in.rdbuf();
    return parse_lockfile(buffer.str());
}

std::string write_lockfile(const Lockfile& lockfile) {
    std::vector<LockfilePackage> packages = lockfile.packages;
    std::sort(packages.begin(), packages.end(),
              [](const LockfilePackage& lhs, const LockfilePackage& rhs) {
                  const bool lhs_root = lhs.source == "root";
                  const bool rhs_root = rhs.source == "root";
                  if (lhs_root != rhs_root) return lhs_root;
                  if (lhs.name != rhs.name) return lhs.name < rhs.name;
                  if (lhs.version != rhs.version) return lhs.version < rhs.version;
                  return lhs.source < rhs.source;
              });

    std::ostringstream out;
    out << "# Auto-generated by eta - DO NOT EDIT.\n";
    out << "version = 1\n";

    for (auto& pkg : packages) {
        std::sort(pkg.dependencies.begin(), pkg.dependencies.end(),
                  [](const LockedDependency& lhs, const LockedDependency& rhs) {
                      if (lhs.name != rhs.name) return lhs.name < rhs.name;
                      return lhs.version < rhs.version;
                  });

        out << "\n[[package]]\n";
        out << "name = \"" << escape_string(pkg.name) << "\"\n";
        out << "version = \"" << escape_string(pkg.version) << "\"\n";
        out << "source = \"" << escape_string(pkg.source) << "\"\n";
        out << "dependencies = [";
        for (std::size_t i = 0; i < pkg.dependencies.size(); ++i) {
            if (i != 0u) out << ", ";
            out << "\""
                << escape_string(pkg.dependencies[i].name)
                << "@"
                << escape_string(pkg.dependencies[i].version)
                << "\"";
        }
        out << "]\n";
    }

    return out.str();
}

std::expected<void, LockfileError> write_lockfile_file(const Lockfile& lockfile,
                                                       const fs::path& lockfile_path) {
    std::error_code ec;
    if (lockfile_path.has_parent_path()) {
        fs::create_directories(lockfile_path.parent_path(), ec);
        if (ec) {
            return std::unexpected(LockfileError{
                LockfileError::Code::IoError,
                "failed to create lockfile parent directory: " + lockfile_path.parent_path().string(),
                0,
            });
        }
    }

    std::ofstream out(lockfile_path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out) {
        return std::unexpected(LockfileError{
            LockfileError::Code::IoError,
            "cannot open lockfile for writing: " + lockfile_path.string(),
            0,
        });
    }

    out << write_lockfile(lockfile);
    if (!out.good()) {
        return std::unexpected(LockfileError{
            LockfileError::Code::IoError,
            "failed to write lockfile: " + lockfile_path.string(),
            0,
        });
    }
    return {};
}

} // namespace eta::package
