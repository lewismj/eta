#pragma once

/**
 * @file builtin_names.h
 * @brief Analysis-only builtin registration for the LSP and other tools
 *        that need to know builtin names/arities but do NOT execute code.
 *
 * Registers every builtin that the runtime provides (core + port + io +
 * torch + stats + nng) into a BuiltinEnvironment using null PrimitiveFuncs.
 * The SemanticAnalyzer only reads names/arities from the env to pre-allocate
 *
 * REGISTRATION ORDER must stay in sync with all_primitives.h / driver.h:
 */

#include "eta/runtime/builtin_env.h"

namespace eta::runtime {

inline void register_builtin_names(BuiltinEnvironment& env) {
    /// Helper: register with a null func (analysis-only, never installed)
    auto r = [&env](const char* name, uint32_t arity, bool has_rest) {
        env.register_builtin(name, arity, has_rest, PrimitiveFunc{});
    };

    /**
     * core_primitives.h  (must match registration order exactly)
     */

    /// Arithmetic
    r("+",  0, true);
    r("-",  1, true);
    r("*",  0, true);
    r("/",  1, true);

    /// Numeric comparison
    r("=",  2, true);
    r("<",  2, true);
    r(">",  2, true);
    r("<=", 2, true);
    r(">=", 2, true);

    /// Equivalence
    r("eq?",  2, false);
    r("eqv?", 2, false);
    r("not",  1, false);

    /// Pairs / lists
    r("cons",  2, false);
    r("car",   1, false);
    r("cdr",   1, false);
    r("pair?", 1, false);
    r("null?", 1, false);
    r("list",  0, true);

    /// Type predicates
    r("number?",    1, false);
    r("boolean?",   1, false);
    r("string?",    1, false);
    r("char?",      1, false);
    r("symbol?",    1, false);
    r("procedure?", 1, false);
    r("integer?",   1, false);

    /// Numeric predicates
    r("zero?",     1, false);
    r("positive?", 1, false);
    r("negative?", 1, false);

    /// Numeric operations
    r("abs",       1, false);
    r("min",       2, true);
    r("max",       2, true);
    r("modulo",    2, false);
    r("remainder", 2, false);

    /// Transcendentals
    r("sin",  1, false);
    r("cos",  1, false);
    r("tan",  1, false);
    r("asin", 1, false);
    r("acos", 1, false);
    r("atan", 1, true);
    r("exp",  1, false);
    r("log",  1, false);
    r("sqrt", 1, false);

    /// List operations
    r("length",   1, false);
    r("append",   0, true);
    r("reverse",  1, false);
    r("list-ref", 2, false);
    r("list-tail", 2, false);
    r("set-car!",  2, false);
    r("set-cdr!",  2, false);
    r("assq",      2, false);
    r("assoc",     2, false);
    r("member",    2, false);

    /// Symbol / string interop
    r("symbol->string", 1, false);
    r("string->symbol", 1, false);

    /// Higher-order
    r("apply",    2, true);
    r("map",      2, false);
    r("for-each", 2, false);

    /// Deep equality
    r("equal?", 2, false);

    /// String operations
    r("string-length",   1, false);
    r("string-append",   0, true);
    r("number->string",  1, false);
    r("string->number",  1, false);
    r("string-ref",      2, false);
    r("substring",       3, false);
    r("string=?",        2, false);
    r("string<?",        2, false);
    r("string>?",        2, false);
    r("string<=?",       2, false);
    r("string>=?",       2, false);

    /// Char operations
    r("char->integer",   1, false);
    r("integer->char",   1, false);

    /// Vector operations
    r("vector",        0, true);
    r("vector-length", 1, false);
    r("vector-ref",    2, false);
    r("vector-set!",   3, false);
    r("vector?",       1, false);
    r("make-vector",   2, false);

    /// Misc
    r("error",      1, true);
    r("platform",   0, false);
    r("logic-var?", 1, false);
    /**
     * register_builtin calls in core_primitives.h immediately after
     * `logic-var?` and before `logic-var/named` (patch-mode validates
     * name/arity per slot).
     */
    r("put-attr",            3, false);
    r("get-attr",            2, false);
    r("del-attr",            2, false);
    r("attr-var?",           1, false);
    r("register-attr-hook!", 2, false);
    r("logic-var/named", 1, false);
    r("var-name", 1, false);
    r("set-occurs-check!", 1, false);
    r("occurs-check-mode", 0, false);
    r("ground?",    1, false);

    /// Compound terms
    r("compound?", 1, false);
    r("term",      1, true);
    r("functor",   1, false);
    r("arity",     1, false);
    r("arg",       2, false);

    /// AD Dual number primitives
    r("dual?",          1, false);
    r("dual-primal",    1, false);
    r("dual-backprop",  1, false);
    r("make-dual",      2, false);

    r("%clp-domain-z!",  3, false);
    r("%clp-domain-fd!", 2, false);
    r("%clp-domain-r!",  5, false);
    r("%clp-get-domain", 1, false);
    r("%clp-linearize",  1, false);
    r("%clp-fm-feasible?", 1, true);
    r("%clp-fm-bounds",    2, true);
    r("%clp-r-post-leq!",  2, false);
    r("%clp-r-post-eq!",   2, false);
    r("%clp-r-propagate!", 0, false);
    r("%clp-r-minimize",   1, false);
    r("%clp-r-maximize",   1, false);

    /**
     * CLP(FD) native bounds propagators.
     * Must stay in the same order as the register_builtin calls in
     * core_primitives.h immediately after %clp-r-maximize.
     */
    r("%clp-fd-plus!",         3, false);
    r("%clp-fd-plus-offset!",  3, false);
    r("%clp-fd-abs!",          2, false);
    r("%clp-fd-times!",           3, false);
    r("%clp-fd-sum!",             2, false);
    r("%clp-fd-scalar-product!",  3, false);
    r("%clp-fd-element!",         3, false);
    r("%clp-fd-all-different!",   1, false);

    /**
     * CLP(B) native Boolean propagators.
     * Order must match the register_builtin calls in core_primitives.h
     * immediately after %clp-fd-all-different!.
     */
    r("%clp-bool-and!",  3, false);
    r("%clp-bool-or!",   3, false);
    r("%clp-bool-xor!",  3, false);
    r("%clp-bool-imp!",  3, false);
    r("%clp-bool-eq!",   3, false);
    r("%clp-bool-not!",  2, false);
    r("%clp-bool-card!", 3, false);

    /// AD Tape primitives (tape-based reverse-mode AD)
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

    /// Fact-table builtins (must match core_primitives.h registration order)
    r("fact-table?",             1, false);
    r("%make-fact-table",        1, false);
    r("%fact-table-insert!",     2, false);
    r("%fact-table-insert-clause!", 4, false);
    r("%fact-table-delete-row!", 2, false);
    r("%fact-table-row-live?",   2, false);
    r("%fact-table-row-ground?", 2, false);
    r("%fact-table-row-rule",    2, false);
    r("%fact-table-set-predicate!", 3, false);
    r("%fact-table-predicate",   1, false);
    r("%fact-table-build-index!", 2, false);
    r("%fact-table-query",       3, false);
    r("%fact-table-live-row-ids", 1, false);
    r("%fact-table-ref",         3, false);
    r("%fact-table-row-count",   1, false);
    r("term-hash",               2, false);
    r("term-variant-hash",       2, false);

    /// Statistics builtins (must match core_primitives.h registration order)
    r("%stats-mean",             1, false);
    r("%stats-variance",         1, false);
    r("%stats-stddev",           1, false);
    r("%stats-sem",              1, false);
    r("%stats-percentile",       2, false);
    r("%stats-covariance",       2, false);
    r("%stats-correlation",      2, false);
    r("%stats-t-cdf",            2, false);
    r("%stats-t-quantile",       2, false);
    r("%stats-ci",               2, false);
    r("%stats-t-test-2",         2, false);
    r("%stats-ols",              2, false);

    /**
     * register_builtin calls at the END of register_core_primitives in
     * core_primitives.h (immediately before port_primitives runs).
     */
    r("register-prop-attr!",   1, false);
    r("%clp-prop-queue-size",  0, false);

    /**
     * port_primitives.h  (must match registration order exactly)
     */

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

    /**
     * io_primitives.h  (must match registration order exactly)
     */

    r("display", 1, true);
    r("write",   1, true);
    r("newline", 0, true);

    /**
     * torch_primitives.h  (must match registration order exactly)
     */

    /// Tensor creation
    r("torch/tensor",    1, true);
    r("torch/ones",      1, false);
    r("torch/zeros",     1, false);
    r("torch/randn",     1, false);
    r("torch/arange",    3, false);
    r("torch/linspace",  3, false);
    r("torch/from-list", 1, false);

    /// Predicates
    r("torch/tensor?",   1, false);

    /// Arithmetic
    r("torch/add",       2, false);
    r("torch/sub",       2, false);
    r("torch/mul",       2, false);
    r("torch/div",       2, false);
    r("torch/matmul",    2, false);
    r("torch/dot",       2, false);

    /// Unary
    r("torch/neg",       1, false);
    r("torch/abs",       1, false);
    r("torch/exp",       1, false);
    r("torch/log",       1, false);
    r("torch/sqrt",      1, false);
    r("torch/relu",      1, false);
    r("torch/sigmoid",   1, false);
    r("torch/tanh",      1, false);
    r("torch/softmax",   2, false);

    /// Shape
    r("torch/shape",     1, false);
    r("torch/reshape",   2, false);
    r("torch/transpose", 3, false);
    r("torch/squeeze",   1, false);
    r("torch/unsqueeze", 2, false);
    r("torch/cat",       2, false);

    /// Reductions
    r("torch/sum",       1, false);
    r("torch/mean",      1, false);
    r("torch/max",       1, false);
    r("torch/min",       1, false);
    r("torch/argmax",    1, false);
    r("torch/argmin",    1, false);

    /// Conversion
    r("torch/item",      1, false);
    r("torch/to-list",   1, false);
    r("torch/numel",     1, false);

    /// Autograd
    r("torch/requires-grad!",  2, false);
    r("torch/requires-grad?",  1, false);
    r("torch/detach",          1, false);
    r("torch/backward",        1, false);
    r("torch/grad",            1, false);
    r("torch/zero-grad!",      1, false);

    /// NN layers
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

    /// Loss
    r("nn/mse-loss",            2, false);
    r("nn/l1-loss",             2, false);
    r("nn/cross-entropy-loss",  2, false);

    /// Optimizers
    r("optim/sgd",              2, false);
    r("optim/adam",             2, false);
    r("optim/step!",            1, false);
    r("optim/zero-grad!",       1, false);
    r("optim/optimizer?",       1, false);

    /// Serialization / IO
    r("torch/save",             2, false);
    r("torch/load",             1, false);
    r("torch/print",            1, false);

    /// Device management
    r("torch/cuda-available?",    0, false);
    r("torch/cuda-device-count",  0, false);
    r("torch/device",             1, false);
    r("torch/to-device",          2, false);
    r("nn/to-device",             2, false);

    /**
     * stats_primitives.h  (must match registration order exactly)
     */
    r("%stats-mean-vec",     1, true);
    r("%stats-var-vec",      1, true);
    r("%stats-cov-matrix",   1, true);
    r("%stats-cor-matrix",   1, true);
    r("%stats-quantile-vec", 2, true);
    r("%stats-ols-multi",    2, true);

    /**
     * nng_primitives.h  (must match registration order exactly)
     */
    r("nng-socket",          1, false);
    r("nng-listen",          2, false);
    r("nng-dial",            2, false);
    r("nng-close",           1, false);
    r("nng-socket?",         1, false);
    r("send!",               2, true);
    r("recv!",               1, true);
    r("nng-poll",            2, false);
    r("nng-subscribe",       2, false);
    r("nng-set-option",      3, false);
    r("spawn",               1, true);
    r("spawn-kill",          1, false);
    r("spawn-wait",          1, false);
    r("current-mailbox",     0, false);
    r("spawn-thread-with",   2, true);
    r("spawn-thread",        1, false);
    r("thread-join",         1, false);
    r("thread-alive?",       1, false);
    r("monitor",             1, false);
    r("demonitor",           1, false);
    r("enable-heartbeat",    2, false);
}

} ///< namespace eta::runtime

