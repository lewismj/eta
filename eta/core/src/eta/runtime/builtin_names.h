#pragma once

/**
 * @file builtin_names.h
 * @brief Analysis-only builtin registration for the LSP and other tools
 *        that need to know builtin names/arities but do NOT execute code.
 *
 * Registers every builtin that the runtime provides (core + port + io) into
 * a BuiltinEnvironment using null PrimitiveFuncs.  The SemanticAnalyzer only
 * reads names/arities from the env to pre-allocate global slots — it never
 * calls the funcs — so null is safe here.
 *
 * Registration order MUST stay in sync with the three runtime headers:
 *   core_primitives.h  →  port_primitives.h  →  io_primitives.h
 * because the order determines the global-slot indices used by emitted IR.
 */

#include "eta/runtime/builtin_env.h"

namespace eta::runtime {

inline void register_builtin_names(BuiltinEnvironment& env) {
    // Helper: register with a null func (analysis-only, never installed)
    auto r = [&env](const char* name, uint32_t arity, bool has_rest) {
        env.register_builtin(name, arity, has_rest, PrimitiveFunc{});
    };

    // ====================================================================
    // core_primitives.h  (must match registration order exactly)
    // ====================================================================

    // Arithmetic
    r("+",  0, true);
    r("-",  1, true);
    r("*",  0, true);
    r("/",  1, true);

    // Numeric comparison
    r("=",  2, true);
    r("<",  2, true);
    r(">",  2, true);
    r("<=", 2, true);
    r(">=", 2, true);

    // Equivalence
    r("eq?",  2, false);
    r("eqv?", 2, false);
    r("not",  1, false);

    // Pairs / lists
    r("cons",  2, false);
    r("car",   1, false);
    r("cdr",   1, false);
    r("pair?", 1, false);
    r("null?", 1, false);
    r("list",  0, true);

    // Type predicates
    r("number?",    1, false);
    r("boolean?",   1, false);
    r("string?",    1, false);
    r("char?",      1, false);
    r("symbol?",    1, false);
    r("procedure?", 1, false);
    r("integer?",   1, false);

    // Numeric predicates
    r("zero?",     1, false);
    r("positive?", 1, false);
    r("negative?", 1, false);

    // Numeric operations
    r("abs",       1, false);
    r("min",       2, true);
    r("max",       2, true);
    r("modulo",    2, false);
    r("remainder", 2, false);

    // Transcendentals
    r("sin",  1, false);
    r("cos",  1, false);
    r("tan",  1, false);
    r("asin", 1, false);
    r("acos", 1, false);
    r("atan", 1, true);
    r("exp",  1, false);
    r("log",  1, false);
    r("sqrt", 1, false);

    // List operations
    r("length",   1, false);
    r("append",   0, true);
    r("reverse",  1, false);
    r("list-ref", 2, false);

    // Higher-order
    r("apply",    2, true);
    r("map",      2, false);
    r("for-each", 2, false);

    // Deep equality
    r("equal?", 2, false);

    // String operations
    r("string-length",   1, false);
    r("string-append",   0, true);
    r("number->string",  1, false);
    r("string->number",  1, false);

    // Vector operations
    r("vector",        0, true);
    r("vector-length", 1, false);
    r("vector-ref",    2, false);
    r("vector-set!",   3, false);
    r("vector?",       1, false);
    r("make-vector",   2, false);

    // Misc
    r("error",    1, true);
    r("platform", 0, false);
    r("logic-var?", 1, false);

    // ====================================================================
    // port_primitives.h  (must match registration order exactly)
    // ====================================================================

    r("current-input-port",       0, false);
    r("current-output-port",      0, false);
    r("current-error-port",       0, false);
    r("set-current-input-port!",  1, false);
    r("set-current-output-port!", 1, false);
    r("set-current-error-port!",  1, false);
    r("open-output-string",       0, false);
    r("get-output-string",        1, false);
    r("open-input-string",        1, false);
    r("write-string",             1, true);
    r("read-char",                0, true);
    r("port?",                    1, false);
    r("input-port?",              1, false);
    r("output-port?",             1, false);
    r("close-port",               1, false);
    r("open-input-file",          1, false);
    r("open-output-file",         1, false);
    r("open-output-bytevector",   0, false);
    r("open-input-bytevector",    1, false);
    r("get-output-bytevector",    1, false);
    r("read-u8",                  0, true);
    r("write-u8",                 1, true);
    r("binary-port?",             1, false);

    // ====================================================================
    // io_primitives.h  (must match registration order exactly)
    // ====================================================================

    r("display", 1, true);
    r("write",   1, true);
    r("newline", 0, true);
}

} // namespace eta::runtime

