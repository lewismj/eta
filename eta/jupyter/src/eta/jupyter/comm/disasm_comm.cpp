#include "eta/jupyter/comm/disasm_comm.h"

#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "eta/runtime/vm/disassembler.h"
#include "eta/session/driver.h"

namespace eta::jupyter::comm {

namespace {

[[nodiscard]] std::string escape_html(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        switch (ch) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

[[nodiscard]] const eta::runtime::vm::BytecodeFunction* find_function(
    const eta::semantics::BytecodeFunctionRegistry& registry,
    std::string_view function_name) {
    if (function_name.empty()) return nullptr;

    for (const auto& fn : registry.all()) {
        if (fn.name == function_name) return &fn;
    }
    const std::string suffix = "." + std::string(function_name);
    for (const auto& fn : registry.all()) {
        if (fn.name.ends_with(suffix)) return &fn;
    }
    for (const auto& fn : registry.all()) {
        if (fn.name.find(function_name) != std::string::npos) return &fn;
    }
    return nullptr;
}

} // namespace

std::string_view disasm_target()
{
    return "eta.disasm";
}

nlohmann::json build_disassembly(eta::session::Driver& driver,
                                 std::string_view scope,
                                 std::string_view function_name)
{
    nlohmann::json out = nlohmann::json::object();
    out["version"] = 1;
    out["scope"] = std::string(scope);

    eta::runtime::vm::Disassembler disasm(driver.heap(), driver.intern_table());
    std::ostringstream oss;

    std::string selected_name(function_name);
    bool rendered = false;

    if (scope == "all") {
        disasm.disassemble_all(driver.registry(), oss);
        rendered = true;
        selected_name = "all";
    } else {
        const eta::runtime::vm::BytecodeFunction* target = nullptr;
        if (!function_name.empty()) {
            target = find_function(driver.registry(), function_name);
        } else {
            const auto frames = driver.vm().get_frames();
            if (!frames.empty()) {
                selected_name = frames.front().func_name;
                target = find_function(driver.registry(), selected_name);
            }
        }

        if (target) {
            disasm.disassemble(*target, oss);
            rendered = true;
            selected_name = target->name;
        }
    }

    if (!rendered) {
        if (!selected_name.empty()) {
            oss << "; Function not found in registry: " << selected_name << '\n';
        } else {
            oss << "; No active frame to disassemble.\n";
        }
    }

    out["function"] = selected_name;
    out["currentPC"] = driver.vm().paused_instruction_index();
    out["text"] = oss.str();
    return out;
}

std::string disassembly_html(const nlohmann::json& payload)
{
    std::string html;
    html += "<div class=\"eta-disassembly\">";
    html += "<div><strong>Disassembly</strong> ";
    html += escape_html(payload.value("function", ""));
    html += "</div>";
    html += "<pre>";
    html += escape_html(payload.value("text", ""));
    html += "</pre></div>";
    return html;
}

} // namespace eta::jupyter::comm
