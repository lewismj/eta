#pragma once

#include <compare>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "eta/semantics/core_ir.h"
#include "eta/semantics/arena.h"
#include "eta/reader/module_linker.h"
#include "eta/runtime/builtin_env.h"

namespace eta::semantics {

/**
 * @brief Semantic analysis error
 *
 * These errors are convertible to eta::diagnostic::Diagnostic via the
 * to_diagnostic<SemanticError>() specialization in diagnostic.h.
 */
struct SemanticError {
    enum class Kind : std::uint16_t {
        UndefinedName,
        DuplicateDefinition,
        NonFunctionCall,
        InvalidFormShape,
        ImmutableAssignment,
        SetOnImported,
        InvalidLetrecInit,
        ExportOfUnknownBinding,
    } kind{};
    eta::reader::parser::Span span{};
    std::string message;
};

template <typename T>
using SemResult = std::expected<T, SemanticError>;

/**
 * @brief Information about a binding (variable, parameter, etc.)
 */
struct BindingInfo {
    enum class Kind : std::uint8_t { Param, Local, Global, Import } kind{};
    std::string name;                         // identifier text
    bool mutable_flag{false};                 // globals true; lexicals false
    std::uint16_t slot{0};                    // VM stack slot (for locals) or global ID
    eta::reader::parser::Span def_span{};     // site of define/param/let
    std::optional<eta::reader::linker::ImportOrigin> origin; // for imports
};

/**
 * @brief Lexical scope during semantic analysis
 */
struct Scope {
    Scope* parent{nullptr};
    std::unordered_map<std::string, core::BindingId> table; // string keys by design
    bool is_lambda_boundary{false};
    core::Lambda* lambda_node{nullptr}; // for upval tracking
    std::uint16_t next_slot{0};         // Next available VM slot in this lambda's frame
};

/**
 * @brief Semantic analysis result for a single module
 *
 * Ownership model:
 * - The `arena` provides stable addresses for all IR nodes via block allocation
 * - All Node* pointers within the IR reference nodes allocated from the arena
 * - The ModuleSemantics owns all nodes and they are valid for its lifetime
 *
 * For optimization passes that need to transform nodes:
 * - Use the emplace() helper to create new nodes in the arena
 * - To replace a node, create the new node and update pointers
 * - The old node remains in memory (arena frees all at destruction)
 */
struct ModuleSemantics {
    std::string name;
    core::Arena arena;                        // Arena allocator for IR nodes
    std::vector<core::Node*> toplevel_inits;  // ordered module initializer forms
    std::unordered_map<std::string, core::BindingId> exports; // export name -> binding
    std::vector<BindingInfo> bindings;        // indexed by BindingId
    std::uint32_t stack_size{0};              // stack size for module init function
    std::uint32_t total_globals{0};           // unified global count (shared across all modules)

    /**
     * @brief Construct a node in-place and get a stable pointer
     *
     * This is the preferred way to create IR nodes. The returned pointer
     * is stable for the lifetime of this ModuleSemantics.
     *
     * Uses the Arena allocator for better memory locality and cache performance.
     */
    template <class Alt, class... Args>
    core::Node* emplace(const eta::reader::parser::Span& span, Args&&... args) {
        // Allocate in arena for stable pointer and better cache performance
        auto* node = arena.alloc<core::Node>(Alt{std::forward<Args>(args)...}, span);
        return node;
    }
};

/**
 * @brief Semantic analyzer for the eta language
 *
 * The semantic analyzer operates on expanded S-expressions (from the Expander)
 * and produces a Core IR suitable for code generation.
 *
 * Expected input forms (Core IR - should be fully desugared):
 * - if, begin, set!, lambda, quote
 * - dynamic-wind, values, call-with-values, call/cc
 * - function application
 *
 * Note: Derived forms (let, letrec, case, do) are no longer handled by the
 * SemanticAnalyzer. They MUST be desugared by the Expander before analysis.
 */
class SemanticAnalyzer {
public:
    SemResult<std::vector<ModuleSemantics>> analyze_all(
        std::span<const eta::reader::parser::SExprPtr> forms,
        const eta::reader::ModuleLinker& linker,
        const eta::runtime::BuiltinEnvironment& builtins);

    /// Overload without builtins — uses an empty environment (for backward compatibility)
    SemResult<std::vector<ModuleSemantics>> analyze_all(
        std::span<const eta::reader::parser::SExprPtr> forms,
        const eta::reader::ModuleLinker& linker);
};

} // namespace eta::semantics

