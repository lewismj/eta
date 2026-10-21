/**
 * @file numeric_tests.cpp
 * @brief Numeric conformance: every expression must give the same answer
 *        through the primitive call, the specialised opcodes, and `etac -O`.
 *
 * Each case is compiled three ways:
 *   - Primitive: no passes, so `+`, `<`, ... are generic primitive calls
 *   - Opcode:    PrimitiveSpecialisation (what the Driver always runs)
 *   - Optimised: PrimitiveSpecialisation + ConstantFolding + DCE (`etac -O`)
 *
 * Results are compared exactly: fixnum vs flonum, the sign of zero, NaN,
 * and the text of any runtime error.
 */

#include <boost/test/unit_test.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "eta/reader/expander.h"
#include "eta/reader/lexer.h"
#include "eta/reader/module_linker.h"
#include "eta/reader/parser.h"
#include "eta/runtime/builtin_env.h"
#include "eta/runtime/core_primitives.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/numeric_ops.h"
#include "eta/runtime/types/cons.h"
#include "eta/runtime/value_formatter.h"
#include "eta/runtime/vm/sandbox.h"
#include "eta/runtime/vm/vm.h"
#include "eta/semantics/emitter.h"
#include "eta/semantics/optimization_pipeline.h"
#include "eta/semantics/passes/constant_folding.h"
#include "eta/semantics/passes/dead_code_elimination.h"
#include "eta/semantics/passes/primitive_specialisation.h"
#include "eta/semantics/semantic_analyzer.h"

using namespace eta;
using namespace eta::semantics;
using namespace eta::runtime;
using namespace eta::runtime::vm;
using namespace eta::runtime::nanbox;

namespace {

enum class Mode { Primitive, Opcode, Optimised };

constexpr Mode kAllModes[] = {Mode::Primitive, Mode::Opcode, Mode::Optimised};

const char* to_string(const Mode mode) {
    switch (mode) {
        case Mode::Primitive: return "primitive";
        case Mode::Opcode:    return "opcode";
        case Mode::Optimised: return "optimised";
    }
    return "?";
}

/// What a program evaluated to, decoded while its VM was still alive.
struct Outcome {
    enum class Kind { Fixnum, Flonum, Boolean, Error, Other };
    Kind kind{Kind::Other};
    int64_t int_val{};
    double float_val{};
    bool bool_val{};
    std::string text; ///< error message, or the formatted value for Other
};

std::ostream& operator<<(std::ostream& os, const Outcome& o) {
    switch (o.kind) {
        case Outcome::Kind::Fixnum:  return os << "fixnum " << o.int_val;
        case Outcome::Kind::Flonum:  return os << "flonum " << o.float_val
                                               << (std::signbit(o.float_val) ? " (sign set)" : "");
        case Outcome::Kind::Boolean: return os << (o.bool_val ? "#t" : "#f");
        case Outcome::Kind::Error:   return os << "error \"" << o.text << "\"";
        case Outcome::Kind::Other:   return os << "other " << o.text;
    }
    return os;
}

Outcome fix(int64_t v)            { Outcome o; o.kind = Outcome::Kind::Fixnum;  o.int_val = v;   return o; }
Outcome flo(double v)             { Outcome o; o.kind = Outcome::Kind::Flonum;  o.float_val = v; return o; }
Outcome boolean(bool v)           { Outcome o; o.kind = Outcome::Kind::Boolean; o.bool_val = v;  return o; }
Outcome raises(std::string msg)   { Outcome o; o.kind = Outcome::Kind::Error;   o.text = std::move(msg); return o; }

/// Exact match: flonums compare by bit pattern (so -0.0 != 0.0), except that any NaN matches NaN.
bool same(const Outcome& expected, const Outcome& actual) {
    if (expected.kind != actual.kind) return false;
    switch (expected.kind) {
        case Outcome::Kind::Fixnum:  return expected.int_val == actual.int_val;
        case Outcome::Kind::Flonum:
            if (std::isnan(expected.float_val)) return std::isnan(actual.float_val);
            return std::bit_cast<uint64_t>(expected.float_val) == std::bit_cast<uint64_t>(actual.float_val);
        case Outcome::Kind::Boolean: return expected.bool_val == actual.bool_val;
        case Outcome::Kind::Error:   return expected.text == actual.text;
        case Outcome::Kind::Other:   return expected.text == actual.text;
    }
    return false;
}

struct Case {
    std::string_view expr;
    Outcome expected;
};

/// Shared definitions available to every case.  INT64_MIN has no literal, so build it.
constexpr std::string_view kPrelude =
    "(define min64 (- (- 0 9223372036854775807) 1)) "
    "(define max64 9223372036854775807) ";

constexpr double kTwo63 = 9223372036854775808.0;

/**
 * One compile-and-run: a fresh heap, VM and builtins each time, so a VM's GC
 * callback never outlives it and no run can see another's globals.
 */
struct Session {
    memory::heap::Heap heap{8 * 1024 * 1024};
    memory::intern::InternTable intern_table;
    BytecodeFunctionRegistry registry;
    BuiltinEnvironment builtins;

    Session() {
        register_core_primitives(builtins, heap, intern_table);
    }

    /// Compile and run @p source, and decode the `result` global.
    Outcome run(const std::string& source, const Mode mode) {
        reader::lexer::Lexer lex(0, source);
        reader::parser::Parser p(lex);
        auto parsed = p.parse_toplevel();
        if (!parsed) throw std::runtime_error("parse error: " + source);

        reader::expander::Expander ex;
        auto expanded = ex.expand_many(*parsed);
        if (!expanded) throw std::runtime_error("expansion error: " + expanded.error().message);

        reader::ModuleLinker linker;
        (void) linker.index_modules(*expanded);
        (void) linker.link();

        SemanticAnalyzer sa;
        auto sem_mods = sa.analyze_all(*expanded, linker, builtins);
        if (!sem_mods) throw std::runtime_error("semantic error: " + sem_mods.error().message);

        OptimizationPipeline pipeline;
        if (mode != Mode::Primitive) {
            pipeline.add_pass(std::make_unique<passes::PrimitiveSpecialisation>());
        }
        if (mode == Mode::Optimised) {
            pipeline.add_pass(std::make_unique<passes::ConstantFolding>());
            pipeline.add_pass(std::make_unique<passes::DeadCodeElimination>());
        }
        pipeline.run_all(*sem_mods);

        auto& sem_mod = (*sem_mods)[0];
        Emitter emitter(sem_mod, heap, intern_table, registry);
        auto* main_func = emitter.emit();

        VM vm(heap, intern_table);
        vm.set_function_resolver([this](uint32_t idx) { return registry.get(idx); });
        /// Re-register with the VM so the tape and AD-policy builtins work
        builtins.clear();
        register_core_primitives(builtins, heap, intern_table, &vm);
        if (!builtins.install(heap, vm.globals(), sem_mod.total_globals)) {
            throw std::runtime_error("failed to install builtins");
        }

        if (!vm.execute(*main_func)) {
            throw std::runtime_error("uncaught execution error: " + source);
        }

        for (const auto& bi : sem_mod.bindings) {
            if (bi.name == "result") return decode(vm.globals()[bi.slot]);
        }
        throw std::runtime_error("no result binding: " + source);
    }

private:
    Outcome decode(const LispVal v) {
        Outcome o;
        if (v == True || v == False) {
            o.kind = Outcome::Kind::Boolean;
            o.bool_val = v == True;
            return o;
        }
        if (auto payload = runtime_error_message(v)) {
            o.kind = Outcome::Kind::Error;
            o.text = *payload;
            return o;
        }
        const auto n = numeric_ops::classify(v, heap);
        if (n.is_fixnum()) {
            o.kind = Outcome::Kind::Fixnum;
            o.int_val = n.int_val;
            return o;
        }
        if (n.is_flonum()) {
            o.kind = Outcome::Kind::Flonum;
            o.float_val = n.float_val;
            return o;
        }
        o.text = format_value(v, FormatMode::Write, heap, intern_table);
        return o;
    }

    const types::Cons* as_cons(const LispVal v) {
        if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return nullptr;
        return heap.try_get_as<memory::heap::ObjectKind::Cons, types::Cons>(ops::payload(v));
    }

    /// (runtime-error tag msg span trace) -> msg
    std::optional<std::string> runtime_error_message(const LispVal v) {
        const auto* head = as_cons(v);
        if (!head) return std::nullopt;
        if (format_value(head->car, FormatMode::Write, heap, intern_table) != "runtime-error") {
            return std::nullopt;
        }
        const auto* tag = as_cons(head->cdr);
        const auto* msg = tag ? as_cons(tag->cdr) : nullptr;
        if (!msg) return std::nullopt;
        return format_value(msg->car, FormatMode::Display, heap, intern_table);
    }
};

struct NumericFixture {
    /// Run module-level forms @p body after the shared prelude.
    Outcome run(std::string_view body, const Mode mode) {
        Session session;
        return session.run("(module m " + std::string(kPrelude) + std::string(body) + ")", mode);
    }

    /// `(define result (catch <expr>))`, so errors come back as a runtime-error payload.
    Outcome eval(std::string_view expr, const Mode mode) {
        return run("(define result (catch " + std::string(expr) + "))", mode);
    }

    void check_cases(const std::vector<Case>& cases) {
        for (const auto& c : cases) {
            for (const Mode mode : kAllModes) {
                BOOST_TEST_CONTEXT(c.expr << " [" << to_string(mode) << "]") {
                    const Outcome actual = eval(c.expr, mode);
                    BOOST_TEST(same(c.expected, actual),
                               "expected " << c.expected << ", got " << actual);
                }
            }
        }
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(numeric_tests, NumericFixture)

/**
 * Exact arithmetic, identities, and the variadic forms
 */

BOOST_AUTO_TEST_CASE(exact_arithmetic) {
    check_cases({
        {"(+ 1 2)", fix(3)},
        {"(+)", fix(0)},
        {"(*)", fix(1)},
        {"(+ 5)", fix(5)},
        {"(* 5)", fix(5)},
        {"(- 5)", fix(-5)},
        {"(- 0)", fix(0)},
        {"(- 10 3 2)", fix(5)},
        {"(+ 1 2 3 4 5)", fix(15)},
        {"(* 2 3 4)", fix(24)},
        {"(/ 6 3)", fix(2)},
        {"(/ 24 2 3)", fix(4)},
        {"(/ -6 3)", fix(-2)},
        {"(/ 1)", fix(1)},
        {"(/ -1)", fix(-1)},
    });
}

BOOST_AUTO_TEST_CASE(inexact_and_mixed_arithmetic) {
    check_cases({
        {"(/ 7 2)", flo(3.5)},
        {"(/ 6 4 2)", flo(0.75)},
        {"(/ 2)", flo(0.5)},
        {"(/ 0.5)", flo(2.0)},
        {"(+ 1 2.5)", flo(3.5)},
        {"(+ 2.5 1)", flo(3.5)},
        {"(* 2 0.5)", flo(1.0)},     ///< inexact contaminates: 1.0, not 1
        {"(- 1.5 1.5)", flo(0.0)},
        {"(* 1.5 2 2)", flo(6.0)},
        {"(+ 0.1 0.2)", flo(0.1 + 0.2)},
    });
}

BOOST_AUTO_TEST_CASE(signed_zero_and_nan) {
    check_cases({
        {"(- 0.0)", flo(-0.0)},
        {"(+ -0.0 -0.0)", flo(-0.0)},
        {"(+ -0.0 0.0)", flo(0.0)},
        {"(+ -0.0 0)", flo(0.0)},
        {"(* -1 0.0)", flo(-0.0)},
        {"(* -0.0 -0.0)", flo(0.0)},
        {"(- 0.0 0.0)", flo(0.0)},
        {"(+ +nan.0 1)", flo(std::numeric_limits<double>::quiet_NaN())},
        {"(* +nan.0 0)", flo(std::numeric_limits<double>::quiet_NaN())},
        {"(- +inf.0 +inf.0)", flo(std::numeric_limits<double>::quiet_NaN())},
        {"(+ +inf.0 1)", flo(std::numeric_limits<double>::infinity())},
    });
}

/**
 * Fixnum range: the 47-bit immediate boundary, heap fixnums, and int64 overflow
 */

BOOST_AUTO_TEST_CASE(immediate_fixnum_boundary) {
    check_cases({
        {"(+ 70368744177663 1)", fix(70368744177664)},
        {"(- -70368744177664 1)", fix(-70368744177665)},
        {"(* 8388608 16777216)", fix(140737488355328)},
        {"(+ 4611686018427387904 1)", fix(4611686018427387905)},
        {"(- 4611686018427387904 4611686018427387904)", fix(0)},
        {"(/ 4611686018427387904 2)", fix(2305843009213693952)},
        {"(+ max64 0)", fix(INT64_MAX)},
        {"(+ min64 0)", fix(INT64_MIN)},
    });
}

BOOST_AUTO_TEST_CASE(overflow_promotes_to_flonum) {
    check_cases({
        {"(+ max64 1)", flo(kTwo63)},
        {"(- min64 1)", flo(-kTwo63)},
        {"(* 4611686018427387904 4)", flo(18446744073709551616.0)},
        {"(+ 4611686018427387904 4611686018427387904)", flo(kTwo63)},
        {"(+ 4611686018427387904 4611686018427387904 1)", flo(kTwo63)},
        {"(* min64 -1)", flo(kTwo63)},
        {"(/ min64 -1)", flo(kTwo63)},
        {"(- min64)", flo(kTwo63)},
        {"(abs min64)", flo(kTwo63)},
        {"(- max64)", fix(-INT64_MAX)},
        {"(abs max64)", fix(INT64_MAX)},
    });
}

/**
 * Errors: same message whether raised by a primitive or an opcode, and catchable
 */

BOOST_AUTO_TEST_CASE(arithmetic_errors) {
    check_cases({
        {"(+ 1 'a)", raises("+: argument is not a number")},
        {"(+ 'a 1)", raises("+: argument is not a number")},
        {"(+ 1 2 'a)", raises("+: argument is not a number")},
        {"(- 'a)", raises("-: argument is not a number")},
        {"(- 1 'a)", raises("-: argument is not a number")},
        {"(* 2 \"x\")", raises("*: argument is not a number")},
        {"(/ 1 'a)", raises("/: argument is not a number")},
        /// Only an exact zero divisor is an error (R7RS); see ieee_division_by_inexact_zero
        {"(/ 1 0)", raises("/: division by zero")},
        {"(/ 0)", raises("/: division by zero")},
        {"(/ 6 2 0)", raises("/: division by zero")},
        {"(/ 1.0 0)", raises("/: division by zero")},
        {"(/ 0.0 0)", raises("/: division by zero")},
    });
}

BOOST_AUTO_TEST_CASE(ieee_division_by_inexact_zero) {
    constexpr double inf = std::numeric_limits<double>::infinity();
    check_cases({
        {"(/ 1.0 0.0)", flo(inf)},
        {"(/ -1.0 0.0)", flo(-inf)},
        {"(/ 1.0 -0.0)", flo(-inf)},
        {"(/ 1 0.0)", flo(inf)},
        {"(/ 0.0 0.0)", flo(std::numeric_limits<double>::quiet_NaN())},
        {"(/ 0 0.0)", flo(std::numeric_limits<double>::quiet_NaN())},
        {"(/ 0.0)", flo(inf)},
        {"(/ -0.0)", flo(-inf)},
        {"(/ 5 2 0.0)", flo(inf)},
        {"(/ +inf.0 0.0)", flo(inf)},
    });
}

BOOST_AUTO_TEST_CASE(exact_mixed_comparison) {
    /// 2^53 + 1 is not a double: converting it first would make these wrong
    check_cases({
        {"(= 9007199254740993 9007199254740992.0)", boolean(false)},
        {"(= 9007199254740992.0 9007199254740993)", boolean(false)},
        {"(> 9007199254740993 9007199254740992.0)", boolean(true)},
        {"(< 9007199254740992.0 9007199254740993)", boolean(true)},
        {"(<= 9007199254740993 9007199254740992.0)", boolean(false)},
        {"(= 9007199254740992 9007199254740992.0)", boolean(true)},
        {"(< max64 9223372036854775808.0)", boolean(true)},
        {"(= max64 9223372036854775808.0)", boolean(false)},
        {"(= min64 -9223372036854775808.0)", boolean(true)},
        {"(> min64 -9223372036854775808.0)", boolean(false)},
        {"(< min64 -9223372036854774784.0)", boolean(true)},
        {"(> min64 -inf.0)", boolean(true)},
        {"(< max64 +inf.0)", boolean(true)},
        {"(< 1 1.5)", boolean(true)},
        {"(> 2 1.5)", boolean(true)},
        {"(< -2 -1.5)", boolean(true)},
        {"(<= -1 -1.5)", boolean(false)},
        {"(>= -1 -1.5)", boolean(true)},
        {"(= 0 -0.0)", boolean(true)},
        {"(= 1 +nan.0)", boolean(false)},
        {"(< 1 +nan.0)", boolean(false)},
        {"(>= +nan.0 1)", boolean(false)},
        {"(< 1 9007199254740992.5)", boolean(true)},
    });
}

BOOST_AUTO_TEST_CASE(min_max_ieee) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    check_cases({
        {"(min 1 2)", fix(1)},
        {"(max 1 2)", fix(2)},
        {"(min 3 1 2)", fix(1)},
        {"(max 1 2.0)", flo(2.0)},
        {"(min 1 2.0)", flo(1.0)},    ///< inexact contagion
        {"(min 1 +nan.0)", flo(nan)},
        {"(min +nan.0 1)", flo(nan)},
        {"(max 3 +nan.0 1)", flo(nan)},
        {"(min 0.0 -0.0)", flo(-0.0)},
        {"(min -0.0 0.0)", flo(-0.0)},
        {"(max -0.0 0.0)", flo(0.0)},
        {"(max 0.0 -0.0)", flo(0.0)},
        {"(min 0 -0.0)", flo(-0.0)},
        {"(max -0.0 0)", flo(0.0)},
        {"(min -inf.0 1)", flo(-std::numeric_limits<double>::infinity())},
        {"(min 'a 1)", raises("min: argument is not a number")},
        {"(max 1 'a)", raises("max: argument is not a number")},
    });
}

/**
 * Rounding: IEEE 754 roundToIntegral, exact integers unchanged
 */

BOOST_AUTO_TEST_CASE(floor_ceiling_truncate) {
    constexpr double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    check_cases({
        {"(floor 2.5)", flo(2.0)},
        {"(floor -2.5)", flo(-3.0)},
        {"(floor -0.5)", flo(-1.0)},
        {"(floor 0.5)", flo(0.0)},
        {"(floor -0.0)", flo(-0.0)},
        {"(floor +inf.0)", flo(inf)},
        {"(floor -inf.0)", flo(-inf)},
        {"(floor +nan.0)", flo(nan)},
        {"(floor 7)", fix(7)},
        {"(floor -7)", fix(-7)},
        {"(ceiling 2.5)", flo(3.0)},
        {"(ceiling -2.5)", flo(-2.0)},
        {"(ceiling -0.5)", flo(-0.0)},
        {"(ceiling 0.5)", flo(1.0)},
        {"(ceiling +inf.0)", flo(inf)},
        {"(ceiling 4611686018427387904)", fix(4611686018427387904)},
        {"(truncate 2.7)", flo(2.0)},
        {"(truncate -2.7)", flo(-2.0)},
        {"(truncate -0.5)", flo(-0.0)},
        {"(truncate +nan.0)", flo(nan)},
        {"(truncate -inf.0)", flo(-inf)},
        {"(floor 'a)", raises("floor: argument is not a number")},
        {"(round \"x\")", raises("round: argument is not a number")},
    });
}

BOOST_AUTO_TEST_CASE(round_ties_to_even) {
    constexpr double inf = std::numeric_limits<double>::infinity();
    check_cases({
        {"(round 0.5)", flo(0.0)},
        {"(round 1.5)", flo(2.0)},
        {"(round 2.5)", flo(2.0)},
        {"(round 3.5)", flo(4.0)},
        {"(round -0.5)", flo(-0.0)},
        {"(round -1.5)", flo(-2.0)},
        {"(round -2.5)", flo(-2.0)},
        {"(round 2.4)", flo(2.0)},
        {"(round 2.6)", flo(3.0)},
        {"(round -2.6)", flo(-3.0)},
        {"(round -0.4)", flo(-0.0)},
        {"(round -0.0)", flo(-0.0)},
        {"(round 0.49999999999999994)", flo(0.0)},   ///< floor(x + 0.5) gives 1.0
        {"(round 4503599627370497.0)", flo(4503599627370497.0)},
        {"(round +inf.0)", flo(inf)},
        {"(round +nan.0)", flo(std::numeric_limits<double>::quiet_NaN())},
        {"(round 7)", fix(7)},
        {"(round max64)", fix(INT64_MAX)},
    });
}

BOOST_AUTO_TEST_CASE(comparison_errors) {
    check_cases({
        {"(< 1 'a)", raises("<: argument is not a number")},
        {"(> 'a 1)", raises(">: argument is not a number")},
        {"(<= 1 \"x\")", raises("<=: argument is not a number")},
        {"(>= #t 1)", raises(">=: argument is not a number")},
        {"(= 'a 'a)", raises("=: argument is not a number")},
    });
}

/**
 * Comparison: numeric (not eqv?) equality, IEEE NaN and signed zero, heap fixnums
 */

BOOST_AUTO_TEST_CASE(numeric_equality) {
    check_cases({
        {"(= 1 1)", boolean(true)},
        {"(= 1 2)", boolean(false)},
        {"(= 1 1.0)", boolean(true)},
        {"(= 1.0 1)", boolean(true)},
        {"(= 0.0 -0.0)", boolean(true)},
        {"(= 0 -0.0)", boolean(true)},
        {"(= +nan.0 +nan.0)", boolean(false)},
        {"(= +nan.0 1)", boolean(false)},
        {"(= +inf.0 +inf.0)", boolean(true)},
        {"(= 4611686018427387904 4611686018427387904)", boolean(true)},
        {"(= 4611686018427387904 4611686018427387904.0)", boolean(true)},
        {"(= 1 1 1.0)", boolean(true)},
        {"(= 1 1 2)", boolean(false)},
    });
}

BOOST_AUTO_TEST_CASE(ordering) {
    check_cases({
        {"(< 1 2)", boolean(true)},
        {"(< 2 1)", boolean(false)},
        {"(< 1 1)", boolean(false)},
        {"(<= 1 1)", boolean(true)},
        {"(<= 2 2.0)", boolean(true)},
        {"(> 2.5 2)", boolean(true)},
        {"(>= 1 2)", boolean(false)},
        {"(>= 2 2)", boolean(true)},
        {"(< -0.0 0.0)", boolean(false)},
        {"(< 4611686018427387904 4611686018427387905)", boolean(true)},
        {"(> min64 max64)", boolean(false)},
        {"(< -inf.0 min64)", boolean(true)},
        {"(> +inf.0 max64)", boolean(true)},
        {"(< 1 2 3)", boolean(true)},
        {"(< 1 3 2)", boolean(false)},
        {"(>= 3 3 1)", boolean(true)},
    });
}

BOOST_AUTO_TEST_CASE(nan_ordering_is_false) {
    check_cases({
        {"(< +nan.0 1)", boolean(false)},
        {"(> +nan.0 1)", boolean(false)},
        {"(<= +nan.0 +nan.0)", boolean(false)},
        {"(>= +nan.0 +nan.0)", boolean(false)},
        {"(< 1 +nan.0)", boolean(false)},
    });
}

/**
 * modulo (floored, sign of divisor) and remainder (truncated, sign of dividend)
 */

BOOST_AUTO_TEST_CASE(modulo_fixnum) {
    check_cases({
        {"(modulo 7 3)", fix(1)},
        {"(modulo -7 3)", fix(2)},
        {"(modulo 7 -3)", fix(-2)},
        {"(modulo -7 -3)", fix(-1)},
        {"(modulo 6 3)", fix(0)},
        {"(modulo min64 -1)", fix(0)},
        {"(modulo 5 0)", raises("modulo: division by zero")},
        {"(modulo 5.0 0)", raises("modulo: division by zero")},
        {"(modulo 5.0 0.0)", flo(std::numeric_limits<double>::quiet_NaN())},
        {"(modulo 'a 2)", raises("modulo: arguments must be numbers")},
    });
}

BOOST_AUTO_TEST_CASE(modulo_flonum) {
    check_cases({
        {"(modulo 7.5 2)", flo(1.5)},
        {"(modulo -7.5 2)", flo(0.5)},
        {"(modulo 7.5 -2)", flo(-0.5)},
        {"(modulo -7.5 -2)", flo(-1.5)},
        {"(modulo -7.0 2.0)", flo(1.0)},
        {"(modulo 7 2.0)", flo(1.0)},
        {"(modulo 6.0 3.0)", flo(0.0)},
        {"(modulo -6.0 3.0)", flo(0.0)},
        {"(modulo 6.0 -3.0)", flo(-0.0)},
    });
}

BOOST_AUTO_TEST_CASE(remainder_fixnum) {
    check_cases({
        {"(remainder 7 3)", fix(1)},
        {"(remainder -7 3)", fix(-1)},
        {"(remainder 7 -3)", fix(1)},
        {"(remainder -7 -3)", fix(-1)},
        {"(remainder min64 -1)", fix(0)},
        {"(remainder 5 0)", raises("remainder: division by zero")},
        {"(remainder 5.0 0)", raises("remainder: division by zero")},
        {"(remainder 5 0.0)", flo(std::numeric_limits<double>::quiet_NaN())},
    });
}

BOOST_AUTO_TEST_CASE(remainder_flonum) {
    check_cases({
        {"(remainder 5.0 3.0)", flo(2.0)},   ///< std::remainder gave -1.0
        {"(remainder -7.5 2)", flo(-1.5)},
        {"(remainder 7.5 -2)", flo(1.5)},
        {"(remainder 6.0 3.0)", flo(0.0)},
        {"(remainder -6.0 3.0)", flo(-0.0)},
    });
}

BOOST_AUTO_TEST_CASE(abs_and_negation) {
    check_cases({
        {"(abs -5)", fix(5)},
        {"(abs 5)", fix(5)},
        {"(abs -2.5)", flo(2.5)},
        {"(abs -0.0)", flo(0.0)},
        {"(- -2.5)", flo(2.5)},
    });
}

/**
 * Constant folding must agree with the runtime for literal operands
 */

BOOST_AUTO_TEST_CASE(folded_literals_match_runtime) {
    /// Optimised mode folds these at compile time; the other modes compute them at run time
    check_cases({
        {"(+ 2 3)", fix(5)},
        {"(* 2 (+ 1 2 3))", fix(12)},
        {"(/ 7 2)", flo(3.5)},
        {"(/ 8 2)", fix(4)},
        {"(- 9223372036854775807 -1)", flo(kTwo63)},
        {"(* 3037000500 3037000500)", flo(3037000500.0 * 3037000500.0)},
        {"(+ 0.5 0.25)", flo(0.75)},
        {"(- 0.0 0.0)", flo(0.0)},
        {"(* -1.0 0.0)", flo(-0.0)},
        {"(/ 1 0)", raises("/: division by zero")},
        {"(/ 1.0 0.0)", flo(std::numeric_limits<double>::infinity())},
    });
}

/**
 * The DAP sandbox evaluator must agree with the runtime on immediate operands
 */

BOOST_AUTO_TEST_CASE(sandbox_matches_runtime) {
    constexpr std::string_view exprs[] = {
        "(+ 1 2 3)", "(- 10 3 2)", "(* 2 3 4)", "(/ 7 2 2)", "(/ 6 3)", "(/ 2)", "(/ 1)",
        "(- 5)", "(- 0.0)", "(+ -0.0 -0.0)", "(+ 1 2.5)", "(* 2 0.5)",
        "(* 70368744177663 70368744177663)",   ///< overflows int64: promotes to flonum
        "(/ 1.0 0.0)", "(/ -1 0.0)", "(/ 0.0 0.0)", "(/ 0.0)",
        "(= 1 1.0)", "(= 0.0 -0.0)", "(< 1 1.5)", "(>= 2 2.0)", "(> -1 -1.5)",
    };

    for (const auto expr : exprs) {
        BOOST_TEST_CONTEXT(expr) {
            Session runtime;
            const Outcome expected = runtime.run(
                "(module m " + std::string(kPrelude) + "(define result " + std::string(expr) + "))",
                Mode::Primitive);

            memory::heap::Heap heap{1024 * 1024};
            memory::intern::InternTable intern_table;
            Sandbox sandbox(heap, intern_table, [](const std::string&, LispVal&) { return false; });
            const auto r = sandbox.eval(std::string(expr));
            BOOST_REQUIRE_MESSAGE(r.ok(), "sandbox error: " << r.error);

            Outcome actual;
            NumericValue n{};
            if (r.value == True || r.value == False) {
                actual = boolean(r.value == True);
            } else if (numeric_ops::classify_immediate(r.value, n)) {
                actual = n.is_fixnum() ? fix(n.int_val) : flo(n.float_val);
            }
            BOOST_TEST(same(expected, actual), "runtime " << expected << ", sandbox " << actual);
        }
    }

    /// Exact zero divisors are errors in both
    memory::heap::Heap heap{1024 * 1024};
    memory::intern::InternTable intern_table;
    Sandbox sandbox(heap, intern_table, [](const std::string&, LispVal&) { return false; });
    for (const auto expr : {"(/ 1 0)", "(/ 1.0 0)", "(/ 0)"}) {
        BOOST_TEST_CONTEXT(expr) {
            const auto r = sandbox.eval(expr);
            BOOST_TEST(!r.ok());
            BOOST_TEST(r.error.find("division by zero") != std::string::npos);
        }
    }
}

/**
 * Automatic differentiation: TapeRefs through Add/Mul chains and NumCmp
 */

BOOST_AUTO_TEST_CASE(tape_gradient_through_arith_chain) {
    /// d/dx (x + x + x) = 3 and d/dx (x * x * x) = 3x^2 = 12 at x = 2
    for (const Mode mode : kAllModes) {
        BOOST_TEST_CONTEXT(to_string(mode)) {
            const Outcome sum = run(
                "(define t (tape-new)) (define x (tape-var t 2.0)) "
                "(tape-start! t) (define y (+ x x x)) (tape-stop!) "
                "(tape-backward! t y) "
                "(define result (tape-adjoint t x))",
                mode);
            BOOST_TEST(same(flo(3.0), sum), "got " << sum);

            const Outcome cube = run(
                "(define t (tape-new)) (define x (tape-var t 2.0)) "
                "(tape-start! t) (define y (* x x x)) (tape-stop!) "
                "(tape-backward! t y) "
                "(define result (tape-adjoint t x))",
                mode);
            BOOST_TEST(same(flo(12.0), cube), "got " << cube);
        }
    }
}

BOOST_AUTO_TEST_CASE(tape_comparison_compares_primal_values) {
    for (const Mode mode : kAllModes) {
        BOOST_TEST_CONTEXT(to_string(mode)) {
            const Outcome o = run(
                "(set-aad-nondiff-policy! 'zero-subgrad) "
                "(define t (tape-new)) (define x (tape-var t 3.0)) "
                "(tape-start! t) "
                "(define result (if (< x 5.0) (if (> x 2) (if (= x 3) (if (>= 3.0 x) #t #f) #f) #f) #f)) "
                "(tape-stop!)",
                mode);
            BOOST_TEST(same(boolean(true), o), "got " << o);
        }
    }
}

BOOST_AUTO_TEST_CASE(tape_comparison_strict_policy_is_catchable) {
    for (const Mode mode : kAllModes) {
        BOOST_TEST_CONTEXT(to_string(mode)) {
            const Outcome o = run(
                "(set-aad-nondiff-policy! 'strict) "
                "(define t (tape-new)) (define x (tape-var t 3.0)) "
                "(tape-start! t) "
                "(define result (catch (< x 5.0))) "
                "(tape-stop!)",
                mode);
            BOOST_TEST(same(raises("<: non-differentiable point reached in strict mode"), o),
                       "got " << o);
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
