/**
 * @file eval_engine.h
 * @brief Runtime `eval` builtin implementation for eta session runtimes.
 */

#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "eta/runtime/error.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/value_formatter.h"
#include "eta/session/compilation_session.h"

namespace eta::runtime {

class BuiltinEnvironment;

namespace memory::heap {
class Heap;
}

namespace vm {
class VM;
}

} // namespace eta::runtime

namespace eta::session {

/**
 * @brief Owns the runtime `eval` builtin behavior and eval compilation state.
 */
class EvalEngine {
public:
    /**
     * @brief Captured lexical binding used when evaluating an expression.
     */
    struct EvalBinding {
        std::string name;
        runtime::nanbox::LispVal value{runtime::nanbox::Nil};
    };

    /**
     * @brief Runtime host operations required by EvalEngine.
     */
    class Host {
    public:
        virtual ~Host() = default;

        [[nodiscard]] virtual runtime::memory::heap::Heap& heap() noexcept = 0;
        [[nodiscard]] virtual std::string format_value(
            runtime::nanbox::LispVal value,
            runtime::FormatMode mode) = 0;
        [[nodiscard]] virtual std::string diagnostics_to_string() const = 0;
    };

    EvalEngine(Host& host,
               CompilationSession& compilation,
               CompilationSession::Host& compilation_host,
               runtime::vm::VM& vm) noexcept
        : host_(host),
          compilation_(compilation),
          compilation_host_(compilation_host),
          vm_(vm) {}

    void install_builtin(runtime::BuiltinEnvironment& builtins);

    [[nodiscard]] static bool is_synthetic_eval_binding_name(std::string_view name);

private:
    std::expected<runtime::nanbox::LispVal, runtime::error::RuntimeError> eval_builtin(
        std::span<const runtime::nanbox::LispVal> args);

    std::expected<runtime::nanbox::LispVal, runtime::error::RuntimeError>
    compile_eval_lambda(std::string_view expr_source,
                        std::span<const EvalBinding> lexical_bindings);

    std::expected<runtime::nanbox::LispVal, runtime::error::RuntimeError>
    invoke_eval_lambda(runtime::nanbox::LispVal closure,
                       std::span<const EvalBinding> lexical_bindings);

    [[nodiscard]] std::vector<EvalBinding> collect_eval_lexical_bindings() const;

    Host& host_;
    CompilationSession& compilation_;
    CompilationSession::Host& compilation_host_;
    runtime::vm::VM& vm_;
    std::uint64_t eval_counter_{0};
};

} // namespace eta::session
