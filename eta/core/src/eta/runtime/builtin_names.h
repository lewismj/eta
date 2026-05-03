#pragma once

/**
 * @file builtin_names.h
 * @brief Analysis-only builtin registration for the LSP and other tools
 *        that need to know builtin names/arities but do NOT execute code.
 *
 * Registers every builtin that the runtime provides (core + port + io + os +
 * process + time + torch + stats + log + nng) into a BuiltinEnvironment using null PrimitiveFuncs.
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
    r("pow",  2, false);

    /// AAD non-differentiability policy controls
    r("set-aad-nondiff-policy!", 1, false);
    r("aad-nondiff-policy",      0, false);

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

    /// CSV operations
    r("%csv-open-reader",        2, false);
    r("%csv-reader-from-string", 2, false);
    r("%csv-columns",            1, false);
    r("%csv-read-row",           1, false);
    r("%csv-read-record",        1, false);
    r("%csv-read-typed-row",     1, false);
    r("%csv-close",              1, false);
    r("%csv-open-writer",        2, false);
    r("%csv-write-row",          2, false);
    r("%csv-write-record",       3, false);
    r("%csv-flush",              1, false);
    r("%fact-table-load-csv",    2, false);
    r("%fact-table-save-csv",    3, false);
    r("%csv-reader?",            1, false);
    r("%csv-writer?",            1, false);

    /// Regex operations
    r("%regex-compile",     2, false);
    r("%regex?",            1, false);
    r("%regex-pattern",     1, false);
    r("%regex-flags",       1, false);
    r("%regex-match?",      2, false);
    r("%regex-search",      3, false);
    r("%regex-find-all",    2, false);
    r("%regex-replace",     3, false);
    r("%regex-replace-fn",  3, false);
    r("%regex-split",       2, false);
    r("%regex-quote",       1, false);
    r("%regex-match?-str",  2, false);
    r("%regex-search-str",  3, false);
    r("%regex-find-all-str", 2, false);
    r("%regex-replace-str", 3, false);
    r("%regex-split-str",   2, false);
    r("%regex-cache-stats", 0, false);
    r("%regex-cache-reset!", 0, false);

    /// JSON operations
    r("%json-read",         2, false);
    r("%json-read-string",  2, false);
    r("%json-write",        2, false);
    r("%json-write-string", 1, false);

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

    /// Hash-map / hash-set primitives
    r("hash-map",           0, true);
    r("make-hash-map",      0, false);
    r("hash-map?",          1, false);
    r("hash-map-ref",       2, true);
    r("hash-map-assoc",     3, false);
    r("hash-map-dissoc",    2, false);
    r("hash-map-keys",      1, false);
    r("hash-map-values",    1, false);
    r("hash-map-size",      1, false);
    r("hash-map->list",     1, false);
    r("list->hash-map",     1, false);
    r("hash-map-fold",      3, false);
    r("hash",               1, false);
    r("make-hash-set",      0, false);
    r("hash-set",           0, true);
    r("hash-set?",          1, false);
    r("hash-set-add",       2, false);
    r("hash-set-remove",    2, false);
    r("hash-set-contains?", 2, false);
    r("hash-set-union",     2, false);
    r("hash-set-intersect", 2, false);
    r("hash-set-diff",      2, false);
    r("hash-set->list",     1, false);
    r("list->hash-set",     1, false);

    /// Atom primitives
    r("%atom-new",              1, false);
    r("%atom?",                 1, false);
    r("%atom-deref",            1, false);
    r("%atom-reset!",           2, false);
    r("%atom-compare-and-set!", 3, false);
    r("%atom-swap!",            2, true);

    /// Misc
    r("error",      1, true);
    r("platform",   0, false);
    r("logic-var?", 1, false);
    r("register-finalizer!",   2, false);
    r("unregister-finalizer!", 1, false);
    r("make-guardian",         0, false);
    r("guardian-track!",       2, false);
    r("guardian-collect",      1, false);
    /**
     * register_builtin calls in core_primitives.h immediately after
     * `guardian-collect` and before `logic-var/named` (patch-mode validates
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
    r("%clp-r-qp-minimize", 1, false);
    r("%clp-r-qp-maximize", 1, false);

    /**
     * CLP(FD) native bounds propagators.
     * Must stay in the same order as the register_builtin calls in
     * core_primitives.h immediately after %clp-r-qp-maximize.
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
    r("tape-clear!",     1, false);
    r("tape-var",        2, false);
    r("tape-backward!",  2, false);
    r("tape-adjoint",    2, false);
    r("tape-primal",     2, false);
    r("tape-ref?",       1, false);
    r("tape-ref-index",  1, false);
    r("tape-size",       1, false);
    r("tape-ref-value-of", 2, false);
    r("tape-ref-value",  1, false);

    /// Fact-table builtins (must match core_primitives.h registration order)
    r("%fact-table?",            1, false);
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
    r("%fact-table-group-count", 2, false);
    r("%fact-table-group-sum",   3, false);
    r("%fact-table-live-row-ids", 1, false);
    r("%fact-table-ref",         3, false);
    r("%fact-table-row-count",   1, false);
    r("%fact-table-column-names", 1, false);
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
    r("%stats-normal-quantile",  1, false);
    r("%stats-ci",               2, false);
    r("%stats-t-test-2",         2, false);
    r("%stats-ols",              2, false);

    /**
     * register_builtin calls at the END of register_core_primitives in
     * core_primitives.h (immediately before port_primitives runs).
     */
    r("register-prop-attr!",   1, false);
    r("%clp-prop-queue-size",  0, false);
    r("eval",                  1, false);

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
     * os_primitives.h  (must match registration order exactly)
     */

    r("getenv",                  1, false);
    r("setenv!",                 2, false);
    r("unsetenv!",               1, false);
    r("environment-variables",   0, false);
    r("command-line-arguments",  0, false);
    r("exit",                    0, true);
    r("current-directory",       0, false);
    r("change-directory!",       1, false);
    r("file-exists?",            1, false);
    r("directory?",              1, false);
    r("delete-file",             1, false);
    r("make-directory",          1, false);
    r("list-directory",          1, false);
    r("path-join",               1, true);
    r("path-split",              1, false);
    r("path-normalize",          1, false);
    r("temp-file",               0, false);
    r("temp-directory",          0, false);
    r("file-modification-time",  1, false);
    r("file-size",               1, false);

    /**
     * process_primitives.h  (must match registration order exactly)
     */

    r("%process-run",          2, true);
    r("%process-spawn",        2, true);
    r("%process-wait",         1, true);
    r("%process-kill",         1, false);
    r("%process-terminate",    1, false);
    r("%process-pid",          1, false);
    r("%process-alive?",       1, false);
    r("%process-exit-code",    1, false);
    r("%process-handle?",      1, false);
    r("%process-stdin-port",   1, false);
    r("%process-stdout-port",  1, false);
    r("%process-stderr-port",  1, false);

    /**
     * time_primitives.h  (must match registration order exactly)
     */

    r("%time-now-ms",             0, false);
    r("%time-now-us",             0, false);
    r("%time-now-ns",             0, false);
    r("%time-monotonic-ms",       0, false);
    r("%time-sleep-ms",           1, false);
    r("%time-utc-parts",          1, false);
    r("%time-local-parts",        1, false);
    r("%time-format-iso8601-utc", 1, false);
    r("%time-format-iso8601-local", 1, false);

    /**
     * torch_primitives.h  (must match registration order exactly)
     */

    /// Tensor creation
    r("torch/tensor",    1, true);
    r("torch/ones",      1, false);
    r("torch/zeros",     1, false);
    r("torch/randn",     1, false);
    r("torch/manual-seed", 1, false);
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
    r("torch/cholesky",  1, false);
    r("torch/matrix-exp", 1, false);
    r("torch/mvnormal",  2, false);

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
    r("torch/fact-table->tensor", 2, false);
    r("torch/column-l2-norm", 2, false);

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
     * log_primitives.h  (must match registration order exactly)
     */
    r("%log-make-stdout-sink",        2, false);
    r("%log-make-file-sink",          2, false);
    r("%log-make-rotating-sink",      3, false);
    r("%log-make-daily-sink",         4, false);
    r("%log-make-port-sink",          1, false);
    r("%log-make-current-error-sink", 0, false);
    r("%log-make-logger",             2, false);
    r("%log-get-logger",              1, false);
    r("%log-default-logger",          0, false);
    r("%log-set-default!",            1, false);
    r("%log-set-level!",              2, false);
    r("%log-level",                   1, false);
    r("%log-set-global-level!",       1, false);
    r("%log-set-pattern!",            2, false);
    r("%log-set-formatter!",          2, false);
    r("%log-flush!",                  1, false);
    r("%log-flush-on!",               2, false);
    r("%log-emit",                    4, false);
    r("%log-shutdown!",               0, false);

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

