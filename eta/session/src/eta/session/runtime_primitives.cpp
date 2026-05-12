/**
 * @file runtime_primitives.cpp
 * @brief Runtime primitive installation and naming helpers.
 */

#include "eta/session/runtime_primitives.h"

#include "eta/runtime/factory.h"
#include "eta/runtime/types/primitive.h"

namespace eta::session {

std::size_t RuntimePrimitiveInstaller::builtin_count() const noexcept {
    return builtins_.specs().size();
}

std::size_t RuntimePrimitiveInstaller::total_primitive_count() const noexcept {
    return builtins_.specs().size() + extensions_.size();
}

std::expected<void, runtime::error::RuntimeError> RuntimePrimitiveInstaller::install_into(
    std::vector<runtime::nanbox::LispVal>& globals,
    std::size_t total_globals) {
    if (globals.size() < total_globals) {
        globals.resize(total_globals, runtime::nanbox::Nil);
    }

    for (std::size_t i = 0; i < builtins_.specs().size(); ++i) {
        const auto& spec = builtins_.specs()[i];
        auto prim = runtime::memory::factory::make_primitive(
            heap_, spec.func, spec.arity, spec.has_rest);
        if (!prim) return std::unexpected(prim.error());
        auto* prim_obj = heap_.try_get_as<
            runtime::memory::heap::ObjectKind::Primitive,
            runtime::types::Primitive>(runtime::nanbox::ops::payload(*prim));
        if (prim_obj) prim_obj->debug_name = spec.name;
        globals[i] = *prim;
    }

    auto extension_install = extensions_.install(
        heap_, globals, total_globals, builtins_.specs().size());
    if (!extension_install) return std::unexpected(extension_install.error());

    installed_ = true;
    return {};
}

void RuntimePrimitiveInstaller::record_names(
    std::unordered_map<uint32_t, std::string>& global_names) const {
    std::size_t slot = 0;
    for (const auto& spec : builtins_.specs()) {
        if (!spec.name.empty()) {
            global_names[static_cast<uint32_t>(slot)] = spec.name;
        }
        ++slot;
    }
    for (const auto& spec : extensions_.specs()) {
        if (!spec.name.empty()) {
            global_names[static_cast<uint32_t>(slot)] = spec.name;
        }
        ++slot;
    }
}

} // namespace eta::session

