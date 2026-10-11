#pragma once

#include "eta/cst/bridge_sexpr.h"

namespace eta::format {
    enum class Outcome { Formatted, Passthrough, Refused };
    enum class Newline { Lf, Crlf };
    struct NewlinePolicy {
        Newline kind{Newline::Lf};
        bool final_newline{false};
        bool operator==(const NewlinePolicy &) const = default;
    };
    struct Options {
        std::size_t width{100};
        std::size_t indent{2};
        cst::ReaderConfig reader{};
        std::optional<NewlinePolicy> newline;
    };
    struct Coverage {
        std::string reason;
        cst::ByteSpan span;
    };
    struct Timings {
        double parse_ms{0};
        double layout_render_ms{0};
        double output_validation_ms{0};
    };
    struct FormatResult {
        Outcome outcome{Outcome::Refused};
        std::optional<std::string> output;
        Options effective_options;
        std::vector<Coverage> passthrough;
        std::vector<cst::AdaptedDiagnostic> diagnostics;
        int exit_class{0};
        std::size_t peak_owned_memory_bytes{0};
        Timings timings;
    };

    /// Always validates with the original compiler reader, concrete tokens and
    /// ordered comment/suppression associations before returning output.
    FormatResult format_source(std::string source, Options options = {}, cst::Operation operation = {});

    /// Shared production guard, exposed internally for deliberate-corruption tests.
    std::expected<void, cst::Failure> validate_output(const cst::File &original,
        std::string output, cst::Operation operation = {}, std::size_t *memory_peak = nullptr);
}
