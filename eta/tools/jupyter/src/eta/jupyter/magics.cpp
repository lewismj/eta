#include "eta/jupyter/magics.h"

#include <algorithm>
#include <cctype>

namespace eta::jupyter {

namespace {

[[nodiscard]] std::string_view trim_left(std::string_view text) {
    const auto pos = text.find_first_not_of(" \t\r");
    if (pos == std::string_view::npos) return {};
    return text.substr(pos);
}

[[nodiscard]] std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r");
    return std::string(text.substr(first, last - first + 1));
}

[[nodiscard]] MagicName parse_magic_name(std::string_view name) {
    if (name == "time") return MagicName::Time;
    if (name == "timeit") return MagicName::Timeit;
    if (name == "bytecode") return MagicName::Bytecode;
    if (name == "load") return MagicName::Load;
    if (name == "run") return MagicName::Run;
    if (name == "env") return MagicName::Env;
    if (name == "cwd") return MagicName::Cwd;
    if (name == "import") return MagicName::Import;
    if (name == "reload") return MagicName::Reload;
    if (name == "who") return MagicName::Who;
    if (name == "whos") return MagicName::Whos;
    if (name == "plot") return MagicName::Plot;
    if (name == "table") return MagicName::Table;
    if (name == "trace") return MagicName::Trace;
    return MagicName::Unknown;
}

[[nodiscard]] ParsedMagic parse_magic_line(std::string_view line,
                                           MagicKind kind,
                                           std::string_view body) {
    ParsedMagic out;
    out.kind = kind;
    out.body = std::string(body);

    const std::size_t prefix = (kind == MagicKind::Cell) ? 2u : 1u;
    if (line.size() <= prefix) return out;

    auto rest = trim_left(line.substr(prefix));
    if (rest.empty()) return out;

    std::size_t split = 0;
    while (split < rest.size() && !std::isspace(static_cast<unsigned char>(rest[split]))) {
        ++split;
    }

    out.name_text = std::string(rest.substr(0, split));
    out.name = parse_magic_name(out.name_text);
    out.args = trim(rest.substr(split));
    return out;
}

} // namespace

ParsedMagic parse_magic(std::string_view code)
{
    std::size_t line_start = 0;
    while (line_start <= code.size()) {
        std::size_t line_end = code.find('\n', line_start);
        if (line_end == std::string_view::npos) line_end = code.size();

        auto line = code.substr(line_start, line_end - line_start);
        auto trimmed = trim_left(line);
        if (!trimmed.empty()) {
            if (trimmed.rfind("%%", 0) == 0) {
                std::string_view body;
                if (line_end < code.size()) {
                    body = code.substr(line_end + 1);
                }
                return parse_magic_line(trimmed, MagicKind::Cell, body);
            }
            if (trimmed.front() == '%') {
                return parse_magic_line(trimmed, MagicKind::Line, {});
            }
            return ParsedMagic{};
        }

        if (line_end == code.size()) break;
        line_start = line_end + 1;
    }
    return ParsedMagic{};
}

bool is_magic(std::string_view line)
{
    return parse_magic(line).kind != MagicKind::None;
}

std::string strip_magic_prefix(std::string_view line)
{
    if (line.size() >= 2 && line[0] == '%' && line[1] == '%') {
        return std::string(line.substr(2));
    }
    if (!line.empty() && line.front() == '%') {
        return std::string(line.substr(1));
    }
    return std::string(line);
}

std::string_view magic_name(MagicName name)
{
    switch (name) {
        case MagicName::Time: return "time";
        case MagicName::Timeit: return "timeit";
        case MagicName::Bytecode: return "bytecode";
        case MagicName::Load: return "load";
        case MagicName::Run: return "run";
        case MagicName::Env: return "env";
        case MagicName::Cwd: return "cwd";
        case MagicName::Import: return "import";
        case MagicName::Reload: return "reload";
        case MagicName::Who: return "who";
        case MagicName::Whos: return "whos";
        case MagicName::Plot: return "plot";
        case MagicName::Table: return "table";
        case MagicName::Trace: return "trace";
        case MagicName::Unknown:
        default:
            return "unknown";
    }
}

} // namespace eta::jupyter
