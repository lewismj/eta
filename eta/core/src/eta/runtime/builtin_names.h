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
    r("error",      1, true);
    r("platform",   0, false);
    r("logic-var?", 1, false);
    r("ground?",    1, false);

    // AD Dual number primitives
    r("dual?",          1, false);
    r("dual-primal",    1, false);
    r("dual-backprop",  1, false);
    r("make-dual",      2, false);

    // CLP domain primitives (internal — consumed by std.clp)
    r("%clp-domain-z!",  3, false);
    r("%clp-domain-fd!", 2, false);
    r("%clp-get-domain", 1, false);

    // AD Tape primitives (tape-based reverse-mode AD)
    r("tape-new",        0, false);
    r("tape-start!",     1, false);
    r("tape-stop!",      0, false);
    r("tape-var",        2, false);
    r("tape-backward!",  2, false);
    r("tape-adjoint",    2, false);
    r("tape-primal",     2, false);
    r("tape-ref?",       1, false);
    r("tape-ref-index",  1, false);
    r("tape-size",       1, false);
    r("tape-ref-value",  1, false);

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
    r("close-input-port",         1, false);
    r("close-output-port",        1, false);
    r("write-char",               1, true);
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

#ifdef ETA_HAS_NNG
    // ====================================================================
    // nng_primitives.h  (must match registration order exactly)
    //
    // These are always registered when ETA_HAS_NNG is defined, even in
    // analysis-only mode (LSP), so that nng/* names resolve and get
    // correct arity checking.  The actual implementations are linked
    // only when a live VM is involved (interpreter, DAP).
    // ====================================================================
    r("nng-socket",     1, false);
    r("nng-listen",     2, false);
    r("nng-dial",       2, false);
    r("nng-close",      1, false);
    r("nng-socket?",    1, false);
    r("send!",          2, true);
    r("recv!",          1, true);
    r("nng-poll",       2, false);
    r("nng-subscribe",  2, false);
    r("nng-set-option", 3, false);
#endif // ETA_HAS_NNG

#ifdef ETA_HAS_TORCH
    // ====================================================================
    // torch_primitives.h  (must match registration order exactly)
    //
    // These are always registered when ETA_HAS_TORCH is defined, even in
    // analysis-only mode (LSP), so that torch/* names resolve and get
    // correct arity checking.  The actual implementations are linked
    // only when a live VM is involved (interpreter, DAP).
    // ====================================================================

    // Tensor creation
    r("torch/tensor",    1, true);
    r("torch/ones",      1, false);
    r("torch/zeros",     1, false);
    r("torch/randn",     1, false);
    r("torch/arange",    3, false);
    r("torch/linspace",  3, false);
    r("torch/from-list", 1, false);

    // Predicates
    r("torch/tensor?",   1, false);

    // Arithmetic
    r("torch/add",       2, false);
    r("torch/sub",       2, false);
    r("torch/mul",       2, false);
    r("torch/div",       2, false);
    r("torch/matmul",    2, false);
    r("torch/dot",       2, false);

    // Unary
    r("torch/neg",       1, false);
    r("torch/abs",       1, false);
    r("torch/exp",       1, false);
    r("torch/log",       1, false);
    r("torch/sqrt",      1, false);
    r("torch/relu",      1, false);
    r("torch/sigmoid",   1, false);
    r("torch/tanh",      1, false);
    r("torch/softmax",   2, false);

    // Shape
    r("torch/shape",     1, false);
    r("torch/reshape",   2, false);
    r("torch/transpose", 3, false);
    r("torch/squeeze",   1, false);
    r("torch/unsqueeze", 2, false);
    r("torch/cat",       2, false);

    // Reductions
    r("torch/sum",       1, false);
    r("torch/mean",      1, false);
    r("torch/max",       1, false);
    r("torch/min",       1, false);
    r("torch/argmax",    1, false);
    r("torch/argmin",    1, false);

    // Conversion
    r("torch/item",      1, false);
    r("torch/to-list",   1, false);
    r("torch/numel",     1, false);

    // Autograd
    r("torch/requires-grad!",  2, false);
    r("torch/requires-grad?",  1, false);
    r("torch/detach",          1, false);
    r("torch/backward",        1, false);
    r("torch/grad",            1, false);
    r("torch/zero-grad!",      1, false);

    // NN layers
    r("nn/linear",              2, false);
    r("nn/sequential",          0, true);
    r("nn/relu-layer",          0, false);
    r("nn/sigmoid-layer",       0, false);
    r("nn/dropout",             1, false);
    r("nn/forward",             2, false);
    r("nn/parameters",          1, false);
    r("nn/train!",              1, false);
    r("nn/eval!",               1, false);
    r("nn/module?",             1, false);

    // Loss
    r("nn/mse-loss",            2, false);
    r("nn/l1-loss",             2, false);
    r("nn/cross-entropy-loss",  2, false);

    // Optimizers
    r("optim/sgd",              2, false);
    r("optim/adam",             2, false);
    r("optim/step!",            1, false);
    r("optim/zero-grad!",       1, false);
    r("optim/optimizer?",       1, false);

    // Serialization / IO
    r("torch/save",             2, false);
    r("torch/load",             1, false);
    r("torch/print",            1, false);

    // Device management
    r("torch/cuda-available?",    0, false);
    r("torch/cuda-device-count",  0, false);
    r("torch/device",             1, false);
    r("torch/to-device",          2, false);
    r("nn/to-device",             2, false);
#endif // ETA_HAS_TORCH
}

} // namespace eta::runtime

