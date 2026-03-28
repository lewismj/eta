#pragma once

#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <ostream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "eta/reader/parser.h"
#include "eta/reader/sexpr_utils.h"
#include "eta/reader/error_format.h"

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
            DuplicateModule,        // two modules with the same name
            CircularDependency      // modules form a dependency cycle
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

        //! Diagnostics & provenance
        std::unordered_map<std::string, Span> define_spans;  // name -> span of its define
        std::unordered_map<std::string, Span> export_spans;  // name -> span where exported
        std::unordered_map<std::string, ImportOrigin> import_origins; // local -> origin

        //! Lifecycle
        ModuleState state{ModuleState::Indexed};

        //! Invariants:
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
        std::string target;   //! target module name (receiver)
        ImportSpec spec;      //! import clause
        Span where;           //! site of the (import ...) form or clause
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


        LinkResult<void> scan_module_body(const List& module_form, ModuleTable& mt);
        LinkResult<void> parse_import_form(ModuleTable& mt, const List& import_form);
        LinkResult<ImportSpec> parse_import_clause(const SExprPtr& clause);
    };

} // namespace eta::reader::linker

// Enum printer for LinkError::Kind
namespace eta::reader::linker {

    constexpr const char* to_string(LinkError::Kind k) noexcept {
        using enum LinkError::Kind;
        switch (k) {
            case UnknownModule:         return "LinkError::Kind::UnknownModule";
            case ExportOfUnknownName:   return "LinkError::Kind::ExportOfUnknownName";
            case ConflictingImport:     return "LinkError::Kind::ConflictingImport";
            case NameNotExported:       return "LinkError::Kind::NameNotExported";
            case DuplicateModule:       return "LinkError::Kind::DuplicateModule";
            case CircularDependency:    return "LinkError::Kind::CircularDependency";
        }
        return "LinkError::Kind::Unknown";
    }


    inline std::ostream& operator<<(std::ostream& os, const LinkError& e) {
        os << to_string(e.kind) << " at ";
        write_span(os, e.span);
        if (!e.message.empty()) os << ": " << e.message;
        return os;
    }

} // namespace eta::reader::linker

//! Re-export linker API into eta::reader to match existing includes/usages in tests
namespace eta::reader {
    using linker::LinkError;
    template <typename T>
    using LinkResult = linker::LinkResult<T>;
    using linker::ImportOrigin;
    using linker::ModuleState;
    using linker::ModuleTable;
    using linker::ModuleLinker;
}
