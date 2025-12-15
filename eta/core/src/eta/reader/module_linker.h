#pragma once

#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "eta/reader/parser.h"

namespace eta::reader::linker {

    using parser::SExpr;
    using parser::SExprPtr;
    using parser::List;
    using parser::Symbol;
    using parser::Span;

    struct LinkError {
        enum class Kind : std::uint8_t {
            UnknownModule,          // import source missing
            ExportOfUnknownName,    // (export x) but x not defined in that module
            ConflictingImport,      // local name clashes with local define or prior import
            NameNotExported,        // only/rename names not exported by source OR not in the current import set
            DuplicateModule         // two modules with the same name
        } kind{};
        Span span{};
        std::string message;
    };

    template <typename T>
    using LinkResult = std::expected<T, LinkError>;

    // For provenance of imported symbols
    struct ImportOrigin {
        std::string from_module; // source module name
        std::string remote_name; // remote exported name
        Span where;              // span of the import clause
    };

    enum class ModuleState : std::uint8_t { Indexed, Linked };

    struct ModuleTable {
        std::string name;
        std::unordered_set<std::string> defined;             // names defined in this module
        std::unordered_set<std::string> exports;             // names exported (declared)
        std::unordered_set<std::string> visible;             // After link(): defined ∪ imported locals

        // Diagnostics & provenance
        std::unordered_map<std::string, Span> define_spans;  // name -> span of its define
        std::unordered_map<std::string, Span> export_spans;  // name -> span where exported
        std::unordered_map<std::string, ImportOrigin> import_origins; // local -> origin

        // Lifecycle
        ModuleState state{ModuleState::Indexed};

        // Invariants:
        //  - Before link(): visible is empty; state == Indexed
        //  - After  link(): visible = defined ∪ { all imported local names }; state == Linked
    };

    struct ImportSpec {
        enum class Kind { Plain, Only, Except, Rename } kind{Kind::Plain};
        std::string module;
        std::vector<std::string> ids; // for Only/Except
        std::vector<std::pair<std::string,std::string>> renames; // (old,new) for Rename
        Span span{};                     // span of the clause (or symbol for Plain)
    };

    struct PendingImport {
        std::string target;   // target module name (receiver)
        ImportSpec spec;      // import clause
        Span where;           // site of the (import ...) form or clause
    };

    class ModuleLinker {
    public:
        ModuleLinker() = default;
        // Input: top-level forms post-expander (many module lists possible)
        // PRECONDITION: forms are expander-normalized `(module name ... )` lists (see expander::handle_module_list)
        // LIFETIME: does not retain references to `forms`; they may be destroyed after return.
        LinkResult<void> index_modules(std::span<const SExprPtr> forms);

        // Resolve pending imports into ModuleTable.visible and fill `import_origins`
        LinkResult<void> link();

        // Lookup: idiomatic optional ref
        std::optional<std::reference_wrapper<const ModuleTable>> get(const std::string& name) const;

    private:
        std::unordered_map<std::string, ModuleTable> modules_; // name -> table
        std::unordered_map<std::string, std::vector<PendingImport>> pending_; // target -> imports

        // Helpers
        static bool is_symbol_named(const SExprPtr& p, std::string_view name);
        static const List* as_list(const SExprPtr& p) { return (p && p->is<List>()) ? p->as<List>() : nullptr; }
        static const Symbol* as_symbol(const SExprPtr& p) { return (p && p->is<Symbol>()) ? p->as<Symbol>() : nullptr; }

        LinkResult<void> scan_module_body(const List& module_form, ModuleTable& mt);
        LinkResult<void> parse_import_form(ModuleTable& mt, const List& import_form);
        LinkResult<ImportSpec> parse_import_clause(const SExprPtr& clause);
        static std::string to_string(const LinkError::Kind k);
    };

} // namespace eta::reader::linker

// Re-export linker API into eta::reader to match existing includes/usages in tests
namespace eta::reader {
    using linker::LinkError;
    template <typename T>
    using LinkResult = linker::LinkResult<T>;
    using linker::ImportOrigin;
    using linker::ModuleState;
    using linker::ModuleTable;
    using linker::ModuleLinker;
}
