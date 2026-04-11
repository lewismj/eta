#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "eta/reader/parser.h"

namespace eta::diagnostic {

using Span = eta::reader::parser::Span;

/**
 * @brief Unified diagnostic severity levels
 */
enum class Severity : std::uint8_t {
    Error,
    Warning,
    Note,
    Hint
};

constexpr const char* to_string(Severity s) noexcept {
    switch (s) {
        case Severity::Error:   return "error";
        case Severity::Warning: return "warning";
        case Severity::Note:    return "note";
        case Severity::Hint:    return "hint";
    }
    return "unknown";
}

/**
 * @brief Unified error code enumeration across all compiler phases
 */
enum class DiagnosticCode : std::uint16_t {
    // Lexer errors (0-99)
    UnterminatedString = 0,
    UnterminatedComment,
    InvalidChar,
    InvalidToken,
    InvalidNumeric,
    InvalidBoolean,
    InvalidCodePoint,
    MissingDelimiter,
    UnexpectedEOF,
    InvalidDatum,

    // Parser errors (100-199)
    UnexpectedToken = 100,
    UnmatchedParen,
    InvalidList,
    InvalidVector,
    InvalidDottedList,
    UnquoteOutsideQuasiquote,

    // Expander errors (200-299)
    InvalidSyntax = 200,
    DuplicateIdentifier,
    InvalidLetBindings,
    ReservedKeyword,
    ArityError,
    ExpansionDepthExceeded,

    // Linker errors (300-399)
    ModuleNotFound = 300,
    CircularDependency,
    DuplicateExport,
    ImportNotFound,
    UnresolvedImport,
    ExportOfUnknownName,
    DuplicateModule,

    // Semantic errors (400-499)
    UndefinedName = 400,
    DuplicateDefinition,
    NonFunctionCall,
    InvalidFormShape,
    ImmutableAssignment,
    SetOnImported,
    InvalidLetrecInit,
    ExportOfUnknownBinding,

    // Runtime errors (500-599)
    NotImplemented = 500,
    StackOverflow,
    FrameOverflow,
    InvalidInstruction,
    InvalidArity,
    TypeError,
    UndefinedGlobal,
    HeapAllocationFailed,
    InternTableFull,
    StackUnderflow,
    InternalError,
    UserError,
};

/**
 * @brief Get the phase name for a diagnostic code
 */
constexpr const char* phase_for_code(DiagnosticCode code) noexcept {
    auto n = static_cast<std::uint16_t>(code);
    if (n < 100) return "lexer";
    if (n < 200) return "parser";
    if (n < 300) return "expander";
    if (n < 400) return "linker";
    if (n < 500) return "semantic";
    return "runtime";
}

/**
 * @brief A single diagnostic message with optional related spans
 */
struct Diagnostic {
    DiagnosticCode code{};
    Severity severity{Severity::Error};
    Span span{};
    std::string message;

    // Optional secondary spans with labels (for multi-span errors)
    struct RelatedSpan {
        Span span;
        std::string label;
    };
    std::vector<RelatedSpan> related;

    // Builder methods for fluent construction
    Diagnostic& with_severity(Severity s) { severity = s; return *this; }
    Diagnostic& with_related(Span s, std::string label) {
        related.push_back({s, std::move(label)});
        return *this;
    }
};

/**
 * @brief Optional callback to resolve a numeric file_id to a human-readable
 *        filename.  When provided to formatting functions, the output will
 *        show the actual file name instead of "file N".
 *
 * Return an empty string to fall back to the numeric file_id display.
 */
using FileResolver = std::function<std::string(uint32_t file_id)>;

/**
 * @brief Format a span for display
 */
inline void write_span(std::ostream& os, const Span& sp,
                        const FileResolver& resolve_file = {}) {
    os << "[";
    if (resolve_file) {
        auto name = resolve_file(sp.file_id);
        if (!name.empty())
            os << name;
        else
            os << "file " << sp.file_id;
    } else {
        os << "file " << sp.file_id;
    }
    os << ":" << sp.start.line << ":" << sp.start.column
       << "-" << sp.end.line << ":" << sp.end.column << "]";
}

/**
 * @brief Format a diagnostic for display (with optional ANSI colors)
 */
inline void format_diagnostic(std::ostream& os, const Diagnostic& d,
                               bool use_color = false,
                               const FileResolver& resolve_file = {}) {
    const char* severity_color = "";
    const char* reset = "";

    if (use_color) {
        reset = "\033[0m";
        switch (d.severity) {
            case Severity::Error:   severity_color = "\033[1;31m"; break;
            case Severity::Warning: severity_color = "\033[1;33m"; break;
            case Severity::Note:    severity_color = "\033[1;36m"; break;
            case Severity::Hint:    severity_color = "\033[1;32m"; break;
        }
    }

    os << severity_color << to_string(d.severity) << reset << " ";
    write_span(os, d.span, resolve_file);
    os << ": " << d.message;

    for (const auto& rel : d.related) {
        os << "\n  ";
        write_span(os, rel.span, resolve_file);
        os << ": " << rel.label;
    }
}

inline std::ostream& operator<<(std::ostream& os, const Diagnostic& d) {
    format_diagnostic(os, d);
    return os;
}

/**
 * @brief Accumulates diagnostics during compilation
 */
class DiagnosticEngine {
public:
    void emit(Diagnostic d) {
        if (d.severity == Severity::Error) ++error_count_;
        diagnostics_.push_back(std::move(d));
    }

    void emit_error(DiagnosticCode code, Span span, std::string message) {
        emit(Diagnostic{code, Severity::Error, span, std::move(message)});
    }

    void emit_warning(DiagnosticCode code, Span span, std::string message) {
        emit(Diagnostic{code, Severity::Warning, span, std::move(message)});
    }

    [[nodiscard]] bool has_errors() const noexcept { return error_count_ > 0; }
    [[nodiscard]] std::size_t error_count() const noexcept { return error_count_; }
    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const noexcept { return diagnostics_; }

    void clear() {
        diagnostics_.clear();
        error_count_ = 0;
    }

    void print_all(std::ostream& os, bool use_color = false,
                   const FileResolver& resolve_file = {}) const {
        for (const auto& d : diagnostics_) {
            format_diagnostic(os, d, use_color, resolve_file);
            os << '\n';
        }
    }

private:
    std::vector<Diagnostic> diagnostics_;
    std::size_t error_count_{0};
};

/**
 * @brief Helper to create diagnostic from existing error types
 */
template<typename E>
Diagnostic to_diagnostic(const E& error);

/**
 * @brief Result type using Diagnostic for errors
 */
template<typename T>
using DiagResult = std::expected<T, Diagnostic>;

} // namespace eta::diagnostic

// ============================================================================
// Specializations: convert phase-specific errors into Diagnostic.
// Include this header and call to_diagnostic() on any supported error type.
// ============================================================================

#include "eta/reader/lexer.h"

namespace eta::diagnostic {

/**
 * @brief Convert LexError to Diagnostic
 */
template<>
inline Diagnostic to_diagnostic<eta::reader::lexer::LexError>(const eta::reader::lexer::LexError& e) {
    DiagnosticCode code{};
    switch (e.kind) {
        case eta::reader::lexer::LexErrorKind::UnterminatedString:
            code = DiagnosticCode::UnterminatedString;
            break;
        case eta::reader::lexer::LexErrorKind::UnterminatedComment:
            code = DiagnosticCode::UnterminatedComment;
            break;
        case eta::reader::lexer::LexErrorKind::InvalidChar:
            code = DiagnosticCode::InvalidChar;
            break;
        case eta::reader::lexer::LexErrorKind::InvalidToken:
            code = DiagnosticCode::InvalidToken;
            break;
        case eta::reader::lexer::LexErrorKind::InvalidNumeric:
            code = DiagnosticCode::InvalidNumeric;
            break;
        case eta::reader::lexer::LexErrorKind::InvalidBoolean:
            code = DiagnosticCode::InvalidBoolean;
            break;
        case eta::reader::lexer::LexErrorKind::InvalidCodePoint:
            code = DiagnosticCode::InvalidCodePoint;
            break;
        case eta::reader::lexer::LexErrorKind::MissingDelimiter:
            code = DiagnosticCode::MissingDelimiter;
            break;
        case eta::reader::lexer::LexErrorKind::UnexpectedEOF:
            code = DiagnosticCode::UnexpectedEOF;
            break;
        case eta::reader::lexer::LexErrorKind::InvalidUtf8:
            code = DiagnosticCode::InvalidToken;
            break;
        case eta::reader::lexer::LexErrorKind::InvalidDatum:
            code = DiagnosticCode::InvalidDatum;
            break;
        default:
            code = DiagnosticCode::InvalidToken;
            break;
    }

    return Diagnostic{code, Severity::Error, e.span, e.message};
}

} // namespace eta::diagnostic

// parser.h is already included at the top of this header.
namespace eta::diagnostic {

/**
 * @brief Convert ParseError to Diagnostic
 *
 * ParseError carries no message string, so a human-readable description
 * is synthesized from the error kind.
 */
template<>
inline Diagnostic to_diagnostic<eta::reader::parser::ParseError>(const eta::reader::parser::ParseError& e) {
    DiagnosticCode code{};
    const char* msg = "parse error";
    using PK = eta::reader::parser::ParseErrorKind;
    switch (e.kind) {
        case PK::FromLexer:
            code = DiagnosticCode::UnexpectedToken;
            msg = "error propagated from lexer";
            break;
        case PK::UnexpectedEOF:
            code = DiagnosticCode::UnexpectedToken;
            msg = "unexpected end of input";
            break;
        case PK::UnexpectedClosingDelimiter:
            code = DiagnosticCode::UnmatchedParen;
            msg = "unexpected closing delimiter";
            break;
        case PK::UnsupportedToken:
            code = DiagnosticCode::UnexpectedToken;
            msg = "unsupported token";
            break;
        case PK::UnclosedList:
            code = DiagnosticCode::InvalidList;
            msg = "unclosed list";
            break;
        case PK::MisplacedDot:
            code = DiagnosticCode::InvalidDottedList;
            msg = "misplaced dot in list";
            break;
        case PK::MultipleDotsInDottedList:
            code = DiagnosticCode::InvalidDottedList;
            msg = "multiple dots in dotted list";
            break;
        case PK::DotAtListStart:
            code = DiagnosticCode::InvalidDottedList;
            msg = "dot at start of list";
            break;
        case PK::UnclosedVector:
            code = DiagnosticCode::InvalidVector;
            msg = "unclosed vector";
            break;
        case PK::DotInVector:
            code = DiagnosticCode::InvalidVector;
            msg = "dot in vector literal";
            break;
        case PK::ByteVectorNonInteger:
            code = DiagnosticCode::InvalidVector;
            msg = "non-integer in byte vector";
            break;
        case PK::InvalidByteLiteral:
            code = DiagnosticCode::InvalidVector;
            msg = "invalid byte literal";
            break;
        case PK::UnquoteOutsideQuasiquote:
            code = DiagnosticCode::UnquoteOutsideQuasiquote;
            msg = "unquote outside quasiquote";
            break;
        case PK::InternalNotAnAtom:
            code = DiagnosticCode::UnexpectedToken;
            msg = "internal error: expected atom";
            break;
        case PK::InternalNotAReaderToken:
            code = DiagnosticCode::UnexpectedToken;
            msg = "internal error: expected reader token";
            break;
        default:
            code = DiagnosticCode::UnexpectedToken;
            break;
    }
    return Diagnostic{code, Severity::Error, e.span, msg};
}

} // namespace eta::diagnostic

#include "eta/reader/expander.h"

namespace eta::diagnostic {

/**
 * @brief Convert ExpandError to Diagnostic
 */
template<>
inline Diagnostic to_diagnostic<eta::reader::expander::ExpandError>(const eta::reader::expander::ExpandError& e) {
    DiagnosticCode code{};
    switch (e.kind) {
        case eta::reader::expander::ExpandError::Kind::InvalidSyntax:
            code = DiagnosticCode::InvalidSyntax;
            break;
        case eta::reader::expander::ExpandError::Kind::DuplicateIdentifier:
            code = DiagnosticCode::DuplicateIdentifier;
            break;
        case eta::reader::expander::ExpandError::Kind::InvalidLetBindings:
            code = DiagnosticCode::InvalidLetBindings;
            break;
        case eta::reader::expander::ExpandError::Kind::ReservedKeyword:
            code = DiagnosticCode::ReservedKeyword;
            break;
        case eta::reader::expander::ExpandError::Kind::ArityError:
            code = DiagnosticCode::ArityError;
            break;
        case eta::reader::expander::ExpandError::Kind::ExpansionDepthExceeded:
            code = DiagnosticCode::ExpansionDepthExceeded;
            break;
        default:
            code = DiagnosticCode::InvalidSyntax;
            break;
    }

    return Diagnostic{code, Severity::Error, e.span, e.message};
}

} // namespace eta::diagnostic

#include "eta/reader/module_linker.h"

namespace eta::diagnostic {

/**
 * @brief Convert LinkError to Diagnostic
 */
template<>
inline Diagnostic to_diagnostic<eta::reader::linker::LinkError>(const eta::reader::linker::LinkError& e) {
    DiagnosticCode code{};
    using K = eta::reader::linker::LinkError::Kind;
    switch (e.kind) {
        case K::UnknownModule:       code = DiagnosticCode::ModuleNotFound; break;
        case K::ExportOfUnknownName: code = DiagnosticCode::ExportOfUnknownName; break;
        case K::ConflictingImport:   code = DiagnosticCode::UnresolvedImport; break;
        case K::NameNotExported:     code = DiagnosticCode::ImportNotFound; break;
        case K::DuplicateModule:     code = DiagnosticCode::DuplicateModule; break;
        case K::CircularDependency:  code = DiagnosticCode::CircularDependency; break;
        default:                     code = DiagnosticCode::ModuleNotFound; break;
    }
    return Diagnostic{code, Severity::Error, e.span, e.message};
}

} // namespace eta::diagnostic

#include "eta/semantics/semantic_analyzer.h"

namespace eta::diagnostic {

/**
 * @brief Convert SemanticError to Diagnostic
 */
template<>
inline Diagnostic to_diagnostic<eta::semantics::SemanticError>(const eta::semantics::SemanticError& e) {
    DiagnosticCode code{};
    switch (e.kind) {
        case eta::semantics::SemanticError::Kind::UndefinedName:
            code = DiagnosticCode::UndefinedName;
            break;
        case eta::semantics::SemanticError::Kind::DuplicateDefinition:
            code = DiagnosticCode::DuplicateDefinition;
            break;
        case eta::semantics::SemanticError::Kind::NonFunctionCall:
            code = DiagnosticCode::NonFunctionCall;
            break;
        case eta::semantics::SemanticError::Kind::InvalidFormShape:
            code = DiagnosticCode::InvalidFormShape;
            break;
        case eta::semantics::SemanticError::Kind::ImmutableAssignment:
            code = DiagnosticCode::ImmutableAssignment;
            break;
        case eta::semantics::SemanticError::Kind::SetOnImported:
            code = DiagnosticCode::SetOnImported;
            break;
        case eta::semantics::SemanticError::Kind::InvalidLetrecInit:
            code = DiagnosticCode::InvalidLetrecInit;
            break;
        case eta::semantics::SemanticError::Kind::ExportOfUnknownBinding:
            code = DiagnosticCode::ExportOfUnknownBinding;
            break;
        default:
            code = DiagnosticCode::InvalidFormShape;
            break;
    }

    return Diagnostic{code, Severity::Error, e.span, e.message};
}

} // namespace eta::diagnostic

#include "eta/runtime/error.h"

namespace eta::diagnostic {

/**
 * @brief Convert VMError to Diagnostic
 */
template<>
inline Diagnostic to_diagnostic<eta::runtime::error::VMError>(const eta::runtime::error::VMError& e) {
    DiagnosticCode code{};
    using EC = eta::runtime::error::RuntimeErrorCode;
    switch (e.code) {
        case EC::NotImplemented:     code = DiagnosticCode::NotImplemented; break;
        case EC::InternalError:      code = DiagnosticCode::InternalError; break;
        case EC::StackOverflow:      code = DiagnosticCode::StackOverflow; break;
        case EC::StackUnderflow:     code = DiagnosticCode::StackUnderflow; break;
        case EC::FrameOverflow:      code = DiagnosticCode::FrameOverflow; break;
        case EC::InvalidInstruction: code = DiagnosticCode::InvalidInstruction; break;
        case EC::InvalidArity:       code = DiagnosticCode::InvalidArity; break;
        case EC::TypeError:          code = DiagnosticCode::TypeError; break;
        case EC::UndefinedGlobal:    code = DiagnosticCode::UndefinedGlobal; break;
        case EC::UserError:          code = DiagnosticCode::UserError; break;
        default:                     code = DiagnosticCode::NotImplemented; break;
    }
    return Diagnostic{code, Severity::Error, {}, e.message};
}

/**
 * @brief Convert NaNBoxError to Diagnostic
 */
template<>
inline Diagnostic to_diagnostic<eta::runtime::nanbox::NaNBoxError>(const eta::runtime::nanbox::NaNBoxError& e) {
    return Diagnostic{DiagnosticCode::TypeError, Severity::Error, {},
        std::string("NaNBox error: ") + eta::runtime::nanbox::to_string(e)};
}

/**
 * @brief Convert HeapError to Diagnostic
 */
template<>
inline Diagnostic to_diagnostic<eta::runtime::memory::heap::HeapError>(const eta::runtime::memory::heap::HeapError& e) {
    return Diagnostic{DiagnosticCode::HeapAllocationFailed, Severity::Error, {},
        std::string("heap error: ") + eta::runtime::memory::heap::to_string(e)};
}

/**
 * @brief Convert InternTableError to Diagnostic
 */
template<>
inline Diagnostic to_diagnostic<eta::runtime::memory::intern::InternTableError>(const eta::runtime::memory::intern::InternTableError& e) {
    return Diagnostic{DiagnosticCode::InternTableFull, Severity::Error, {},
        std::string("intern table error: ") + eta::runtime::memory::intern::to_string(e)};
}

} // namespace eta::diagnostic
