#include "config.h"

#include <charconv>
#include <cctype>
#include <expected>
#include <fstream>
#include <map>
#include <string_view>
#include <sstream>
#include <utility>

namespace eta::linter {
    namespace {
        std::string trim(std::string_view text) {
            const auto begin = text.find_first_not_of(" \t\r\n");
            if (begin == std::string_view::npos) return {};
            const auto end = text.find_last_not_of(" \t\r\n");
            return std::string(text.substr(begin, end - begin + 1));
        }

        std::string strip_comment(std::string_view line) {
            bool quoted = false;
            bool escaped = false;
            for (std::size_t i = 0; i < line.size(); ++i) {
                const char ch = line[i];
                if (escaped) { escaped = false; continue; }
                if (ch == '\\' && quoted) { escaped = true; continue; }
                if (ch == '"') { quoted = !quoted; continue; }
                if (ch == '#' && !quoted) return trim(line.substr(0, i));
            }
            return trim(line);
        }

        bool quoted(std::string_view text) { return text.size() >= 2 && text.front() == '"' && text.back() == '"'; }

        std::expected<std::string, std::string> parse_string(std::string_view text) {
            if (!quoted(text)) return std::unexpected("expected a quoted string");
            std::string out;
            for (std::size_t i = 1; i + 1 < text.size(); ++i) {
                const char ch = text[i];
                if (ch == '\\') {
                    if (i + 1 >= text.size() - 1) return std::unexpected("unterminated escape sequence");
                    const char next = text[++i];
                    switch (next) {
                        case '\\': case '"': out.push_back(next); break;
                        case 'n': out.push_back('\n'); break;
                        case 'r': out.push_back('\r'); break;
                        case 't': out.push_back('\t'); break;
                        default: return std::unexpected("unsupported escape sequence");
                    }
                } else out.push_back(ch);
            }
            return out;
        }

        std::expected<std::vector<std::string>, std::string> parse_string_list(std::string_view text) {
            if (text.size() < 2 || text.front() != '[' || text.back() != ']') return std::unexpected("expected a string list");
            std::vector<std::string> out;
            std::size_t cursor = 1;
            while (cursor + 1 < text.size()) {
                while (cursor + 1 < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) ++cursor;
                if (cursor + 1 >= text.size() || text[cursor] == ']') break;
                if (text[cursor] != '"') return std::unexpected("expected a quoted string in list");
                ++cursor;
                std::string value;
                while (cursor < text.size()) {
                    const char ch = text[cursor++];
                    if (ch == '\\') {
                        if (cursor >= text.size()) return std::unexpected("unterminated escape sequence");
                        const char next = text[cursor++];
                        switch (next) {
                            case '\\': case '"': value.push_back(next); break;
                            case 'n': value.push_back('\n'); break;
                            case 'r': value.push_back('\r'); break;
                            case 't': value.push_back('\t'); break;
                            default: return std::unexpected("unsupported escape sequence");
                        }
                    } else if (ch == '"') {
                        break;
                    } else {
                        value.push_back(ch);
                    }
                }
                out.push_back(std::move(value));
                while (cursor + 1 < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) ++cursor;
                if (cursor + 1 < text.size() && text[cursor] == ',') ++cursor;
            }
            return out;
        }

        std::expected<std::size_t, std::string> parse_size(std::string_view text) {
            std::size_t value = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return std::unexpected("expected an unsigned integer");
            return value;
        }

        std::vector<std::string> split_top_level(std::string_view text, char separator) {
            std::vector<std::string> parts;
            std::size_t start = 0;
            bool quoted_text = false;
            bool escaped = false;
            int braces = 0;
            for (std::size_t i = 0; i < text.size(); ++i) {
                const char ch = text[i];
                if (escaped) {
                    escaped = false;
                    continue;
                }
                if (quoted_text) {
                    if (ch == '\\') escaped = true;
                    else if (ch == '"') quoted_text = false;
                    continue;
                }
                if (ch == '"') {
                    quoted_text = true;
                    continue;
                }
                if (ch == '{') ++braces;
                else if (ch == '}') --braces;
                else if (ch == separator && braces == 0) {
                    parts.push_back(trim(text.substr(start, i - start)));
                    start = i + 1;
                }
            }
            parts.push_back(trim(text.substr(start)));
            return parts;
        }

        std::expected<RuleConfigOverride, std::string> parse_rule_override(const std::string &key, std::string_view value) {
            RuleConfigOverride override;
            if (value == "\"off\"" || value == "off") {
                override.disabled = true;
                return override;
            }
            if (value.size() < 2 || value.front() != '{' || value.back() != '}') {
                return std::unexpected("rule " + key + " must be \"off\" or an inline table");
            }
            const auto entries = split_top_level(value.substr(1, value.size() - 2), ',');
            for (const auto &entry : entries) {
                if (entry.empty()) continue;
                const auto equals = entry.find('=');
                if (equals == std::string::npos) return std::unexpected("invalid rule override entry for " + key);
                const auto name = trim(std::string_view(entry).substr(0, equals));
                const auto raw_value = trim(std::string_view(entry).substr(equals + 1));
                if (name == "severity") {
                    auto parsed = parse_string(raw_value);
                    if (!parsed) return std::unexpected(parsed.error());
                    if (*parsed != "error" && *parsed != "warn" && *parsed != "info") {
                        return std::unexpected("invalid severity for rule " + key + ": " + *parsed);
                    }
                    override.severity = *parsed;
                    continue;
                }
                auto numeric = parse_size(raw_value);
                if (!numeric) return std::unexpected("unsupported rule override key for " + key + ": " + name);
                override.integer_options[name] = *numeric;
            }
            return override;
        }
    }

    std::expected<LintConfig, std::string> load_lint_config(const std::filesystem::path &path) {
        std::ifstream in(path);
        if (!in) return std::unexpected(std::string("cannot open config file: ") + path.string());
        enum class Section { None, EtaLint, Rules };
        Section section = Section::None;
        LintConfig config;
        std::string raw;
        std::size_t line_number = 0;
        while (std::getline(in, raw)) {
            ++line_number;
            const auto line = strip_comment(raw);
            if (line.empty()) continue;
            if (line.front() == '[' && line.back() == ']') {
                const auto name = trim(std::string_view(line).substr(1, line.size() - 2));
                if (name == "eta-lint") section = Section::EtaLint;
                else if (name == "rules") section = Section::Rules;
                else return std::unexpected("unknown config section on line " + std::to_string(line_number));
                continue;
            }
            const auto equals = line.find('=');
            if (equals == std::string::npos) return std::unexpected("expected key = value on line " + std::to_string(line_number));
            const auto key = trim(std::string_view(line).substr(0, equals));
            const auto value = trim(std::string_view(line).substr(equals + 1));
            if (section == Section::EtaLint) {
                if (key == "preset") {
                    auto parsed = parse_string(value);
                    if (!parsed) return std::unexpected(parsed.error() + " on line " + std::to_string(line_number));
                    config.preset = *parsed;
                } else if (key == "max-warnings") {
                    auto parsed = parse_size(value);
                    if (!parsed) return std::unexpected(parsed.error() + " on line " + std::to_string(line_number));
                    config.max_warnings = *parsed;
                } else if (key == "include") {
                    auto parsed = parse_string_list(value);
                    if (!parsed) return std::unexpected(parsed.error() + " on line " + std::to_string(line_number));
                    config.includes.insert(config.includes.end(), parsed->begin(), parsed->end());
                } else if (key == "exclude") {
                    auto parsed = parse_string_list(value);
                    if (!parsed) return std::unexpected(parsed.error() + " on line " + std::to_string(line_number));
                    config.excludes.insert(config.excludes.end(), parsed->begin(), parsed->end());
                } else {
                    return std::unexpected("unknown config key on line " + std::to_string(line_number));
                }
            } else if (section == Section::Rules) {
                auto parsed = parse_rule_override(key, value);
                if (!parsed) return std::unexpected(parsed.error() + " on line " + std::to_string(line_number));
                config.rules[key] = std::move(*parsed);
            } else {
                return std::unexpected("configuration entries must appear under [eta-lint] or [rules]");
            }
        }
        return config;
    }

    std::optional<std::filesystem::path> find_lint_config(const std::filesystem::path &start) {
        auto current = std::filesystem::absolute(start);
        if (!std::filesystem::is_directory(current)) current = current.parent_path();
        for (;;) {
            const auto candidate = current / ".eta-lint.toml";
            if (std::filesystem::exists(candidate)) return candidate;
            if (current == current.root_path()) break;
            const auto parent = current.parent_path();
            if (parent == current) break;
            current = parent;
        }
        return std::nullopt;
    }
}

