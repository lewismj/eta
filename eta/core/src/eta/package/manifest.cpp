#include "eta/package/manifest.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace eta::package {

namespace {

enum class Section {
    Root,
    Package,
    Compatibility,
    Dependencies,
    DevDependencies,
    Other,
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

[[nodiscard]] std::expected<std::string, ManifestError>
parse_quoted_string(std::string_view value, std::size_t line_no) {
    auto text = trim(value);
    if (text.size() < 2u || text.front() != '"' || text.back() != '"') {
        return std::unexpected(ManifestError{
            ManifestError::Code::ParseError,
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
                    return std::unexpected(ManifestError{
                        ManifestError::Code::ParseError,
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
        return std::unexpected(ManifestError{
            ManifestError::Code::ParseError,
            "unterminated escape in string",
            line_no,
        });
    }
    return out;
}

[[nodiscard]] bool is_valid_hex(std::string_view value, std::size_t expected_len) {
    if (value.size() != expected_len) return false;
    for (const char c : value) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

[[nodiscard]] std::expected<ManifestDependency, ManifestError>
parse_dependency_inline_table(std::string_view dep_name,
                              std::string_view value,
                              std::size_t line_no) {
    auto text = trim(value);
    if (text.size() < 2u || text.front() != '{' || text.back() != '}') {
        return std::unexpected(ManifestError{
            ManifestError::Code::UnsupportedValue,
            "dependencies must use inline table form",
            line_no,
        });
    }

    ManifestDependency dep;
    dep.name = std::string(dep_name);

    bool seen_path = false;
    bool seen_git = false;
    bool seen_rev = false;
    bool seen_tarball = false;
    bool seen_sha256 = false;

    const auto body = std::string_view(text).substr(1u, text.size() - 2u);
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

        auto pair = trim(body.substr(cursor, next - cursor));
        if (!pair.empty()) {
            const auto eq = find_unquoted_char(pair, '=');
            if (eq == std::string::npos) {
                return std::unexpected(ManifestError{
                    ManifestError::Code::ParseError,
                    "invalid dependency inline table entry",
                    line_no,
                });
            }

            const auto key = trim(std::string_view(pair).substr(0u, eq));
            const auto raw = std::string_view(pair).substr(eq + 1u);
            auto value_res = parse_quoted_string(raw, line_no);
            if (!value_res) return std::unexpected(value_res.error());

            if (key == "path") {
                if (seen_path) {
                    return std::unexpected(ManifestError{
                        ManifestError::Code::ParseError,
                        "duplicate path key in dependency spec",
                        line_no,
                    });
                }
                dep.path = fs::path(*value_res);
                seen_path = true;
            } else if (key == "git") {
                if (seen_git) {
                    return std::unexpected(ManifestError{
                        ManifestError::Code::ParseError,
                        "duplicate git key in dependency spec",
                        line_no,
                    });
                }
                dep.git = std::move(*value_res);
                seen_git = true;
            } else if (key == "rev") {
                if (seen_rev) {
                    return std::unexpected(ManifestError{
                        ManifestError::Code::ParseError,
                        "duplicate rev key in dependency spec",
                        line_no,
                    });
                }
                dep.rev = std::move(*value_res);
                seen_rev = true;
            } else if (key == "tarball") {
                if (seen_tarball) {
                    return std::unexpected(ManifestError{
                        ManifestError::Code::ParseError,
                        "duplicate tarball key in dependency spec",
                        line_no,
                    });
                }
                dep.tarball = std::move(*value_res);
                seen_tarball = true;
            } else if (key == "sha256") {
                if (seen_sha256) {
                    return std::unexpected(ManifestError{
                        ManifestError::Code::ParseError,
                        "duplicate sha256 key in dependency spec",
                        line_no,
                    });
                }
                dep.sha256 = std::move(*value_res);
                seen_sha256 = true;
            } else {
                return std::unexpected(ManifestError{
                    ManifestError::Code::UnsupportedValue,
                    "unsupported dependency key: " + key,
                    line_no,
                });
            }
        }

        if (next == body.size()) break;
        cursor = next + 1u;
    }

    const int source_kinds =
        (seen_path ? 1 : 0) + (seen_git ? 1 : 0) + (seen_tarball ? 1 : 0);
    if (source_kinds != 1) {
        return std::unexpected(ManifestError{
            ManifestError::Code::MissingRequiredField,
            "dependency spec must declare exactly one of path/git/tarball",
            line_no,
        });
    }

    if (seen_path) {
        if (dep.path.empty()) {
            return std::unexpected(ManifestError{
                ManifestError::Code::MissingRequiredField,
                "dependency spec is missing non-empty path",
                line_no,
            });
        }
        dep.kind = ManifestDependencyKind::Path;
        return dep;
    }

    if (seen_git) {
        if (!seen_rev || dep.git.empty() || dep.rev.empty()) {
            return std::unexpected(ManifestError{
                ManifestError::Code::MissingRequiredField,
                "git dependencies require git and rev keys",
                line_no,
            });
        }
        if (!is_valid_hex(dep.rev, 40u)) {
            return std::unexpected(ManifestError{
                ManifestError::Code::InvalidValue,
                "git dependency rev must be a full 40-hex commit id",
                line_no,
            });
        }
        dep.kind = ManifestDependencyKind::Git;
        return dep;
    }

    if (!seen_sha256 || dep.tarball.empty() || dep.sha256.empty()) {
        return std::unexpected(ManifestError{
            ManifestError::Code::MissingRequiredField,
            "tarball dependencies require tarball and sha256 keys",
            line_no,
        });
    }
    if (!is_valid_hex(dep.sha256, 64u)) {
        return std::unexpected(ManifestError{
            ManifestError::Code::InvalidValue,
            "tarball dependency sha256 must be 64 hex characters",
            line_no,
        });
    }
    dep.kind = ManifestDependencyKind::Tarball;
    return dep;
}

[[nodiscard]] bool is_valid_package_name(std::string_view value) {
    static const std::regex kPattern("^[a-z][a-z0-9_-]{0,63}$");
    return std::regex_match(value.begin(), value.end(), kPattern);
}

[[nodiscard]] bool is_valid_semver(std::string_view value) {
    static const std::regex kPattern(
        "^[0-9]+\\.[0-9]+\\.[0-9]+(?:-[0-9A-Za-z.-]+)?(?:\\+[0-9A-Za-z.-]+)?$");
    return std::regex_match(value.begin(), value.end(), kPattern);
}

[[nodiscard]] bool looks_like_spdx_expression(std::string_view value) {
    if (value.empty()) return false;
    bool has_alnum = false;
    for (const char c : value) {
        const bool allowed =
            std::isalnum(static_cast<unsigned char>(c))
            || c == ' ' || c == '-' || c == '.' || c == '+' || c == '(' || c == ')'
            || c == ':';
        if (!allowed) return false;
        if (std::isalnum(static_cast<unsigned char>(c))) has_alnum = true;
    }
    return has_alnum;
}

[[nodiscard]] bool looks_like_semver_range(std::string_view value) {
    if (value.empty()) return false;
    bool has_digit = false;
    for (const char c : value) {
        const bool allowed =
            std::isdigit(static_cast<unsigned char>(c))
            || std::isalpha(static_cast<unsigned char>(c))
            || c == ' ' || c == ',' || c == '.' || c == '-' || c == '+'
            || c == '<' || c == '>' || c == '=' || c == '^' || c == '~' || c == '*';
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

[[nodiscard]] std::string render_dependency(const ManifestDependency& dep) {
    std::ostringstream out;
    out << dep.name << " = { ";
    switch (dep.kind) {
        case ManifestDependencyKind::Path:
            out << "path = \"" << escape_string(dep.path.generic_string()) << "\"";
            break;
        case ManifestDependencyKind::Git:
            out << "git = \"" << escape_string(dep.git) << "\", "
                << "rev = \"" << escape_string(dep.rev) << "\"";
            break;
        case ManifestDependencyKind::Tarball:
            out << "tarball = \"" << escape_string(dep.tarball) << "\", "
                << "sha256 = \"" << escape_string(dep.sha256) << "\"";
            break;
    }
    out << " }";
    return out.str();
}

} // namespace

ManifestResult parse_manifest(std::string_view text, const fs::path& source_path) {
    Manifest manifest;
    manifest.manifest_path = source_path;

    Section current_section = Section::Root;
    std::unordered_set<std::string> dependency_names;
    std::unordered_set<std::string> dev_dependency_names;

    std::istringstream stream{std::string(text)};
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(stream, line)) {
        ++line_no;
        const auto stripped = trim(strip_comment(line));
        if (stripped.empty()) continue;

        if (stripped.front() == '[') {
            if (stripped.size() < 2u || stripped.back() != ']') {
                return std::unexpected(ManifestError{
                    ManifestError::Code::ParseError,
                    "unterminated section header",
                    line_no,
                });
            }
            if (stripped.size() >= 4u
                && stripped[0] == '[' && stripped[1] == '['
                && stripped[stripped.size() - 1u] == ']'
                && stripped[stripped.size() - 2u] == ']') {
                return std::unexpected(ManifestError{
                    ManifestError::Code::UnsupportedValue,
                    "array-of-table syntax is not supported in eta.toml",
                    line_no,
                });
            }

            const auto header = trim(
                std::string_view(stripped).substr(1u, stripped.size() - 2u));
            if (header == "package") {
                current_section = Section::Package;
            } else if (header == "compatibility") {
                current_section = Section::Compatibility;
            } else if (header == "dependencies") {
                current_section = Section::Dependencies;
            } else if (header == "dev-dependencies") {
                current_section = Section::DevDependencies;
            } else {
                current_section = Section::Other;
            }
            continue;
        }

        const auto eq = find_unquoted_char(stripped, '=');
        if (eq == std::string::npos) {
            return std::unexpected(ManifestError{
                ManifestError::Code::ParseError,
                "expected key/value assignment",
                line_no,
            });
        }
        const auto key = trim(std::string_view(stripped).substr(0u, eq));
        const auto value = std::string_view(stripped).substr(eq + 1u);
        if (key.empty()) {
            return std::unexpected(ManifestError{
                ManifestError::Code::ParseError,
                "empty key in assignment",
                line_no,
            });
        }

        if (current_section == Section::Package) {
            if (key == "name" || key == "version" || key == "license") {
                auto parsed = parse_quoted_string(value, line_no);
                if (!parsed) return std::unexpected(parsed.error());

                if (key == "name") {
                    manifest.name = std::move(*parsed);
                } else if (key == "version") {
                    manifest.version = std::move(*parsed);
                } else {
                    manifest.license = std::move(*parsed);
                }
            }
            continue;
        }

        if (current_section == Section::Compatibility) {
            if (key == "eta") {
                auto parsed = parse_quoted_string(value, line_no);
                if (!parsed) return std::unexpected(parsed.error());
                manifest.compatibility_eta = std::move(*parsed);
            }
            continue;
        }

        if (current_section == Section::Dependencies
            || current_section == Section::DevDependencies) {
            if (!is_valid_package_name(key)) {
                return std::unexpected(ManifestError{
                    ManifestError::Code::InvalidValue,
                    "dependency name must match [a-z][a-z0-9_-]{0,63}",
                    line_no,
                });
            }

            auto& names = (current_section == Section::Dependencies)
                ? dependency_names
                : dev_dependency_names;
            if (!names.insert(key).second) {
                return std::unexpected(ManifestError{
                    ManifestError::Code::ParseError,
                    "duplicate dependency entry: " + key,
                    line_no,
                });
            }
            if (dependency_names.contains(key) && dev_dependency_names.contains(key)) {
                return std::unexpected(ManifestError{
                    ManifestError::Code::ParseError,
                    "dependency '" + key + "' cannot be in both dependencies and dev-dependencies",
                    line_no,
                });
            }

            auto dep = parse_dependency_inline_table(key, value, line_no);
            if (!dep) return std::unexpected(dep.error());

            if (current_section == Section::Dependencies) {
                manifest.dependencies.push_back(std::move(*dep));
            } else {
                manifest.dev_dependencies.push_back(std::move(*dep));
            }
            continue;
        }
    }

    if (manifest.name.empty()) {
        return std::unexpected(ManifestError{
            ManifestError::Code::MissingRequiredField,
            "missing required field [package].name",
            0,
        });
    }
    if (manifest.version.empty()) {
        return std::unexpected(ManifestError{
            ManifestError::Code::MissingRequiredField,
            "missing required field [package].version",
            0,
        });
    }
    if (manifest.license.empty()) {
        return std::unexpected(ManifestError{
            ManifestError::Code::MissingRequiredField,
            "missing required field [package].license",
            0,
        });
    }
    if (manifest.compatibility_eta.empty()) {
        return std::unexpected(ManifestError{
            ManifestError::Code::MissingRequiredField,
            "missing required field [compatibility].eta",
            0,
        });
    }

    if (!is_valid_package_name(manifest.name)) {
        return std::unexpected(ManifestError{
            ManifestError::Code::InvalidValue,
            "package.name must match [a-z][a-z0-9_-]{0,63}",
            0,
        });
    }
    if (!is_valid_semver(manifest.version)) {
        return std::unexpected(ManifestError{
            ManifestError::Code::InvalidValue,
            "package.version must be valid semver (MAJOR.MINOR.PATCH)",
            0,
        });
    }
    if (!looks_like_spdx_expression(manifest.license)) {
        return std::unexpected(ManifestError{
            ManifestError::Code::InvalidValue,
            "package.license must be a valid SPDX expression",
            0,
        });
    }
    if (!looks_like_semver_range(manifest.compatibility_eta)) {
        return std::unexpected(ManifestError{
            ManifestError::Code::InvalidValue,
            "compatibility.eta must be a valid semver range expression",
            0,
        });
    }

    std::sort(manifest.dependencies.begin(),
              manifest.dependencies.end(),
              [](const ManifestDependency& lhs, const ManifestDependency& rhs) {
                  return lhs.name < rhs.name;
              });
    std::sort(manifest.dev_dependencies.begin(),
              manifest.dev_dependencies.end(),
              [](const ManifestDependency& lhs, const ManifestDependency& rhs) {
                  return lhs.name < rhs.name;
              });
    return manifest;
}

ManifestResult read_manifest(const fs::path& manifest_path) {
    std::ifstream in(manifest_path, std::ios::in | std::ios::binary);
    if (!in) {
        return std::unexpected(ManifestError{
            ManifestError::Code::IoError,
            "cannot open manifest: " + manifest_path.string(),
            0,
        });
    }

    std::ostringstream buffer;
    buffer << in.rdbuf();
    auto parsed = parse_manifest(buffer.str(), manifest_path);
    if (!parsed) return parsed;
    parsed->manifest_path = manifest_path;
    return parsed;
}

std::string write_manifest(const Manifest& manifest) {
    Manifest normalized = manifest;
    std::sort(normalized.dependencies.begin(),
              normalized.dependencies.end(),
              [](const ManifestDependency& lhs, const ManifestDependency& rhs) {
                  return lhs.name < rhs.name;
              });
    std::sort(normalized.dev_dependencies.begin(),
              normalized.dev_dependencies.end(),
              [](const ManifestDependency& lhs, const ManifestDependency& rhs) {
                  return lhs.name < rhs.name;
              });

    std::ostringstream out;
    out << "[package]\n";
    out << "name = \"" << escape_string(normalized.name) << "\"\n";
    out << "version = \"" << escape_string(normalized.version) << "\"\n";
    out << "license = \"" << escape_string(normalized.license) << "\"\n";
    out << "\n";
    out << "[compatibility]\n";
    out << "eta = \"" << escape_string(normalized.compatibility_eta) << "\"\n";
    out << "\n";
    out << "[dependencies]\n";
    for (const auto& dep : normalized.dependencies) {
        out << render_dependency(dep) << "\n";
    }

    if (!normalized.dev_dependencies.empty()) {
        out << "\n[dev-dependencies]\n";
        for (const auto& dep : normalized.dev_dependencies) {
            out << render_dependency(dep) << "\n";
        }
    }

    return out.str();
}

std::expected<void, ManifestError> write_manifest_file(const Manifest& manifest,
                                                       const fs::path& manifest_path) {
    std::error_code ec;
    if (manifest_path.has_parent_path()) {
        fs::create_directories(manifest_path.parent_path(), ec);
        if (ec) {
            return std::unexpected(ManifestError{
                ManifestError::Code::IoError,
                "failed to create manifest parent directory: "
                    + manifest_path.parent_path().string(),
                0,
            });
        }
    }

    std::ofstream out(manifest_path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out) {
        return std::unexpected(ManifestError{
            ManifestError::Code::IoError,
            "cannot open manifest for writing: " + manifest_path.string(),
            0,
        });
    }

    out << write_manifest(manifest);
    if (!out.good()) {
        return std::unexpected(ManifestError{
            ManifestError::Code::IoError,
            "failed to write manifest: " + manifest_path.string(),
            0,
        });
    }
    return {};
}

} // namespace eta::package
