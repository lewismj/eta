/**
 * @file repl_controller.cpp
 * @brief REPL and notebook presentation controller implementation.
 */

#include "eta/session/repl_controller.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

#include "eta/docs/markdown.h"
#include "eta/docs/stdlib_docs.h"
#include "eta/interpreter/repl_complete.h"
#include "eta/reader/special_form_docs.h"
#include "eta/runtime/builtin_metadata.h"
#include "eta/session/display_classifier.h"
#include "eta/session/repl_input.h"

namespace eta::session {

bool ReplController::eval_string(std::string source, std::string& out) {
    out.clear();
    auto forms = split_toplevel_forms(source);
    if (forms.empty()) return true;

    auto wrapped = interpreter::wrap_repl_submission(
        forms,
        repl_counter_++,
        runtime_.has_module("std.prelude"),
        repl_modules_);

    runtime::nanbox::LispVal result{runtime::nanbox::Nil};
    const bool ok = wrapped.last_is_expr
        ? runtime_.run_source(wrapped.source, &result, wrapped.result_name)
        : runtime_.run_source(wrapped.source);
    if (!ok) return false;

    repl_modules_.push_back(interpreter::PriorModule{
        wrapped.module_name,
        wrapped.user_defines,
        wrapped.user_imports});
    if (wrapped.last_is_expr && result != runtime::nanbox::Nil) {
        out = runtime_.format_value(result, runtime::FormatMode::Write);
    }
    return true;
}

DisplayValue ReplController::eval_to_display(const std::string& source) {
    auto forms = split_toplevel_forms(source);
    if (forms.empty()) return DisplayValue{};

    auto wrapped = interpreter::wrap_repl_submission(
        forms,
        repl_counter_++,
        runtime_.has_module("std.prelude"),
        repl_modules_);

    runtime::nanbox::LispVal result{runtime::nanbox::Nil};
    const bool ok = wrapped.last_is_expr
        ? runtime_.run_source(wrapped.source, &result, wrapped.result_name)
        : runtime_.run_source(wrapped.source);
    if (!ok) {
        return DisplayValue{
            .tag = DisplayTag::Error,
            .text = runtime_.diagnostics_to_string(),
            .value = runtime::nanbox::Nil,
        };
    }

    repl_modules_.push_back(interpreter::PriorModule{
        wrapped.module_name,
        wrapped.user_defines,
        wrapped.user_imports});
    if (!wrapped.last_is_expr || result == runtime::nanbox::Nil) {
        return DisplayValue{
            .tag = DisplayTag::Text,
            .text = {},
            .value = runtime::nanbox::Nil,
        };
    }

    return DisplayValue{
        .tag = display_classifier_.classify_display_tag(result),
        .text = runtime_.format_value(result, runtime::FormatMode::Write),
        .value = result,
    };
}

ReplController::CompletionResult ReplController::completions_at(
    const std::string& source,
    std::size_t cursor_pos) const {
    if (cursor_pos > source.size()) cursor_pos = source.size();

    const auto tok = interpreter::repl_complete::token_at(source, cursor_pos);
    CompletionResult out{
        .matches = {},
        .cursor_start = tok.start,
        .cursor_end = tok.end,
    };

    std::string prefix;
    if (!tok.text.empty() && tok.start <= cursor_pos && cursor_pos <= tok.end) {
        prefix = source.substr(tok.start, cursor_pos - tok.start);
    } else {
        out.cursor_start = cursor_pos;
        out.cursor_end = cursor_pos;
    }

    std::unordered_set<std::string> candidates;
    auto add_candidate = [&candidates](std::string name) {
        if (!name.empty()) candidates.insert(std::move(name));
    };

    for (const auto& entry : reader::special_form_docs()) {
        add_candidate(std::string(entry.name));
    }
    for (const auto& builtin : runtime::builtin_metadata()) {
        add_candidate(builtin.name);
    }
    for (const auto& stdlib_doc : docs::stdlib_doc_registry()) {
        add_candidate(std::string(stdlib_doc.name));
        add_candidate(std::string(stdlib_doc.qualified_name));
    }

    for (const auto& [_, qualified] : runtime_.global_names()) {
        add_candidate(qualified);
        const auto dot = qualified.find_last_of('.');
        if (dot != std::string::npos && dot + 1 < qualified.size()) {
            add_candidate(qualified.substr(dot + 1));
        }
    }

    for (auto& mod : runtime_.discover_module_names()) {
        add_candidate(std::move(mod));
    }

    out.matches.reserve(candidates.size());
    for (const auto& name : candidates) {
        if (prefix.empty() || name.starts_with(prefix)) {
            out.matches.push_back(name);
        }
    }
    std::sort(out.matches.begin(), out.matches.end());
    return out;
}

std::string ReplController::hover_at(const std::string& symbol) const {
    if (symbol.empty()) return {};

    if (auto entry = reader::lookup_special_form_doc(symbol)) {
        return docs::render_markdown(*entry);
    }
    if (auto builtin = runtime::lookup_builtin_metadata(symbol)) {
        return docs::render_builtin_markdown(*builtin);
    }
    if (auto stdlib_doc = docs::lookup_stdlib_doc(symbol)) {
        return docs::render_markdown(*stdlib_doc);
    }

    for (const auto& [_, qualified] : runtime_.global_names()) {
        if (qualified == symbol) {
            const auto dot = qualified.find_last_of('.');
            if (dot != std::string::npos) {
                const auto mod = qualified.substr(0, dot);
                const auto name = qualified.substr(dot + 1);
                return "**" + name + "**  -  binding from `" + mod + "`.";
            }
            return "**" + qualified + "**  -  module binding.";
        }

        const auto dot = qualified.find_last_of('.');
        if (dot != std::string::npos && dot + 1 < qualified.size()) {
            const auto short_name = qualified.substr(dot + 1);
            if (short_name == symbol) {
                const auto mod = qualified.substr(0, dot);
                return "**" + short_name + "**  -  binding from `" + mod + "`.";
            }
        }
    }

    return {};
}

void ReplController::forget_module(std::string_view module_name) {
    repl_modules_.erase(
        std::remove_if(
            repl_modules_.begin(),
            repl_modules_.end(),
            [module_name](const interpreter::PriorModule& prior) {
                return prior.name == module_name;
            }),
        repl_modules_.end());
}

} // namespace eta::session
