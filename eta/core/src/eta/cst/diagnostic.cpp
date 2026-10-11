#include "diagnostic.h"

namespace eta::cst {
    namespace {
        Location locate(reader::lexer::Span span, const DiagnosticContext &context) {
            Location location;
            if (!context.resolve) return location;
            const auto source = context.resolve(span.file_id);
            if (!source) return location;
            if (!source->identity().empty()) location.file = source->identity();
            const bool default_span = span.file_id == 0 && span.start.offset == 0 && span.end.offset == 0 &&
                                      span.start.line == 1 && span.start.column == 1 && span.end.line == 1 && span.end.
                                      column == 1;
            if ((!default_span || context.default_span_is_located) && span.start.offset <= span.end.offset && span.end.
                offset <= source->text().size())
                location.span = ByteSpan{
                    static_cast<std::size_t>(span.start.offset), static_cast<std::size_t>(span.end.offset)
                };
            return location;
        }

        template<class Error>
        AdaptedDiagnostic typed(const Error &error, const DiagnosticContext &context, std::string owner,
                                int exit_class) {
            AdaptedDiagnostic result;
            result.producer = context.producer;
            result.module = context.module;
            result.native_kind = to_string(error.kind);
            const auto separator = result.native_kind.rfind("::");
            const auto kind = result.native_kind.substr(separator == std::string::npos ? 0 : separator + 2);
            result.classified = kind != "Unknown";
            result.code = std::move(owner) + "." + kind;
            if (!result.classified) result.code += "." + std::to_string(static_cast<unsigned>(error.kind));
            result.location = locate(error.span, context);
            result.message = eta::diagnostic::to_diagnostic(error).message;
            result.exit_class = exit_class;
            return result;
        }
    }

    AdaptedDiagnostic adapt(const reader::lexer::LexError &error, const DiagnosticContext &context) {
        return typed(error, context, "reader.lexer", 2);
    }

    AdaptedDiagnostic adapt(const reader::parser::ParseError &error, const DiagnosticContext &context) {
        using Kind = reader::parser::ParseErrorKind;
        return typed(error, context, "reader.parser",
                     error.kind == Kind::InternalNotAnAtom || error.kind == Kind::InternalNotAReaderToken ? 70 : 2);
    }

    AdaptedDiagnostic adapt(const reader::expander::ExpandError &error, const DiagnosticContext &context) {
        return typed(error, context, "compiler.expander",
                     error.kind == reader::expander::ExpandError::Kind::ExpansionDepthExceeded ? 2 : 1);
    }

    AdaptedDiagnostic adapt(const reader::linker::LinkError &error, const DiagnosticContext &context) {
        return typed(error, context, "compiler.linker", 1);
    }

    AdaptedDiagnostic adapt(const eta::diagnostic::Diagnostic &diagnostic, const DiagnosticContext &context) {
        AdaptedDiagnostic result;
        result.producer = context.producer;
        result.module = context.module;
        const auto number = static_cast<unsigned>(diagnostic.code);
        result.code = "compiler.diagnostic." + std::to_string(number);
        result.native_kind = std::to_string(number);
        result.message = diagnostic.message;
        result.location = locate(diagnostic.span, context);
        result.severity = diagnostic.severity;
        for (const auto &related: diagnostic.related)
            result.related.push_back({locate(related.span, context), related.label});
        result.classified = number <= 9 || (number >= 100 && number <= 105) || (number >= 200 && number <= 205) ||
                            (number >= 300 && number <= 306) || (number >= 400 && number <= 407) || (
                                number >= 500 && number <= 511);
        using Code = eta::diagnostic::DiagnosticCode;
        if (diagnostic.code == Code::InternalError || diagnostic.code == Code::InvalidInstruction || diagnostic.code ==
            Code::StackUnderflow)
            result.exit_class = 70;
        else if (number < 200 || diagnostic.code == Code::ExpansionDepthExceeded || diagnostic.code ==
                 Code::StackOverflow ||
                 diagnostic.code == Code::FrameOverflow || diagnostic.code == Code::HeapAllocationFailed || diagnostic.
                 code == Code::InternTableFull)
            result.exit_class = 2;
        else result.exit_class = 1;
        if (!result.classified) result.exit_class = 1;
        return result;
    }

    AdaptedDiagnostic adapt_unclassified(std::string message, const DiagnosticContext &context, int exit_class) {
        AdaptedDiagnostic result;
        result.producer = context.producer;
        result.module = context.module;
        result.code = "compiler.unclassified";
        result.classified = false;
        result.message = std::move(message);
        result.exit_class = exit_class;
        return result;
    }

    bool same_diagnostic(const AdaptedDiagnostic &left, const AdaptedDiagnostic &right) {
        if (left.source != right.source || left.code != right.code || left.location.file != right.location.file ||
            left.location.span != right.location.span)
            return false;
        if (left.location.span) return true;
        return left.producer == right.producer && left.module == right.module && left.message == right.message;
    }
}
