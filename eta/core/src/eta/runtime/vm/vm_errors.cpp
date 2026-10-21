/// VM runtime errors and exceptions: error payloads, throw/catch and non-local transfer.
#include "vm.h"
#include <algorithm>
#include <chrono>
#include <exception>
#include <future>
#include <limits>
#include "eta/runtime/actor/actor_system.h"
#include "eta/runtime/builtin_catalog.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/ad_error.h"
#include "eta/runtime/types/types.h"
#include "eta/runtime/types/logic_var.h"
#include "eta/runtime/types/primitive.h"
#include "eta/runtime/types/tape.h"
#include "eta/runtime/types/tape_ref.h"
#include "eta/runtime/memory/value_visit.h"
#include "eta/runtime/memory/mark_sweep_gc.h"
#include "eta/runtime/string_view.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/overflow.h"
#include "eta/runtime/port.h"
#include "eta/runtime/clp/domain.h"
#include "eta/runtime/prof/profiler.h"
#include <bit>

namespace eta::runtime::vm {

using namespace eta::runtime::memory::value_visit;

using namespace eta::runtime::memory::factory;
using namespace eta::runtime::types;
namespace prof = eta::runtime::prof;

std::expected<LispVal, RuntimeError> VM::make_list_payload(const std::vector<LispVal>& elements) {
    LispVal out = Nil;
    for (auto it = elements.rbegin(); it != elements.rend(); ++it) {
        temp_roots_.push_back(out);
        temp_roots_.push_back(*it);
        auto cell = make_cons(heap_, *it, out);
        temp_roots_.pop_back();
        temp_roots_.pop_back();
        if (!cell) return std::unexpected(cell.error());
        out = *cell;
    }
    return out;
}

std::expected<LispVal, RuntimeError> VM::runtime_error_tag(const RuntimeError& err) {
    const char* tag_name = "runtime.internal-error";
    std::visit([&](auto&& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, VMError>) {
            if (!e.tag_override.empty()) {
                tag_name = e.tag_override.c_str();
                return;
            }
            switch (e.code) {
                case RuntimeErrorCode::TypeError:       tag_name = "runtime.type-error"; break;
                case RuntimeErrorCode::InvalidArity:    tag_name = "runtime.invalid-arity"; break;
                case RuntimeErrorCode::UserError:       tag_name = "runtime.user-error"; break;
                case RuntimeErrorCode::UndefinedGlobal: tag_name = "runtime.undefined-global"; break;
                case RuntimeErrorCode::SandboxViolation: tag_name = "runtime.sandbox-violation"; break;
                case RuntimeErrorCode::NotImplemented:
                case RuntimeErrorCode::InternalError:
                case RuntimeErrorCode::StackOverflow:
                case RuntimeErrorCode::StackUnderflow:
                case RuntimeErrorCode::FrameOverflow:
                case RuntimeErrorCode::InvalidInstruction:
                case RuntimeErrorCode::UserThrow:
                default:
                    tag_name = "runtime.internal-error";
                    break;
            }
        } else if constexpr (std::is_same_v<T, NaNBoxError>) {
            tag_name = "runtime.nanbox-error";
        } else if constexpr (std::is_same_v<T, HeapError>) {
            tag_name = "runtime.heap-error";
        } else if constexpr (std::is_same_v<T, InternTableError>) {
            tag_name = "runtime.intern-error";
        }
    }, err);
    return make_symbol(intern_table_, tag_name);
}

std::string VM::runtime_error_message(const RuntimeError& err) const {
    return std::visit([](auto&& e) -> std::string {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, VMError>) {
            return e.message;
        } else if constexpr (std::is_same_v<T, NaNBoxError>) {
            return std::string("nanbox error: ") + eta::runtime::nanbox::to_string(e);
        } else if constexpr (std::is_same_v<T, HeapError>) {
            return std::string("heap error: ") + eta::runtime::memory::heap::to_string(e);
        } else if constexpr (std::is_same_v<T, InternTableError>) {
            return std::string("intern error: ") + eta::runtime::memory::intern::to_string(e);
        }
        return std::string("runtime error");
    }, err);
}

std::vector<FrameInfo> VM::capture_runtime_stack_trace(reader::lexer::Span span) const {
    auto trace = get_frames();
    if (!trace.empty()) trace[0].span = span;
    return trace;
}

bool VM::runtime_catch_matches(LispVal catch_tag, LispVal specific_tag, LispVal super_tag) const {
    /// Runtime catch-all (tagless catch) and explicit runtime tags both match.
    if (catch_tag == Nil) return true;
    return values_eqv(catch_tag, specific_tag) || values_eqv(catch_tag, super_tag);
}

std::expected<LispVal, RuntimeError> VM::build_runtime_error_payload(
    const RuntimeError& err, LispVal tag, reader::lexer::Span span) {
    const auto roots_base = temp_roots_.size();
    auto restore_roots = [&] { temp_roots_.resize(roots_base); };

    auto runtime_error_sym = make_symbol(intern_table_, "runtime-error");
    if (!runtime_error_sym) return std::unexpected(runtime_error_sym.error());
    temp_roots_.push_back(*runtime_error_sym);

    auto span_sym = make_symbol(intern_table_, "span");
    if (!span_sym) {
        restore_roots();
        return std::unexpected(span_sym.error());
    }
    temp_roots_.push_back(*span_sym);

    auto frame_sym = make_symbol(intern_table_, "frame");
    if (!frame_sym) {
        restore_roots();
        return std::unexpected(frame_sym.error());
    }
    temp_roots_.push_back(*frame_sym);

    auto to_fixnum = [&](std::uint32_t v) -> std::expected<LispVal, RuntimeError> {
        auto enc = ops::encode<int64_t>(static_cast<int64_t>(v));
        if (enc) return *enc;
        return make_fixnum(heap_, static_cast<int64_t>(v));
    };

    auto build_span_record = [&](const reader::lexer::Span& s) -> std::expected<LispVal, RuntimeError> {
        auto file_id = to_fixnum(s.file_id);
        if (!file_id) return std::unexpected(file_id.error());
        auto start_line = to_fixnum(s.start.line);
        if (!start_line) return std::unexpected(start_line.error());
        auto start_col = to_fixnum(s.start.column);
        if (!start_col) return std::unexpected(start_col.error());
        auto end_line = to_fixnum(s.end.line);
        if (!end_line) return std::unexpected(end_line.error());
        auto end_col = to_fixnum(s.end.column);
        if (!end_col) return std::unexpected(end_col.error());

        std::vector<LispVal> fields{*span_sym, *file_id, *start_line, *start_col, *end_line, *end_col};
        return make_list_payload(fields);
    };

    auto msg_val = make_string(heap_, intern_table_, runtime_error_message(err));
    if (!msg_val) {
        restore_roots();
        return std::unexpected(msg_val.error());
    }
    temp_roots_.push_back(*msg_val);

    auto span_val = build_span_record(span);
    if (!span_val) {
        restore_roots();
        return std::unexpected(span_val.error());
    }
    temp_roots_.push_back(*span_val);

    std::vector<LispVal> frame_rows;
    auto trace = capture_runtime_stack_trace(span);
    frame_rows.reserve(trace.size());
    for (const auto& fr : trace) {
        auto fn = make_string(heap_, intern_table_, fr.func_name);
        if (!fn) {
            restore_roots();
            return std::unexpected(fn.error());
        }
        temp_roots_.push_back(*fn);

        auto frame_span = build_span_record(fr.span);
        if (!frame_span) {
            restore_roots();
            return std::unexpected(frame_span.error());
        }
        temp_roots_.push_back(*frame_span);

        auto frame_row = make_list_payload({*frame_sym, *fn, *frame_span});
        if (!frame_row) {
            restore_roots();
            return std::unexpected(frame_row.error());
        }
        frame_rows.push_back(*frame_row);
        temp_roots_.push_back(*frame_row);
    }

    auto stack_trace = make_list_payload(frame_rows);
    if (!stack_trace) {
        restore_roots();
        return std::unexpected(stack_trace.error());
    }
    temp_roots_.push_back(*stack_trace);

    auto build_field_rows = [&]() -> std::expected<LispVal, RuntimeError> {
        const auto* vm_err = std::get_if<VMError>(&err);
        if (!vm_err || vm_err->fields.empty()) return Nil;

        std::vector<LispVal> rows;
        rows.reserve(vm_err->fields.size());
        for (const auto& field : vm_err->fields) {
            auto key_sym = make_symbol(intern_table_, field.key);
            if (!key_sym) return std::unexpected(key_sym.error());
            temp_roots_.push_back(*key_sym);

            std::expected<LispVal, RuntimeError> value = std::visit([&](auto&& raw)
                -> std::expected<LispVal, RuntimeError> {
                    using U = std::decay_t<decltype(raw)>;
                    if constexpr (std::is_same_v<U, int64_t>) {
                        auto enc = ops::encode<int64_t>(raw);
                        if (enc) return *enc;
                        return make_fixnum(heap_, raw);
                    } else if constexpr (std::is_same_v<U, double>) {
                        return make_flonum(raw);
                    } else {
                        return make_string(heap_, intern_table_, raw);
                    }
                }, field.value);
            if (!value) return std::unexpected(value.error());
            temp_roots_.push_back(*value);

            auto row = make_list_payload({*key_sym, *value});
            if (!row) return std::unexpected(row.error());
            rows.push_back(*row);
            temp_roots_.push_back(*row);
        }
        return make_list_payload(rows);
    };

    auto field_rows = build_field_rows();
    if (!field_rows) {
        restore_roots();
        return std::unexpected(field_rows.error());
    }
    if (*field_rows != Nil) {
        temp_roots_.push_back(*field_rows);
    }

    std::expected<LispVal, RuntimeError> payload = (*field_rows == Nil)
        ? make_list_payload({*runtime_error_sym, tag, *msg_val, *span_val, *stack_trace})
        : make_list_payload({*runtime_error_sym, tag, *msg_val, *span_val, *stack_trace, *field_rows});
    if (!payload) {
        restore_roots();
        return std::unexpected(payload.error());
    }

    restore_roots();
    return *payload;
}

std::expected<void, RuntimeError> VM::do_runtime_error(const RuntimeError& err, reader::lexer::Span span) {
    auto specific_tag = runtime_error_tag(err);
    if (!specific_tag) return std::unexpected(err);

    auto super_tag = make_symbol(intern_table_, "runtime.error");
    if (!super_tag) return std::unexpected(err);

    for (int i = static_cast<int>(catch_stack_.size()) - 1; i >= 0; --i) {
        const CatchFrame& cf = catch_stack_[static_cast<std::size_t>(i)];
        if (!runtime_catch_matches(cf.tag, *specific_tag, *super_tag)) continue;

        auto payload = build_runtime_error_payload(err, *specific_tag, span);
        if (!payload) return std::unexpected(err);
        return begin_exception_transfer(cf, *payload, static_cast<std::size_t>(i));
    }

    if (debug_) debug_->notify_exception(runtime_error_message(err), span);
    return std::unexpected(err);
}

/**
 * Exception Throw
 */

std::expected<void, RuntimeError> VM::do_throw(LispVal tag, LispVal value,
                                                reader::lexer::Span span) {
    /// Search the catch stack from top (most recent) to bottom for a matching tag.
    for (int i = static_cast<int>(catch_stack_.size()) - 1; i >= 0; --i) {
        const CatchFrame& cf = catch_stack_[static_cast<std::size_t>(i)];
        /// Match if tags are equal (same symbol) OR the catch frame is a catch-all (Nil tag).
        bool matches = (cf.tag == Nil) || values_eqv(cf.tag, tag);
        if (matches) {
            return begin_exception_transfer(cf, value, static_cast<std::size_t>(i));
        }
    }

    std::string tag_str = "unknown-tag";
    if (ops::is_boxed(tag) && ops::tag(tag) == Tag::Symbol) {
        auto name = intern_table_.get_string(ops::payload(tag));
        if (name) tag_str = std::string(*name);
    }
    std::string msg = "Unhandled raise: " + tag_str;

    if (debug_) debug_->notify_exception(msg, span);

    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::UserThrow, msg}});
}

void VM::clear_exception_transfer() noexcept {
    pending_exception_transfer_.reset();
    pending_unwind_thunks_.clear();
    pending_unwind_index_ = 0;
}

std::expected<void, RuntimeError> VM::resume_exception_transfer() {
    if (!pending_exception_transfer_) {
        return std::unexpected(RuntimeError{
            VMError{RuntimeErrorCode::InternalError, "missing pending exception transfer"}});
    }

    while (pending_unwind_index_ < pending_unwind_thunks_.size()) {
        LispVal thunk = pending_unwind_thunks_[pending_unwind_index_++];
        auto dispatch = dispatch_callee(thunk, 0, false);
        if (!dispatch) {
            clear_exception_transfer();
            return std::unexpected(dispatch.error());
        }

        if (dispatch->action == DispatchAction::SetupFrame) {
            setup_frame(dispatch->func, dispatch->closure, 0, FrameKind::ExceptionUnwind);
            return {};
        }

        if (dispatch->action == DispatchAction::NonLocalTransfer) {
            /**
             * An unwind thunk performed its own non-local transfer (e.g. raise).
             * The original transfer is abandoned.
             */
            clear_exception_transfer();
            return {};
        }

        /// Primitive thunk path: ignore result and continue to next thunk.
        if (!stack_.empty()) pop();
    }

    const auto state = *pending_exception_transfer_;
    clear_exception_transfer();

    /// Restore VM frame state to the matching catch handler.
    frames_.resize(state.frame_count);
    winding_stack_.resize(state.wind_count);
    active_tapes_.resize(state.tape_count);

    /// Restore stack to the saved top, then push the caught payload.
    stack_.resize(state.stack_top);
    push(state.payload);

    /// Restore execution context to the function containing SetupCatch.
    current_func_    = state.func;
    fp_              = state.fp;
    current_closure_ = state.closure;
    pc_              = state.handler_pc;
    return {};
}

std::expected<void, RuntimeError> VM::begin_exception_transfer(const CatchFrame& cf,
                                                               LispVal payload,
                                                               std::size_t matched_catch_index) {
    /// Pop all catch frames from this one upward.
    catch_stack_.resize(matched_catch_index);

    if (winding_stack_.size() <= cf.wind_count) {
        /// Fast path: no dynamic-wind frames to exit.
        frames_.resize(cf.frame_count);
        winding_stack_.resize(cf.wind_count);
        active_tapes_.resize(cf.tape_count);

        stack_.resize(cf.stack_top);
        push(payload);

        current_func_    = cf.func;
        fp_              = cf.fp;
        current_closure_ = cf.closure;
        pc_              = cf.handler_pc;
        return {};
    }

    pending_exception_transfer_ = PendingExceptionTransfer{
        .func = cf.func,
        .handler_pc = cf.handler_pc,
        .fp = cf.fp,
        .closure = cf.closure,
        .frame_count = cf.frame_count,
        .stack_top = cf.stack_top,
        .wind_count = cf.wind_count,
        .tape_count = cf.tape_count,
        .payload = payload,
    };

    pending_unwind_thunks_.clear();
    pending_unwind_thunks_.reserve(winding_stack_.size() - cf.wind_count);
    for (int32_t idx = static_cast<int32_t>(winding_stack_.size()) - 1;
         idx >= static_cast<int32_t>(cf.wind_count);
         --idx) {
        pending_unwind_thunks_.push_back(winding_stack_[static_cast<std::size_t>(idx)].after);
    }
    pending_unwind_index_ = 0;

    return resume_exception_transfer();
}
} ///< namespace eta::runtime::vm

