/**
 * @file eval_engine.cpp
 * @brief Runtime `eval` builtin implementation for eta session runtimes.
 */

#include "eta/session/eval_engine.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>
#include <utility>

#include "eta/runtime/builtin_env.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/vm/vm.h"

namespace eta::session {

void EvalEngine::install_builtin(runtime::BuiltinEnvironment& builtins) {
    builtins.overwrite_func("eval",
        [this](std::span<const runtime::nanbox::LispVal> args)
            -> std::expected<runtime::nanbox::LispVal, runtime::error::RuntimeError>
        {
            return eval_builtin(args);
        });
}

bool EvalEngine::is_synthetic_eval_binding_name(std::string_view name) {
    if (name.empty() || name.size() < 2u) return false;
    const char prefix = name.front();
    if (prefix != '%' && prefix != '&') return false;
    return std::all_of(name.begin() + 1, name.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0;
    });
}

std::expected<runtime::nanbox::LispVal, runtime::error::RuntimeError>
EvalEngine::eval_builtin(std::span<const runtime::nanbox::LispVal> args) {
    using runtime::error::RuntimeError;
    using runtime::error::RuntimeErrorCode;
    using runtime::error::VMError;
    using runtime::memory::heap::ObjectKind;
    using runtime::nanbox::LispVal;
    using runtime::nanbox::Tag;

    if (args.size() != 1u) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::InvalidArity,
            "eval: expected 1 argument"}});
    }

    const LispVal expr = args[0];
    bool should_eval = false;
    if (runtime::nanbox::ops::is_boxed(expr)) {
        const auto tag = runtime::nanbox::ops::tag(expr);
        if (tag == Tag::Symbol) {
            should_eval = true;
        } else if (tag == Tag::HeapObject) {
            const auto id = runtime::nanbox::ops::payload(expr);
            should_eval = (host_.heap().try_get_as<ObjectKind::Cons, void>(id) != nullptr);
        }
    }
    if (!should_eval) return expr;

    auto lexical_bindings = collect_eval_lexical_bindings();
    const std::string expr_source = host_.format_value(expr, runtime::FormatMode::Write);

    auto closure_res = [&]() -> std::expected<LispVal, RuntimeError> {
        runtime::vm::VM::ExecutionScope scope(vm_);
        return compile_eval_lambda(expr_source, lexical_bindings);
    }();
    if (!closure_res) {
        return std::unexpected(closure_res.error());
    }
    return invoke_eval_lambda(*closure_res, lexical_bindings);
}

std::expected<runtime::nanbox::LispVal, runtime::error::RuntimeError>
EvalEngine::compile_eval_lambda(std::string_view expr_source,
                                std::span<const EvalBinding> lexical_bindings) {
    const std::uint64_t eval_id = eval_counter_++;
    const std::string module_name = "__eta.eval." + std::to_string(eval_id);
    const std::string fn_name = "__eta_eval_fn_" + std::to_string(eval_id);
    auto active_module_guard = compilation_.make_active_module_execution_guard();
    (void)active_module_guard;

    std::ostringstream source;
    source << "(module " << module_name << "\n"
           << "  (define " << fn_name << "\n"
           << "    (lambda (";
    for (std::size_t i = 0; i < lexical_bindings.size(); ++i) {
        if (i > 0u) source << ' ';
        source << lexical_bindings[i].name;
    }
    source << ")\n"
           << "      " << expr_source << ")))";

    runtime::nanbox::LispVal closure = runtime::nanbox::Nil;
    if (!compilation_.run_source_impl(
            compilation_host_,
            source.str(),
            /*file_id=*/0u,
            &closure,
            fn_name)) {
        return std::unexpected(runtime::error::RuntimeError{runtime::error::VMError{
            runtime::error::RuntimeErrorCode::UserError,
            host_.diagnostics_to_string()}});
    }
    return closure;
}

std::expected<runtime::nanbox::LispVal, runtime::error::RuntimeError>
EvalEngine::invoke_eval_lambda(runtime::nanbox::LispVal closure,
                               std::span<const EvalBinding> lexical_bindings) {
    std::vector<runtime::nanbox::LispVal> args;
    args.reserve(lexical_bindings.size());
    for (const auto& binding : lexical_bindings) {
        args.push_back(binding.value);
    }
    return vm_.call_value(closure, std::move(args));
}

std::vector<EvalEngine::EvalBinding> EvalEngine::collect_eval_lexical_bindings() const {
    std::vector<EvalBinding> bindings;
    std::unordered_set<std::string> seen;

    auto append = [&](const std::vector<runtime::vm::VarEntry>& vars) {
        for (const auto& var : vars) {
            if (var.name.empty() || is_synthetic_eval_binding_name(var.name)) continue;
            if (!seen.insert(var.name).second) continue;
            bindings.push_back(EvalBinding{var.name, var.value});
        }
    };

    const auto frames = vm_.get_frames();
    for (std::size_t frame_index = 0; frame_index < frames.size(); ++frame_index) {
        append(vm_.get_locals(frame_index));
        append(vm_.get_upvalues(frame_index));
    }
    return bindings;
}

} // namespace eta::session
