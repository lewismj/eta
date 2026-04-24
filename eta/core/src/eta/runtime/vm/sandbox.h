#pragma once

/**
 * Sandbox evaluator
 *
 * Tree-walking evaluator over a strictly limited, side-effect-free subset
 * of Eta surface syntax. Designed to satisfy the DAP server's need to
 * evaluate watch / hover / breakpoint-condition / setVariable RHS
 * expressions against a paused VM frame's environment WITHOUT entering
 * the bytecode interpreter on the live VM stack (which would corrupt
 * frames_, fp_, pc_, and trail_stack_).
 *
 * Safety guarantees
 *  - Never executes a bytecode opcode against the paused VM.
 *  - Reads of locals / upvalues / globals are obtained through the
 *    caller-supplied `Lookup` callback only; the sandbox never indexes
 *    into VM internals directly.
 *  - Allocates no heap objects in v1 (cons / list / string-append are
 *    deliberately excluded). The shared `Heap&` is held only so primitive
 *    accessors like `car` / `cdr` can dereference Cons cells already
 *    reachable from the VM.
 *  - Refuses anything outside its allow-list with `SandboxViolation`,
 *    matching the runtime tag installed in `eta::runtime::error`.
 */

#include <expected>
#include <functional>
#include <string>

#include "eta/runtime/error.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/nanbox.h"

namespace eta::runtime::vm {

struct SandboxResult {
    /// Success: the evaluated value (NaN-boxed). Otherwise unspecified.
    nanbox::LispVal value{nanbox::Nil};
    /// Empty on success; otherwise human-readable error text.
    std::string     error;
    /// True when `error` is the result of a SandboxViolation.
    bool            violation{false};
    /// Convenience: success iff `error` is empty.
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

class Sandbox {
public:
    /**
     * Lookup callback invoked for every free identifier appearing in the
     * source expression. Implementations should mirror DAP's frame
     * resolution order (locals -> upvalues -> module globals -> short-name
     * globals) and return false on miss.
     */
    using Lookup = std::function<bool(const std::string& name,
                                      nanbox::LispVal& out_value)>;

    Sandbox(memory::heap::Heap& heap,
            memory::intern::InternTable& intern_table,
            Lookup lookup);

    /// Evaluate one Eta s-expression supplied as source text.
    [[nodiscard]] SandboxResult eval(const std::string& expr_text);

private:
    memory::heap::Heap&          heap_;
    memory::intern::InternTable& intern_table_;
    Lookup                       lookup_;
};

} ///< namespace eta::runtime::vm

