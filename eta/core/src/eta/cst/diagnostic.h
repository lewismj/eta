#pragma once

#include <memory>

#include "source.h"
#include "eta/diagnostic/diagnostic.h"

namespace eta::cst {
    struct Location {
        std::optional<std::string> file;
        std::optional<ByteSpan> span;
    };

    struct RelatedLocation {
        Location location;
        std::string message;
    };

    struct AdaptedDiagnostic {
        std::string source{"compiler"};
        std::string producer;
        std::string code;
        std::string native_kind;
        std::string module;
        Location location;
        eta::diagnostic::Severity severity{eta::diagnostic::Severity::Error};
        std::string message;
        std::vector<RelatedLocation> related;
        int exit_class{1};
        bool classified{true};
    };

    struct DiagnosticContext {
        std::string producer;
        std::string module;
        std::function<std::shared_ptr<const Source>(reader::lexer::FileId)> resolve;
        /// The producer can distinguish a real zero-width location from Span{}.
        bool default_span_is_located{false};
    };

    AdaptedDiagnostic adapt(const reader::lexer::LexError &error, const DiagnosticContext &context);

    AdaptedDiagnostic adapt(const reader::parser::ParseError &error, const DiagnosticContext &context);

    AdaptedDiagnostic adapt(const reader::expander::ExpandError &error, const DiagnosticContext &context);

    AdaptedDiagnostic adapt(const reader::linker::LinkError &error, const DiagnosticContext &context);

    AdaptedDiagnostic adapt(const eta::diagnostic::Diagnostic &diagnostic, const DiagnosticContext &context);

    AdaptedDiagnostic adapt_unclassified(std::string message, const DiagnosticContext &context, int exit_class = 1);

    bool same_diagnostic(const AdaptedDiagnostic &left, const AdaptedDiagnostic &right);
}
