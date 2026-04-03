#pragma once

#include <memory>
#include <unordered_map>
#include <functional>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include "eta/semantics/core_ir.h"
#include "eta/runtime/vm/bytecode.h"
#include "eta/semantics/semantic_analyzer.h"

#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"

namespace eta::semantics {

// Registry to own BytecodeFunction objects with stable addresses.
// Solves the dangling pointer issue when storing BytecodeFunction* in Closures.
// Functions are accessed by index (uint32_t) rather than raw pointers for type safety.
// Thread-safe for concurrent add/get operations.
class BytecodeFunctionRegistry {
public:
    // Adds a BytecodeFunction and returns its index in the registry.
    // Thread-safe: uses exclusive lock.
    uint32_t add(runtime::vm::BytecodeFunction&& func) {
        std::unique_lock lock(mutex_);
        uint32_t idx = static_cast<uint32_t>(functions_.size());
        functions_.push_back(std::move(func));
        return idx;
    }

    // Get a function by index. Returns nullptr if index is out of bounds.
    // Thread-safe: uses shared lock (allows concurrent reads).
    const runtime::vm::BytecodeFunction* get(uint32_t index) const {
        std::shared_lock lock(mutex_);
        if (index < functions_.size()) {
            return &functions_[index];
        }
        return nullptr;
    }

    // Get a mutable function by index.
    // Thread-safe: uses exclusive lock.
    runtime::vm::BytecodeFunction* get_mut(uint32_t index) {
        std::unique_lock lock(mutex_);
        if (index < functions_.size()) {
            return &functions_[index];
        }
        return nullptr;
    }

    // Get all functions (for iteration). Caller must ensure no concurrent modifications.
    // Note: Returns reference - caller should not hold while other threads might modify.
    const std::deque<runtime::vm::BytecodeFunction>& all() const { return functions_; }

    // Thread-safe size query
    std::size_t size() const {
        std::shared_lock lock(mutex_);
        return functions_.size();
    }

private:
    mutable std::shared_mutex mutex_;
    std::deque<runtime::vm::BytecodeFunction> functions_;
};

class Emitter {
public:
    explicit Emitter(const ModuleSemantics& sem, 
                     runtime::memory::heap::Heap& heap,
                     runtime::memory::intern::InternTable& intern_table,
                     BytecodeFunctionRegistry& registry)
        : sem_(sem), heap_(heap), intern_table_(intern_table), registry_(registry) {}

    runtime::vm::BytecodeFunction* emit();

private:
    const ModuleSemantics& sem_;
    runtime::memory::heap::Heap& heap_;
    runtime::memory::intern::InternTable& intern_table_;
    BytecodeFunctionRegistry& registry_;

    struct Context {
        runtime::vm::BytecodeFunction func;
        std::unordered_map<std::string, uint32_t> string_constant_cache;
    };

    void emit_node(const core::Node* node, Context& ctx);
    uint32_t emit_lambda(const core::Lambda& lambda, const std::string& parent_name, const eta::reader::parser::Span& span);

    // Helper to emit load/store operations for different address types
    void emit_address_load(const core::Address& addr, Context& ctx);
    void emit_address_store(const core::Address& addr, Context& ctx);

    // Dedicated emit methods for each IR node type
    uint32_t add_const(runtime::nanbox::LispVal val, Context& ctx);
    uint32_t emit_load_const(runtime::nanbox::LispVal val, Context& ctx);
    void emit_const(const core::Const& n, Context& ctx);
    void emit_var(const core::Var& n, Context& ctx);
    void emit_call(const core::Call& n, bool tail, Context& ctx);
    void emit_if(const core::If& n, Context& ctx);
    void emit_begin(const core::Begin& n, Context& ctx);
    void emit_lambda_node(const core::Lambda& n, const eta::reader::parser::Span& span, Context& ctx);
    void emit_set(const core::Set& n, Context& ctx);
    void emit_values(const core::Values& n, Context& ctx);
    void emit_call_with_values(const core::CallWithValues& n, bool tail, Context& ctx);
    void emit_dynamic_wind(const core::DynamicWind& n, Context& ctx);
    void emit_call_cc(const core::CallCC& n, bool tail, Context& ctx);
    void emit_apply(const core::Apply& n, bool tail, Context& ctx);
    void emit_quote(const core::Quote& n, Context& ctx);
};

} // namespace eta::semantics
