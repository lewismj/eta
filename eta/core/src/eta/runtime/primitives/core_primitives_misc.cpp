#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include "eta/runtime/core_primitives_internal.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/prof/profiler.h"
#include "eta/runtime/string_view.h"
#include "eta/runtime/value_formatter.h"

namespace eta::runtime {

void PrimReg::register_misc() {
    using Args = PrimReg::Args;
    auto& env = this->env;
    auto& heap = this->heap;
    auto& intern_table = this->intern_table;

    /**
     * Error signaling: error
     */

    env.register_builtin("error", 1, true, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        /**
         * (error message irritant ...)
         * First arg should be a string message
         */
        std::string msg;
        auto sv = StringView::try_from(args[0], intern_table);
        if (sv) {
            msg = std::string(sv->view());
        } else {
            msg = format_value(args[0], FormatMode::Write, heap, intern_table);
        }
        /// Append irritants
        for (size_t i = 1; i < args.size(); ++i) {
            msg += " ";
            msg += format_value(args[i], FormatMode::Write, heap, intern_table);
        }
        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::UserError, msg}});
    });

    /**
     * Platform detection: platform
     * Returns a symbol identifying the executing host OS at runtime.
     * The #if selects which string is compiled into each platform's binary;
     * the primitive is always invoked at VM execution time, so bytecode built
     * on one platform and run on another correctly reports the executing host.
     */

    env.register_builtin("platform", 0, false, [&intern_table](Args) -> std::expected<LispVal, RuntimeError> {
#if defined(_WIN32)
        return make_symbol(intern_table, "Win32");
#elif defined(__APPLE__)
        return make_symbol(intern_table, "Darwin");
#elif defined(__linux__)
        return make_symbol(intern_table, "Linux");
#else
        return make_symbol(intern_table, "Unknown");
#endif
    });

    /**
     * Profiler controls consumed by std.prof.
     */

    env.register_builtin("%prof-start", 0, true, [&heap, &intern_table](Args args)
        -> std::expected<LispVal, RuntimeError> {
        std::string mode = "trace";
        std::uint32_t sample_hz = 1000;
        if (!args.empty()) {
            auto mode_sv = StringView::try_from(args[0], intern_table);
            if (!mode_sv) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "%prof-start: mode must be a string or symbol"}});
            }
            mode = std::string(mode_sv->view());
        }

        if (mode != "trace" && mode != "sample") {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::UserError,
                "%prof-start: mode must be 'trace' or 'sample'"}});
        }

        if (args.size() > 2) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::InvalidArity,
                "%prof-start: expected at most 2 arguments"}});
        }

        if (args.size() >= 2) {
            auto hz = classify_numeric(args[1], heap);
            if (!hz.is_valid() || !hz.is_fixnum() || hz.int_val <= 0) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "%prof-start: hz must be a positive integer"}});
            }
            sample_hz = static_cast<std::uint32_t>(hz.int_val);
        }

        auto& profiler = eta::runtime::prof::runtime_profiler();
        auto session = (mode == "sample")
            ? profiler.start_sample_session(sample_hz)
            : profiler.start_trace_session();
        if (!session) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::UserError,
                "%prof-start: a profiling session is already active"}});
        }
        return make_fixnum(heap, static_cast<int64_t>(*session));
    });

    env.register_builtin("%prof-stop", 1, false, [&heap](Args args)
        -> std::expected<LispVal, RuntimeError> {
        auto id_num = classify_numeric(args[0], heap);
        if (!id_num.is_valid() || !id_num.is_fixnum() || id_num.int_val <= 0) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError,
                "%prof-stop: session handle must be a positive integer"}});
        }

        const auto session_id = static_cast<std::uint64_t>(id_num.int_val);
        auto& profiler = eta::runtime::prof::runtime_profiler();
        if (!profiler.stop_trace_session(session_id)
            && !profiler.stop_sample_session(session_id)) {
            return False;
        }
        return make_fixnum(heap, static_cast<int64_t>(session_id));
    });

    env.register_builtin("%prof-report", 1, true, [&heap, &intern_table](Args args)
        -> std::expected<LispVal, RuntimeError> {
        if (args.size() > 2) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::InvalidArity,
                "%prof-report: expected one or two arguments"}});
        }

        auto id_num = classify_numeric(args[0], heap);
        if (!id_num.is_valid() || !id_num.is_fixnum() || id_num.int_val <= 0) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError,
                "%prof-report: session handle must be a positive integer"}});
        }

        std::string format = "pretty";
        if (args.size() >= 2) {
            auto format_sv = StringView::try_from(args[1], intern_table);
            if (!format_sv) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "%prof-report: format must be a string or symbol"}});
            }
            format = std::string(format_sv->view());
        }

        const auto session_id = static_cast<std::uint64_t>(id_num.int_val);
        auto& profiler = eta::runtime::prof::runtime_profiler();
        std::optional<std::string> report;
        if (format == "pretty") {
            report = profiler.render_pretty_report_for_session(session_id);
        } else if (format == "json") {
            report = profiler.render_json_report_for_session(session_id);
        } else if (format == "speedscope") {
            report = profiler.render_speedscope_report_for_session(session_id);
        } else if (format == "chrome") {
            report = profiler.render_chrome_report_for_session(session_id);
        } else if (format == "eta-prof") {
            report = profiler.render_archive_report_for_session(session_id);
        } else {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::UserError,
                "%prof-report: unknown format"}});
        }

        if (!report) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::UserError,
                "%prof-report: unknown profiler session handle"}});
        }
        return make_string(heap, intern_table, *report);
    });

    env.register_builtin("%prof-counter", 2, false, [&heap, &intern_table](Args args)
        -> std::expected<LispVal, RuntimeError> {
        auto name_sv = StringView::try_from(args[0], intern_table);
        if (!name_sv) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError,
                "%prof-counter: name must be a string or symbol"}});
        }

        auto value_num = classify_numeric(args[1], heap);
        if (!value_num.is_valid() || !value_num.is_fixnum() || value_num.int_val < 0) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError,
                "%prof-counter: value must be a non-negative integer"}});
        }

        eta::runtime::prof::runtime_profiler().add_counter(
            name_sv->view(),
            static_cast<std::uint64_t>(value_num.int_val));
        return True;
    });

    env.register_builtin("%prof-region-enter", 1, false, [&intern_table](Args args)
        -> std::expected<LispVal, RuntimeError> {
        auto name_sv = StringView::try_from(args[0], intern_table);
        if (!name_sv) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError,
                "%prof-region-enter: name must be a string or symbol"}});
        }
        eta::runtime::prof::runtime_profiler().push_user_region(name_sv->view());
        return True;
    });

    env.register_builtin("%prof-region-exit", 0, false, [](Args)
        -> std::expected<LispVal, RuntimeError> {
        eta::runtime::prof::runtime_profiler().pop_user_region();
        return True;
    });

    env.register_builtin("%prof-enabled?", 0, false, [](Args)
        -> std::expected<LispVal, RuntimeError> {
        return eta::runtime::prof::runtime_profiler().enabled() ? True : False;
    });
}

void PrimReg::register_misc_lifecycle_bridge() {
    using Args = PrimReg::Args;
    auto& env = this->env;
    auto& heap = this->heap;

    env.register_builtin("register-finalizer!", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        const LispVal obj = args[0];
        const LispVal proc = args[1];

        if (!ops::is_boxed(obj) || ops::tag(obj) != Tag::HeapObject) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "register-finalizer!: first arg must be a heap object"}});
        }

        if (!ops::is_boxed(proc) || ops::tag(proc) != Tag::HeapObject) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "register-finalizer!: second arg must be a procedure"}});
        }
        const auto proc_id = static_cast<memory::heap::ObjectId>(ops::payload(proc));
        if (!heap.try_get_as<ObjectKind::Closure, types::Closure>(proc_id)
            && !heap.try_get_as<ObjectKind::Primitive, types::Primitive>(proc_id)) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "register-finalizer!: second arg must be a procedure"}});
        }

        const auto obj_id = static_cast<memory::heap::ObjectId>(ops::payload(obj));
        auto registered = heap.register_finalizer(obj_id, proc);
        if (!registered.has_value()) {
            if (registered.error() == memory::heap::HeapError::ObjectIdNotFound) {
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "register-finalizer!: first arg must be a live heap object"}});
            }
            if (registered.error() == memory::heap::HeapError::UnexpectedObjectKind) {
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "register-finalizer!: first arg must be a non-cons heap object"}});
            }
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "register-finalizer!: failed to register finalizer"}});
        }

        return True;
    });

    env.register_builtin("unregister-finalizer!", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        const LispVal obj = args[0];
        if (!ops::is_boxed(obj) || ops::tag(obj) != Tag::HeapObject) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "unregister-finalizer!: first arg must be a heap object"}});
        }

        const auto obj_id = static_cast<memory::heap::ObjectId>(ops::payload(obj));
        return heap.remove_finalizer(obj_id) ? True : False;
    });

    env.register_builtin("make-guardian", 0, false, [&heap](Args) -> std::expected<LispVal, RuntimeError> {
        return make_guardian(heap);
    });

    env.register_builtin("guardian-track!", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        const LispVal guardian = args[0];
        const LispVal obj = args[1];

        if (!ops::is_boxed(guardian) || ops::tag(guardian) != Tag::HeapObject) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "guardian-track!: first arg must be a guardian"}});
        }

        const auto guardian_id = static_cast<memory::heap::ObjectId>(ops::payload(guardian));
        if (!heap.try_get_as<ObjectKind::Guardian, types::Guardian>(guardian_id)) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "guardian-track!: first arg must be a guardian"}});
        }

        if (!ops::is_boxed(obj) || ops::tag(obj) != Tag::HeapObject) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "guardian-track!: second arg must be a heap object"}});
        }

        const auto obj_id = static_cast<memory::heap::ObjectId>(ops::payload(obj));
        auto tracked = heap.guardian_track(guardian_id, obj_id);
        if (!tracked.has_value()) {
            if (tracked.error() == memory::heap::HeapError::ObjectIdNotFound) {
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "guardian-track!: second arg must be a live heap object"}});
            }
            if (tracked.error() == memory::heap::HeapError::UnexpectedObjectKind) {
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "guardian-track!: second arg must be a non-cons heap object"}});
            }
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "guardian-track!: failed to track object"}});
        }

        return True;
    });

    env.register_builtin("guardian-collect", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        const LispVal guardian = args[0];
        if (!ops::is_boxed(guardian) || ops::tag(guardian) != Tag::HeapObject) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "guardian-collect: arg must be a guardian"}});
        }

        const auto guardian_id = static_cast<memory::heap::ObjectId>(ops::payload(guardian));
        if (!heap.try_get_as<ObjectKind::Guardian, types::Guardian>(guardian_id)) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "guardian-collect: arg must be a guardian"}});
        }

        auto next = heap.dequeue_guardian_ready(guardian_id);
        return next.has_value() ? *next : False;
    });
}

void PrimReg::register_misc_eval_bridge() {
    using Args = PrimReg::Args;
    auto& env = this->env;

    /**
     * Eval is installed by Driver after primitive registration so it can
     * capture lexical environment and delegate compilation through Driver.
     */
    env.register_builtin("eval", 1, false,
        [](Args /*args*/) -> std::expected<LispVal, RuntimeError> {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::InternalError,
                "eval: runtime stub invoked before driver installation"}});
        });
}

} // namespace eta::runtime
