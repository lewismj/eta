#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "eta/runtime/builtin_env.h"

namespace eta::runtime {

/**
 * @brief Specification for one extension primitive.
 *
 * Extension primitives share the same callable contract as core builtins.
 */
using ExtensionSpec = BuiltinSpec;

/**
 * @brief Runtime environment for extension primitive registrations.
 *
 * Extension primitive slots are allocated immediately after core builtin slots.
 */
class ExtensionEnvironment {
public:
    /**
     * @brief Register an extension primitive descriptor.
     *
     * @param name Scheme-visible symbol name.
     * @param arity Minimum required argument count.
     * @param has_rest True when variadic arguments are accepted.
     * @param func Primitive implementation.
     */
    void register_extension(std::string name,
                            uint32_t arity,
                            bool has_rest,
                            PrimitiveFunc func) {
        specs_.push_back(ExtensionSpec{std::move(name), arity, has_rest, std::move(func)});
    }

    /**
     * @brief Install extension primitives into VM globals.
     *
     * Primitive objects are written to slots `[start_slot, start_slot + N)`.
     *
     * @param heap Heap used for primitive allocation.
     * @param globals VM globals vector.
     * @param total_globals Total globals required by current semantic result.
     * @param start_slot First slot index reserved for extension primitives.
     * @return error if primitive allocation fails.
     */
    std::expected<void, RuntimeError> install(Heap& heap,
                                              std::vector<LispVal>& globals,
                                              std::size_t total_globals,
                                              std::size_t start_slot) const {
        if (globals.size() < total_globals) {
            globals.resize(total_globals, Nil);
        }
        for (std::size_t i = 0; i < specs_.size(); ++i) {
            const auto& spec = specs_[i];
            auto prim = make_primitive(heap, spec.func, spec.arity, spec.has_rest);
            if (!prim) return std::unexpected(prim.error());
            auto* prim_obj = heap.try_get_as<ObjectKind::Primitive, Primitive>(ops::payload(*prim));
            if (prim_obj) prim_obj->debug_name = spec.name;
            const std::size_t slot = start_slot + i;
            if (globals.size() <= slot) globals.resize(slot + 1, Nil);
            globals[slot] = *prim;
        }
        return {};
    }

    /// Remove all registered extension primitives.
    void clear() { specs_.clear(); }

    [[nodiscard]] const std::vector<ExtensionSpec>& specs() const { return specs_; }
    [[nodiscard]] std::size_t size() const { return specs_.size(); }

    /**
     * @brief Compute a deterministic hash over extension primitive descriptors.
     *
     * The hash includes each descriptor's symbol name, arity, and variadic flag
     * in registration order. Primitive function pointers are intentionally not
     * included so equal extension surfaces produce the same fingerprint.
     *
     * @return 0 when no extension primitives are registered.
     */
    [[nodiscard]] std::uint64_t fingerprint() const noexcept {
        if (specs_.empty()) return 0;

        constexpr std::uint64_t kFNVOffsetBasis = 14695981039346656037ull;
        constexpr std::uint64_t kFNVPrime = 1099511628211ull;

        auto mix_byte = [](std::uint64_t hash, std::uint8_t byte) noexcept {
            hash ^= static_cast<std::uint64_t>(byte);
            hash *= kFNVPrime;
            return hash;
        };

        std::uint64_t hash = kFNVOffsetBasis;
        for (const auto& spec : specs_) {
            for (const unsigned char ch : spec.name) {
                hash = mix_byte(hash, static_cast<std::uint8_t>(ch));
            }
            hash = mix_byte(hash, 0xFFu);
            for (std::uint32_t i = 0; i < 4u; ++i) {
                hash = mix_byte(
                    hash,
                    static_cast<std::uint8_t>((spec.arity >> (i * 8u)) & 0xFFu));
            }
            hash = mix_byte(hash, static_cast<std::uint8_t>(spec.has_rest ? 1u : 0u));
            hash = mix_byte(hash, 0x00u);
        }
        return hash;
    }

    /// Look up an extension primitive by name. Returns slot offset or nullopt.
    [[nodiscard]] std::optional<std::size_t> lookup(std::string_view name) const {
        for (std::size_t i = 0; i < specs_.size(); ++i) {
            if (specs_[i].name == name) return i;
        }
        return std::nullopt;
    }

private:
    std::vector<ExtensionSpec> specs_;
};

} // namespace eta::runtime
