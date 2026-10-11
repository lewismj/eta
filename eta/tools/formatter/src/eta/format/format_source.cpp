#include "format_source.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <set>

#include "doc.h"
#include "forms.h"
#include "layout.h"

namespace eta::format {
    namespace {
        using namespace cst;
        using Kind = reader::lexer::Token::Kind;

        std::string_view raw(const File &file, TokenId token) { return file.source->slice(file.tokens[token].span); }
        ByteSpan extent(const File &file, const Node &node) {
            return {file.tokens[node.token_begin].span.begin, file.tokens[node.token_end - 1].span.end};
        }

        std::expected<NewlinePolicy, Failure> newline_policy(std::string_view source, const Operation &operation) {
            bool lf = false, crlf = false;
            for (std::size_t i = 0; i < source.size(); ++i) {
                if ((i & 4095) == 0 && operation.cancelled && operation.cancelled->load(std::memory_order_relaxed))
                    return std::unexpected(Failure{"format.Cancelled", "formatting cancelled", 2});
                if (source[i] == '\r') {
                    if (i + 1 == source.size() || source[i + 1] != '\n')
                        return std::unexpected(Failure{"format.LineEndings", "bare CR line endings are unsupported", 2});
                    crlf = true;
                    ++i;
                } else if (source[i] == '\n') lf = true;
            }
            if (lf && crlf) return std::unexpected(Failure{"format.LineEndings", "mixed LF/CRLF line endings are unsupported", 2});
            return NewlinePolicy{crlf ? Newline::Crlf : Newline::Lf, source.ends_with('\n')};
        }

        void refuse(FormatResult &result, Failure failure, const std::string &identity) {
            result.outcome = Outcome::Refused;
            result.output.reset();
            result.exit_class = failure.exit_class;
            AdaptedDiagnostic diagnostic;
            diagnostic.source = "formatter";
            diagnostic.producer = "eta_format_lib";
            diagnostic.code = std::move(failure.code);
            diagnostic.message = std::move(failure.message);
            diagnostic.exit_class = failure.exit_class;
            if (!identity.empty()) diagnostic.location.file = identity;
            result.diagnostics.push_back(std::move(diagnostic));
        }
    }

    std::expected<void, cst::Failure> validate_output(const cst::File &original, std::string output, cst::Operation operation, std::size_t *memory_peak) {
        auto parsed = cst::parse(std::move(output), original.config, std::move(operation));
        if (!parsed) return std::unexpected(parsed.error());
        const auto mismatch = [] { return std::unexpected(cst::Failure{"format.Invariant", "formatter output failed independent preservation checks", 70}); };
        if (parsed->status != cst::Status::Valid || !cst::has_unique_byte_ownership(*parsed->file)) return mismatch();
        const auto &formatted = *parsed->file;
        if (!cst::equivalent_reader_trace(original, formatted) || original.associations.size() != formatted.associations.size()) return mismatch();
        for (std::size_t i = 0; i < original.associations.size(); ++i) {
            const auto &a = original.associations[i];
            const auto &b = formatted.associations[i];
            if (a.kind != b.kind || a.anchor != b.anchor || original.trivia[a.comment].kind != formatted.trivia[b.comment].kind
                || original.source->slice(original.trivia[a.comment].span) != formatted.source->slice(formatted.trivia[b.comment].span)) return mismatch();
            auto owner = [](const cst::File &file, cst::TriviaId comment) {
                return std::lower_bound(file.tokens.begin(), file.tokens.end(), comment,
                    [](const cst::Token &token, cst::TriviaId id) { return token.trivia_end <= id; })->id;
            };
            const auto left = owner(original, a.comment), right = owner(formatted, b.comment);
            if (left != right || original.trace[left].path != formatted.trace[right].path) return mismatch();
        }
        auto before = cst::read_original(original, true);
        auto after = cst::read_original(formatted, true);
        if (!before) {
            if (const auto *failure = std::get_if<cst::Failure>(&before.error())) return std::unexpected(*failure);
            return mismatch();
        }
        if (!after) {
            if (const auto *failure = std::get_if<cst::Failure>(&after.error())) return std::unexpected(*failure);
            return mismatch();
        }
        if (before->size() != after->size()) return mismatch();
        for (std::size_t i = 0; i < before->size(); ++i)
            if (!cst::equivalent_sexpr(*(*before)[i], *(*after)[i])) return mismatch();
        if (memory_peak) *memory_peak = formatted.peak_memory_bytes();
        return {};
    }

    FormatResult format_source(std::string source, Options options, cst::Operation operation) {
        using Clock = std::chrono::steady_clock;
        const auto started = Clock::now();
        FormatResult result;
        result.effective_options = options;
        try {
            if (!options.width || !options.indent) {
                refuse(result, {"format.Options", "width and indentation must be positive", 64}, operation.identity);
                return result;
            }
            if (source.size() > operation.limits.input_bytes) {
                refuse(result, {"format.InputLimit", "input byte limit exceeded", 2}, operation.identity);
                return result;
            }
            auto policy = newline_policy(source, operation);
            if (!policy) { refuse(result, policy.error(), operation.identity); return result; }
            if (options.newline && (options.newline->final_newline != policy->final_newline
                || (source.find('\n') != std::string::npos && options.newline->kind != policy->kind))) {
                refuse(result, {"format.LineEndings", "captured newline policy differs from the input", 2}, operation.identity);
                return result;
            }
            if (options.newline) policy->kind = options.newline->kind;
            options.newline = *policy;
            result.effective_options = options;
            auto parsed = cst::parse(std::move(source), options.reader, operation);
            const auto parsed_at = Clock::now();
            result.timings.parse_ms = std::chrono::duration<double, std::milli>(parsed_at - started).count();
            if (!parsed) { refuse(result, parsed.error(), operation.identity); return result; }
            if (parsed->status != cst::Status::Valid) {
                result.diagnostics = std::move(parsed->diagnostics);
                refuse(result, {parsed->status == cst::Status::Recovered ? "format.Malformed" : "format.Unsupported",
                    parsed->status == cst::Status::Recovered ? "input fails the selected compiler reader" : "valid input is unsupported for formatting", 2}, operation.identity);
                return result;
            }
            const auto &file = *parsed->file;
            auto lossless = cst::serialize_losslessly(file);
            if (!lossless) { refuse(result, lossless.error(), operation.identity); return result; }
            if (*lossless != file.source->text()) {
                refuse(result, {"format.SourceFidelity", "lossless CST serialization differs from input", 70}, operation.identity);
                return result;
            }
            lossless = std::string{};
            if (operation.limits.output_bytes > operation.limits.memory_bytes / 2) {
                refuse(result, {"format.MemoryLimit", "output reservations exceed the memory limit", 2}, operation.identity);
                return result;
            }
            file.account_external_bytes(operation.limits.output_bytes * 2);
            std::expected<std::string, Failure> output;
            {
                Layout layout(file, options, result.passthrough);
                output = layout.run();
            }
            if (!output) { refuse(result, output.error(), operation.identity); return result; }
            while (output->ends_with('\n') || output->ends_with('\r')) output->pop_back();
            if (policy->final_newline) output->append(policy->kind == Newline::Crlf ? "\r\n" : "\n");
            if (output->size() > operation.limits.output_bytes) {
                refuse(result, {"format.OutputLimit", "formatted output limit exceeded", 2}, operation.identity);
                return result;
            }
            auto validation_operation = operation;
            const auto reserved = file.peak_memory_bytes() + output->capacity() + 1;
            if (reserved >= operation.limits.memory_bytes) {
                refuse(result, {"format.MemoryLimit", "insufficient memory for independent output validation", 2}, operation.identity);
                return result;
            }
            validation_operation.limits.memory_bytes -= reserved;
            std::size_t validation_peak = 0;
            const auto validating_at = Clock::now();
            result.timings.layout_render_ms = std::chrono::duration<double, std::milli>(validating_at - parsed_at).count();
            auto valid = validate_output(file, *output, validation_operation, &validation_peak);
            result.timings.output_validation_ms = std::chrono::duration<double, std::milli>(Clock::now() - validating_at).count();
            if (!valid) { refuse(result, valid.error(), operation.identity); return result; }
            result.output = std::move(*output);
            result.peak_owned_memory_bytes = reserved + validation_peak;
            result.outcome = result.passthrough.empty() ? Outcome::Formatted : Outcome::Passthrough;
            return result;
        } catch (const cst::detail::OperationStopped &stop) {
            refuse(result, stop.failure, operation.identity);
        } catch (const std::bad_alloc &) {
            refuse(result, {"format.MemoryLimit", "formatter allocation failed", 2}, operation.identity);
        } catch (const std::exception &error) {
            refuse(result, {"format.Internal", error.what(), 70}, operation.identity);
        }
        return result;
    }
}
