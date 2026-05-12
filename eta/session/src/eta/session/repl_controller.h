/**
 * @file repl_controller.h
 * @brief REPL and notebook presentation controller for session front-ends.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "eta/interpreter/repl_wrap.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/value_formatter.h"
#include "eta/session/eval_display.h"

namespace eta::session {

class DisplayClassifier;

/**
 * @brief Stateful REPL controller built on top of a runtime host interface.
 */
class ReplController {
public:
    /**
     * @brief Runtime operations consumed by ReplController.
     */
    class ReplRuntime {
    public:
        virtual ~ReplRuntime() = default;

        [[nodiscard]] virtual const std::unordered_map<uint32_t, std::string>& global_names() const noexcept = 0;
        [[nodiscard]] virtual std::vector<std::string> discover_module_names() const = 0;
        [[nodiscard]] virtual bool has_module(const std::string& name) const = 0;
        [[nodiscard]] virtual std::string diagnostics_to_string() const = 0;
        [[nodiscard]] virtual std::string format_value(
            runtime::nanbox::LispVal value,
            runtime::FormatMode mode = runtime::FormatMode::Write) = 0;

        virtual bool run_source(std::string_view source,
                                runtime::nanbox::LispVal* result = nullptr,
                                const std::string& result_binding = {}) = 0;
    };

    /**
     * @brief Completion payload for front-ends.
     */
    struct CompletionResult {
        std::vector<std::string> matches;
        std::size_t cursor_start{0};
        std::size_t cursor_end{0};
    };

    ReplController(ReplRuntime& runtime, const DisplayClassifier& display_classifier) noexcept
        : runtime_(runtime),
          display_classifier_(display_classifier) {}

    /**
     * @brief Evaluate REPL input and return a formatted output string.
     */
    bool eval_string(std::string source, std::string& out);

    /**
     * @brief Evaluate source and return a structured display value.
     */
    [[nodiscard]] DisplayValue eval_to_display(const std::string& source);

    /**
     * @brief Collect completion matches at @p cursor_pos in @p source.
     */
    [[nodiscard]] CompletionResult completions_at(
        const std::string& source,
        std::size_t cursor_pos) const;

    /**
     * @brief Return Markdown hover text for a symbol.
     */
    [[nodiscard]] std::string hover_at(const std::string& symbol) const;

    /**
     * @brief Remove a synthesized REPL module from replay/import state.
     */
    void forget_module(std::string_view module_name);

private:
    ReplRuntime& runtime_;
    const DisplayClassifier& display_classifier_;
    int repl_counter_{0};
    std::vector<interpreter::PriorModule> repl_modules_;
};

} // namespace eta::session
