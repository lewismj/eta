#pragma once

#include <memory>
#include <unordered_map>
#include <functional>
#include "eta/semantics/core_ir.h"
#include "eta/runtime/vm/bytecode.h"
#include "eta/semantics/semantic_analyzer.h"

#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"

namespace eta::semantics {

class Emitter {
public:
    explicit Emitter(const ModuleSemantics& sem, 
                     runtime::memory::heap::Heap& heap,
                     runtime::memory::intern::InternTable& intern_table) 
        : sem_(sem), heap_(heap), intern_table_(intern_table) {}

    std::vector<runtime::vm::BytecodeFunction> emit();

private:
    const ModuleSemantics& sem_;
    runtime::memory::heap::Heap& heap_;
    runtime::memory::intern::InternTable& intern_table_;
    
    struct BindingIdHash {
        size_t operator()(const core::BindingId& b) const {
            return std::hash<uint32_t>{}(b.id);
        }
    };

    struct Context {
        runtime::vm::BytecodeFunction func;
        std::unordered_map<core::BindingId, uint32_t, BindingIdHash> binding_to_slot;
        std::unordered_map<core::BindingId, uint32_t, BindingIdHash> binding_to_upval;
    };

    void emit_node(const core::Node* node, Context& ctx);
    void emit_lambda(const core::Lambda& lambda, std::vector<runtime::vm::BytecodeFunction>& out);
};

} // namespace eta::semantics
