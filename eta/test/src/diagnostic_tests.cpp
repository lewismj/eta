#include <boost/test/unit_test.hpp>
#include <sstream>
#include "eta/diagnostic/diagnostic.h"

using namespace eta::diagnostic;

BOOST_AUTO_TEST_SUITE(diagnostic_tests)

// ============================================================================
// Severity to_string
// ============================================================================

BOOST_AUTO_TEST_CASE(severity_to_string_error) {
    BOOST_CHECK_EQUAL(std::string(to_string(Severity::Error)), "error");
}

BOOST_AUTO_TEST_CASE(severity_to_string_warning) {
    BOOST_CHECK_EQUAL(std::string(to_string(Severity::Warning)), "warning");
}

BOOST_AUTO_TEST_CASE(severity_to_string_note) {
    BOOST_CHECK_EQUAL(std::string(to_string(Severity::Note)), "note");
}

BOOST_AUTO_TEST_CASE(severity_to_string_hint) {
    BOOST_CHECK_EQUAL(std::string(to_string(Severity::Hint)), "hint");
}

// ============================================================================
// phase_for_code
// ============================================================================

BOOST_AUTO_TEST_CASE(phase_lexer_codes) {
    BOOST_CHECK_EQUAL(std::string(phase_for_code(DiagnosticCode::UnterminatedString)), "lexer");
    BOOST_CHECK_EQUAL(std::string(phase_for_code(DiagnosticCode::InvalidChar)), "lexer");
    BOOST_CHECK_EQUAL(std::string(phase_for_code(DiagnosticCode::UnexpectedEOF)), "lexer");
}

BOOST_AUTO_TEST_CASE(phase_parser_codes) {
    BOOST_CHECK_EQUAL(std::string(phase_for_code(DiagnosticCode::UnexpectedToken)), "parser");
    BOOST_CHECK_EQUAL(std::string(phase_for_code(DiagnosticCode::UnmatchedParen)), "parser");
}

BOOST_AUTO_TEST_CASE(phase_expander_codes) {
    BOOST_CHECK_EQUAL(std::string(phase_for_code(DiagnosticCode::InvalidSyntax)), "expander");
    BOOST_CHECK_EQUAL(std::string(phase_for_code(DiagnosticCode::ReservedKeyword)), "expander");
    BOOST_CHECK_EQUAL(std::string(phase_for_code(DiagnosticCode::ExpansionDepthExceeded)), "expander");
}

BOOST_AUTO_TEST_CASE(phase_linker_codes) {
    BOOST_CHECK_EQUAL(std::string(phase_for_code(DiagnosticCode::ModuleNotFound)), "linker");
    BOOST_CHECK_EQUAL(std::string(phase_for_code(DiagnosticCode::CircularDependency)), "linker");
}

BOOST_AUTO_TEST_CASE(phase_semantic_codes) {
    BOOST_CHECK_EQUAL(std::string(phase_for_code(DiagnosticCode::UndefinedName)), "semantic");
    BOOST_CHECK_EQUAL(std::string(phase_for_code(DiagnosticCode::DuplicateDefinition)), "semantic");
}

BOOST_AUTO_TEST_CASE(phase_runtime_codes) {
    BOOST_CHECK_EQUAL(std::string(phase_for_code(DiagnosticCode::StackOverflow)), "runtime");
    BOOST_CHECK_EQUAL(std::string(phase_for_code(DiagnosticCode::TypeError)), "runtime");
}

// ============================================================================
// Diagnostic construction and builder
// ============================================================================

BOOST_AUTO_TEST_CASE(diagnostic_default_construction) {
    Diagnostic d;
    d.code = DiagnosticCode::InvalidSyntax;
    d.message = "test error";
    BOOST_CHECK(d.severity == Severity::Error);
    BOOST_CHECK(d.related.empty());
}

BOOST_AUTO_TEST_CASE(diagnostic_with_severity) {
    Diagnostic d;
    d.code = DiagnosticCode::InvalidSyntax;
    d.message = "test";
    d.with_severity(Severity::Warning);
    BOOST_CHECK(d.severity == Severity::Warning);
}

BOOST_AUTO_TEST_CASE(diagnostic_with_related) {
    Diagnostic d;
    d.code = DiagnosticCode::UndefinedName;
    d.message = "undefined 'x'";
    Span rel_span{};
    d.with_related(rel_span, "defined here");
    BOOST_CHECK_EQUAL(d.related.size(), 1u);
    BOOST_CHECK_EQUAL(d.related[0].label, "defined here");
}

BOOST_AUTO_TEST_CASE(diagnostic_builder_chaining) {
    Diagnostic d;
    d.code = DiagnosticCode::InvalidSyntax;
    d.message = "test";
    Span s1{}, s2{};
    d.with_severity(Severity::Note)
     .with_related(s1, "first")
     .with_related(s2, "second");
    BOOST_CHECK(d.severity == Severity::Note);
    BOOST_CHECK_EQUAL(d.related.size(), 2u);
}

// ============================================================================
// DiagnosticEngine
// ============================================================================

BOOST_AUTO_TEST_CASE(engine_empty) {
    DiagnosticEngine eng;
    BOOST_CHECK(!eng.has_errors());
    BOOST_CHECK_EQUAL(eng.error_count(), 0u);
    BOOST_CHECK(eng.diagnostics().empty());
}

BOOST_AUTO_TEST_CASE(engine_emit_error) {
    DiagnosticEngine eng;
    eng.emit_error(DiagnosticCode::InvalidSyntax, Span{}, "bad syntax");
    BOOST_CHECK(eng.has_errors());
    BOOST_CHECK_EQUAL(eng.error_count(), 1u);
    BOOST_CHECK_EQUAL(eng.diagnostics().size(), 1u);
    BOOST_CHECK_EQUAL(eng.diagnostics()[0].message, "bad syntax");
}

BOOST_AUTO_TEST_CASE(engine_emit_warning_not_error) {
    DiagnosticEngine eng;
    eng.emit_warning(DiagnosticCode::InvalidSyntax, Span{}, "suspicious");
    BOOST_CHECK(!eng.has_errors());
    BOOST_CHECK_EQUAL(eng.error_count(), 0u);
    BOOST_CHECK_EQUAL(eng.diagnostics().size(), 1u);
}

BOOST_AUTO_TEST_CASE(engine_multiple_diagnostics) {
    DiagnosticEngine eng;
    eng.emit_error(DiagnosticCode::InvalidSyntax, Span{}, "err1");
    eng.emit_warning(DiagnosticCode::InvalidSyntax, Span{}, "warn1");
    eng.emit_error(DiagnosticCode::TypeError, Span{}, "err2");
    BOOST_CHECK_EQUAL(eng.error_count(), 2u);
    BOOST_CHECK_EQUAL(eng.diagnostics().size(), 3u);
}

BOOST_AUTO_TEST_CASE(engine_clear) {
    DiagnosticEngine eng;
    eng.emit_error(DiagnosticCode::InvalidSyntax, Span{}, "err");
    BOOST_CHECK(eng.has_errors());
    eng.clear();
    BOOST_CHECK(!eng.has_errors());
    BOOST_CHECK_EQUAL(eng.error_count(), 0u);
    BOOST_CHECK(eng.diagnostics().empty());
}

BOOST_AUTO_TEST_CASE(engine_emit_raw_diagnostic) {
    DiagnosticEngine eng;
    Diagnostic d;
    d.code = DiagnosticCode::StackOverflow;
    d.severity = Severity::Error;
    d.message = "stack overflow";
    eng.emit(std::move(d));
    BOOST_CHECK(eng.has_errors());
    BOOST_CHECK_EQUAL(eng.diagnostics()[0].message, "stack overflow");
}

BOOST_AUTO_TEST_CASE(engine_emit_non_error_severity) {
    DiagnosticEngine eng;
    Diagnostic d;
    d.code = DiagnosticCode::InvalidSyntax;
    d.severity = Severity::Note;
    d.message = "note";
    eng.emit(std::move(d));
    BOOST_CHECK(!eng.has_errors());
    BOOST_CHECK_EQUAL(eng.diagnostics().size(), 1u);
}

// ============================================================================
// format_diagnostic
// ============================================================================

BOOST_AUTO_TEST_CASE(format_diagnostic_plain) {
    Diagnostic d;
    d.code = DiagnosticCode::InvalidSyntax;
    d.severity = Severity::Error;
    d.span = Span{0, {0, 1, 0}, {5, 1, 5}};
    d.message = "bad syntax";

    std::ostringstream os;
    format_diagnostic(os, d, /*use_color=*/false);
    std::string out = os.str();
    BOOST_CHECK(out.find("error") != std::string::npos);
    BOOST_CHECK(out.find("bad syntax") != std::string::npos);
    BOOST_CHECK(out.find("[file 0:") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(format_diagnostic_with_related) {
    Diagnostic d;
    d.code = DiagnosticCode::UndefinedName;
    d.severity = Severity::Error;
    d.message = "undefined 'x'";
    d.with_related(Span{0, {0, 5, 2}, {0, 5, 3}}, "defined here");

    std::ostringstream os;
    format_diagnostic(os, d, false);
    std::string out = os.str();
    BOOST_CHECK(out.find("defined here") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(format_diagnostic_with_color) {
    Diagnostic d;
    d.code = DiagnosticCode::InvalidSyntax;
    d.severity = Severity::Error;
    d.message = "test";

    std::ostringstream os;
    format_diagnostic(os, d, /*use_color=*/true);
    std::string out = os.str();
    // Should contain ANSI escape codes
    BOOST_CHECK(out.find("\033[1;31m") != std::string::npos);  // red for error
    BOOST_CHECK(out.find("\033[0m") != std::string::npos);     // reset
}

BOOST_AUTO_TEST_CASE(format_diagnostic_warning_color) {
    Diagnostic d;
    d.code = DiagnosticCode::InvalidSyntax;
    d.severity = Severity::Warning;
    d.message = "test";

    std::ostringstream os;
    format_diagnostic(os, d, true);
    std::string out = os.str();
    BOOST_CHECK(out.find("\033[1;33m") != std::string::npos);  // yellow for warning
}

BOOST_AUTO_TEST_CASE(format_diagnostic_note_color) {
    Diagnostic d;
    d.code = DiagnosticCode::InvalidSyntax;
    d.severity = Severity::Note;
    d.message = "info";

    std::ostringstream os;
    format_diagnostic(os, d, true);
    std::string out = os.str();
    BOOST_CHECK(out.find("\033[1;36m") != std::string::npos);  // cyan for note
}

BOOST_AUTO_TEST_CASE(format_diagnostic_hint_color) {
    Diagnostic d;
    d.code = DiagnosticCode::InvalidSyntax;
    d.severity = Severity::Hint;
    d.message = "hint";

    std::ostringstream os;
    format_diagnostic(os, d, true);
    std::string out = os.str();
    BOOST_CHECK(out.find("\033[1;32m") != std::string::npos);  // green for hint
}

// ============================================================================
// operator<< for Diagnostic
// ============================================================================

BOOST_AUTO_TEST_CASE(diagnostic_ostream_operator) {
    Diagnostic d;
    d.code = DiagnosticCode::TypeError;
    d.severity = Severity::Error;
    d.message = "type mismatch";

    std::ostringstream os;
    os << d;
    std::string out = os.str();
    BOOST_CHECK(out.find("error") != std::string::npos);
    BOOST_CHECK(out.find("type mismatch") != std::string::npos);
}

// ============================================================================
// print_all
// ============================================================================

BOOST_AUTO_TEST_CASE(engine_print_all) {
    DiagnosticEngine eng;
    eng.emit_error(DiagnosticCode::InvalidSyntax, Span{}, "err1");
    eng.emit_warning(DiagnosticCode::InvalidSyntax, Span{}, "warn1");

    std::ostringstream os;
    eng.print_all(os);
    std::string out = os.str();
    BOOST_CHECK(out.find("err1") != std::string::npos);
    BOOST_CHECK(out.find("warn1") != std::string::npos);
}

// ============================================================================
// to_diagnostic<LexError>
// ============================================================================

BOOST_AUTO_TEST_CASE(to_diagnostic_lex_error_unterminated_string) {
    eta::reader::lexer::LexError e(
        eta::reader::lexer::LexErrorKind::UnterminatedString,
        Span{0, {0, 1, 0}, {10, 1, 10}},
        "unterminated string literal");

    auto d = to_diagnostic(e);
    BOOST_CHECK(d.code == DiagnosticCode::UnterminatedString);
    BOOST_CHECK(d.severity == Severity::Error);
    BOOST_CHECK_EQUAL(d.message, "unterminated string literal");
}

BOOST_AUTO_TEST_CASE(to_diagnostic_lex_error_invalid_char) {
    eta::reader::lexer::LexError e(
        eta::reader::lexer::LexErrorKind::InvalidChar,
        Span{}, "invalid character");

    auto d = to_diagnostic(e);
    BOOST_CHECK(d.code == DiagnosticCode::InvalidChar);
}

BOOST_AUTO_TEST_CASE(to_diagnostic_lex_error_all_kinds) {
    using LK = eta::reader::lexer::LexErrorKind;
    struct KindMapping { LK lex; DiagnosticCode diag; };
    std::vector<KindMapping> mappings = {
        {LK::UnterminatedString, DiagnosticCode::UnterminatedString},
        {LK::UnterminatedComment, DiagnosticCode::UnterminatedComment},
        {LK::InvalidChar, DiagnosticCode::InvalidChar},
        {LK::InvalidToken, DiagnosticCode::InvalidToken},
        {LK::InvalidNumeric, DiagnosticCode::InvalidNumeric},
        {LK::InvalidBoolean, DiagnosticCode::InvalidBoolean},
        {LK::InvalidCodePoint, DiagnosticCode::InvalidCodePoint},
        {LK::MissingDelimiter, DiagnosticCode::MissingDelimiter},
        {LK::UnexpectedEOF, DiagnosticCode::UnexpectedEOF},
        {LK::InvalidDatum, DiagnosticCode::InvalidDatum},
    };
    for (const auto& [lex_kind, diag_code] : mappings) {
        eta::reader::lexer::LexError e(lex_kind, Span{});
        auto d = to_diagnostic(e);
        BOOST_CHECK(d.code == diag_code);
    }
}

// ============================================================================
// to_diagnostic<ExpandError>
// ============================================================================

BOOST_AUTO_TEST_CASE(to_diagnostic_expand_error) {
    using EK = eta::reader::expander::ExpandError::Kind;
    struct KindMapping { EK exp; DiagnosticCode diag; };
    std::vector<KindMapping> mappings = {
        {EK::InvalidSyntax, DiagnosticCode::InvalidSyntax},
        {EK::DuplicateIdentifier, DiagnosticCode::DuplicateIdentifier},
        {EK::InvalidLetBindings, DiagnosticCode::InvalidLetBindings},
        {EK::ReservedKeyword, DiagnosticCode::ReservedKeyword},
        {EK::ArityError, DiagnosticCode::ArityError},
        {EK::ExpansionDepthExceeded, DiagnosticCode::ExpansionDepthExceeded},
    };
    for (const auto& [exp_kind, diag_code] : mappings) {
        eta::reader::expander::ExpandError e;
        e.kind = exp_kind;
        e.message = "test";
        auto d = to_diagnostic(e);
        BOOST_CHECK(d.code == diag_code);
        BOOST_CHECK(d.severity == Severity::Error);
    }
}

// ============================================================================
// to_diagnostic<SemanticError>
// ============================================================================

BOOST_AUTO_TEST_CASE(to_diagnostic_semantic_error) {
    using SK = eta::semantics::SemanticError::Kind;
    struct KindMapping { SK sem; DiagnosticCode diag; };
    std::vector<KindMapping> mappings = {
        {SK::UndefinedName, DiagnosticCode::UndefinedName},
        {SK::DuplicateDefinition, DiagnosticCode::DuplicateDefinition},
        {SK::NonFunctionCall, DiagnosticCode::NonFunctionCall},
        {SK::InvalidFormShape, DiagnosticCode::InvalidFormShape},
        {SK::ImmutableAssignment, DiagnosticCode::ImmutableAssignment},
        {SK::SetOnImported, DiagnosticCode::SetOnImported},
        {SK::InvalidLetrecInit, DiagnosticCode::InvalidLetrecInit},
        {SK::ExportOfUnknownBinding, DiagnosticCode::ExportOfUnknownBinding},
    };
    for (const auto& [sem_kind, diag_code] : mappings) {
        eta::semantics::SemanticError e;
        e.kind = sem_kind;
        e.message = "test";
        auto d = to_diagnostic(e);
        BOOST_CHECK(d.code == diag_code);
        BOOST_CHECK(d.severity == Severity::Error);
    }
}

// ============================================================================
// write_span
// ============================================================================

BOOST_AUTO_TEST_CASE(write_span_format) {
    Span sp{42, {0, 10, 5}, {15, 10, 15}};
    std::ostringstream os;
    write_span(os, sp);
    std::string out = os.str();
    BOOST_CHECK_EQUAL(out, "[file 42:10:5-10:15]");
}

BOOST_AUTO_TEST_SUITE_END()





