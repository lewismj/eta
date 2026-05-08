#pragma once

#include <array>
#include <optional>
#include <span>
#include <string_view>

#include "eta/docs/doc_entry.h"

namespace eta::reader {

/**
 * @brief Canonical documentation table for language-level special forms.
 */
inline constexpr std::array<eta::docs::DocEntry, 40> kSpecialFormDocs = {{
    {"define", "(define name expr)", "Define a variable or function.", eta::docs::DocKind::SpecialForm, {}, "Core", {}},
    {"lambda", "(lambda (args...) body...)", "Create an anonymous function.", eta::docs::DocKind::SpecialForm, {}, "Core", {}},
    {"if", "(if test consequent alternate)", "Conditional expression.", eta::docs::DocKind::SpecialForm, {}, "Core", {}},
    {"begin", "(begin expr...)", "Sequence expressions.", eta::docs::DocKind::SpecialForm, {}, "Core", {}},
    {"set!", "(set! name expr)", "Mutate a variable binding.", eta::docs::DocKind::SpecialForm, {}, "Core", {}},
    {"quote", "(quote datum)", "Return datum without evaluation.", eta::docs::DocKind::SpecialForm, "Reader shorthand: `'datum`", "Core", {}},
    {"let", "(let ((var init)...) body...)", "Parallel local bindings.", eta::docs::DocKind::SpecialForm, {}, "Binding", {}},
    {"let*", "(let* ((var init)...) body...)", "Sequential local bindings.", eta::docs::DocKind::SpecialForm, {}, "Binding", {}},
    {"letrec", "(letrec ((var init)...) body...)", "Recursive local bindings.", eta::docs::DocKind::SpecialForm, {}, "Binding", {}},
    {"letrec*", "(letrec* ((var init)...) body...)", "Sequential recursive bindings.", eta::docs::DocKind::SpecialForm, {}, "Binding", {}},
    {"cond", "(cond (test expr...)... (else expr...))", "Multi-way conditional.", eta::docs::DocKind::SpecialForm, {}, "Control", {}},
    {"case", "(case key ((datum...) expr...)... (else expr...))", "Dispatch on datum equality.", eta::docs::DocKind::SpecialForm, {}, "Control", {}},
    {"and", "(and expr...)", "Short-circuit logical and.", eta::docs::DocKind::SpecialForm, {}, "Control", {}},
    {"or", "(or expr...)", "Short-circuit logical or.", eta::docs::DocKind::SpecialForm, {}, "Control", {}},
    {"when", "(when test body...)", "One-armed conditional.", eta::docs::DocKind::SpecialForm, {}, "Control", {}},
    {"unless", "(unless test body...)", "Negated one-armed conditional.", eta::docs::DocKind::SpecialForm, {}, "Control", {}},
    {"do", "(do ((var init step)...) (test result...) body...)", "Iteration construct.", eta::docs::DocKind::SpecialForm, {}, "Control", {}},
    {"module", "(module name body...)", "Declare a module.", eta::docs::DocKind::SpecialForm, {}, "Module", {}},
    {"import", "(import module-spec...)", "Import bindings from a module.", eta::docs::DocKind::SpecialForm, {}, "Module", {}},
    {"export", "(export name...)", "Export bindings from a module.", eta::docs::DocKind::SpecialForm, {}, "Module", {}},
    {"define-syntax", "(define-syntax name (syntax-rules ...))", "Define a hygienic macro.", eta::docs::DocKind::Macro, {}, "Macro", {}},
    {"syntax-rules", "(syntax-rules (literals...) (pattern template)...)", "Macro transformer pattern system.", eta::docs::DocKind::Macro, {}, "Macro", {}},
    {"define-record-type", "(define-record-type name (ctor field...) pred ...)", "Define a record type.", eta::docs::DocKind::SpecialForm, {}, "Record", {}},
    {"def", "(def name expr)", "Alias for define.", eta::docs::DocKind::SpecialForm, {}, "Alias", {}},
    {"defun", "(defun name (args...) body...)", "Alias for function definition.", eta::docs::DocKind::SpecialForm, {}, "Alias", {}},
    {"progn", "(progn expr...)", "Alias for begin.", eta::docs::DocKind::SpecialForm, {}, "Alias", {}},
    {"quasiquote", "(quasiquote datum)", "Template with unquote forms.", eta::docs::DocKind::SpecialForm, "Reader shorthand: `` `(datum ,x ,@xs) ``", "Core", {}},
    {"call/cc", "(call/cc proc)", "Call with current continuation.", eta::docs::DocKind::SpecialForm, {}, "Advanced", {}},
    {"call-with-current-continuation", "(call-with-current-continuation proc)", "Full-name alias for call/cc.", eta::docs::DocKind::SpecialForm, {}, "Advanced", {}},
    {"dynamic-wind", "(dynamic-wind before thunk after)", "Guard entry/exit across continuation transfers.", eta::docs::DocKind::SpecialForm, {}, "Advanced", {}},
    {"values", "(values expr...)", "Return multiple values.", eta::docs::DocKind::SpecialForm, {}, "Advanced", {}},
    {"call-with-values", "(call-with-values producer consumer)", "Consume multiple values.", eta::docs::DocKind::SpecialForm, {}, "Advanced", {}},
    {"raise", "(raise value) or (raise 'tag value)", "Raise an exception.", eta::docs::DocKind::SpecialForm, {}, "Exception", {}},
    {"catch", "(catch body) or (catch 'tag body)", "Handle an exception by tag.", eta::docs::DocKind::SpecialForm, {}, "Exception", {}},
    {"logic-var", "(logic-var)", "Create a fresh logic variable.", eta::docs::DocKind::SpecialForm, {}, "Logic", {}},
    {"unify", "(unify a b)", "Unify two terms.", eta::docs::DocKind::SpecialForm, {}, "Logic", {}},
    {"deref-lvar", "(deref-lvar lvar)", "Dereference a logic variable.", eta::docs::DocKind::SpecialForm, {}, "Logic", {}},
    {"trail-mark", "(trail-mark)", "Capture the current trail mark.", eta::docs::DocKind::SpecialForm, {}, "Logic", {}},
    {"unwind-trail", "(unwind-trail mark)", "Rollback bindings to a saved trail mark.", eta::docs::DocKind::SpecialForm, {}, "Logic", {}},
    {"copy-term", "(copy-term term)", "Copy a term with fresh logic variables.", eta::docs::DocKind::SpecialForm, {}, "Logic", {}},
}};

/**
 * @brief Intentional name overlaps between special forms and builtins.
 *
 * Resolution precedence for hover/signature/completion is special-form first,
 * then builtin metadata.
 */
inline constexpr std::array<std::string_view, 1> kAllowedSpecialFormBuiltinCollisions = {{
    "apply",
}};

/**
 * @brief Enumerate all known special-form documentation entries.
 */
inline constexpr std::span<const eta::docs::DocEntry> special_form_docs() {
    return std::span<const eta::docs::DocEntry>(kSpecialFormDocs.data(), kSpecialFormDocs.size());
}

/**
 * @brief Lookup a special form by exact name.
 */
inline std::optional<eta::docs::DocEntry> lookup_special_form_doc(std::string_view name) {
    for (const auto& entry : kSpecialFormDocs) {
        if (entry.name == name) return entry;
    }
    return std::nullopt;
}

/**
 * @brief Return true when @p name is an allowed builtin/special-form overlap.
 */
inline constexpr bool is_allowed_special_form_builtin_collision(std::string_view name) {
    for (auto allowed : kAllowedSpecialFormBuiltinCollisions) {
        if (allowed == name) return true;
    }
    return false;
}

} // namespace eta::reader
