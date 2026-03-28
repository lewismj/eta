#pragma once

#include <compare>
#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "eta/semantics/core_ir.h"
#include "eta/reader/module_linker.h"

namespace eta::semantics {

struct SemanticError {
    enum class Kind : std::uint16_t {
        UndefinedName,
        DuplicateDefinition,
        ArityMismatch,
        NonFunctionCall,
        InvalidFormShape,
        ImmutableAssignment,
        SetOnImported,
        InvalidLetrecInit,
        ExportOfUnknownBinding
    } kind{};
    eta::reader::parser::Span span{};
    std::string message;
};

template <typename T>
using SemResult = std::expected<T, SemanticError>;

struct BindingInfo {
    enum class Kind : std::uint8_t { Param, Local, Global, Import } kind{};
    std::string name;                         // identifier text
    bool mutable_flag{false};                 // globals true; lexicals false
    eta::reader::parser::Span def_span{};     // site of define/param/let
    std::optional<eta::reader::linker::ImportOrigin> origin; // for imports
};

struct Scope {
    Scope* parent{nullptr};
    std::unordered_map<std::string, core::BindingId> table; // string keys by design
    bool is_lambda_boundary{false};
    core::Lambda* lambda_node{nullptr}; // for upval tracking
};

struct ModuleSemantics {
    std::string name;
    std::deque<core::Node> nodes;                 // stable storage for nodes
    std::vector<core::Node*> toplevel_inits;      // ordered module initializer forms
    std::unordered_map<std::string, core::BindingId> exports; // export name -> binding
    std::vector<BindingInfo> bindings;            // indexed by BindingId

    // Helper to construct nodes in-place and get a stable pointer
    template <class Alt, class... Args>
    core::Node* emplace(Args&&... args) {
        nodes.emplace_back(Alt{std::forward<Args>(args)...});
        return &nodes.back();
    }
};

class SemanticAnalyzer {
public:
    SemResult<std::vector<ModuleSemantics>> analyze_all(
        std::span<const eta::reader::parser::SExprPtr> forms,
        const eta::reader::ModuleLinker& linker);

    void constant_fold(ModuleSemantics& mod);
};

} // namespace eta::semantics
