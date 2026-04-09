#pragma once

#include <string>
#include <vector>
#include <expected>
#include <optional>
#include <cstdint>
#include <functional>

#include "eta/runtime/nanbox.h"
#include "eta/runtime/error.h"
#include "eta/runtime/types/primitive.h"
#include "eta/runtime/factory.h"

namespace eta::runtime {

using namespace eta::runtime::nanbox;
using namespace eta::runtime::error;
using namespace eta::runtime::types;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::factory;

/**
 * @brief Specification for a single builtin primitive
 *
 * Shared between the compiler (SemanticAnalyzer) and the runtime (VM).
 * The compiler uses name/arity/has_rest to pre-allocate global slots.
 * The runtime uses func to create Primitive heap objects.
 */
struct BuiltinSpec {
    std::string name;
    uint32_t arity;
    bool has_rest;
    PrimitiveFunc func;
};

/**
 * @brief Shared builtin environment — the compiler↔runtime contract
 *
 * Builtins are registered once, and the ordering determines their global slot
 * indices. The SemanticAnalyzer seeds these as immutable globals (slots 0..N-1),
 * and the VM installs the corresponding Primitive objects at those same slots.
 *
 * Usage:
 *   BuiltinEnvironment builtins;
 *   register_core_primitives(builtins, heap);  // from core_primitives.h
 *
 *   // Compiler side:
 *   sa.analyze_all(forms, linker, builtins);
 *
 *   // Runtime side:
 *   builtins.install(heap, vm.globals(), total_global_count);
 */
class BuiltinEnvironment {
public:
    /**
     * @brief Register a builtin primitive
     * @param name    The Scheme-visible name (e.g. "+", "cons")
     * @param arity   Minimum required argument count
     * @param has_rest Whether the primitive accepts variadic arguments
     * @param func    The C++ implementation
     */
    void register_builtin(std::string name, uint32_t arity, bool has_rest, PrimitiveFunc func) {
        specs_.push_back(BuiltinSpec{std::move(name), arity, has_rest, std::move(func)});
    }

    /**
     * @brief Install all builtins into the VM globals vector
     *
     * Allocates Primitive heap objects and places them at slots 0..N-1
     * in the globals vector. The globals vector is resized to at least
     * total_globals entries.
     *
     * @param heap          Heap for Primitive allocation
     * @param globals       The VM's globals vector (will be resized)
     * @param total_globals Total number of global slots (builtins + user definitions)
     * @return error if any Primitive allocation fails
     */
    std::expected<void, RuntimeError> install(Heap& heap, std::vector<LispVal>& globals, size_t total_globals) const {
        globals.assign(total_globals, Nil);
        for (size_t i = 0; i < specs_.size(); ++i) {
            const auto& spec = specs_[i];
            auto prim = make_primitive(heap, spec.func, spec.arity, spec.has_rest);
            if (!prim) return std::unexpected(prim.error());
            globals[i] = *prim;
        }
        return {};
    }

    [[nodiscard]] const std::vector<BuiltinSpec>& specs() const { return specs_; }
    [[nodiscard]] size_t size() const { return specs_.size(); }

    /// Look up a builtin by name.  Returns its index (global slot) or nullopt.
    [[nodiscard]] std::optional<size_t> lookup(std::string_view name) const {
        for (size_t i = 0; i < specs_.size(); ++i) {
            if (specs_[i].name == name) return i;
        }
        return std::nullopt;
    }

private:
    std::vector<BuiltinSpec> specs_;
};

} // namespace eta::runtime

