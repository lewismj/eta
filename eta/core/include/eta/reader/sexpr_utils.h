#pragma once

#include "eta/reader/parser.h"
#include <string_view>

namespace eta::reader::utils {

    using parser::SExprPtr;
    using parser::Symbol;
    using parser::List;

    inline const Symbol* as_symbol(const SExprPtr& p) {
        return (p && p->is<Symbol>()) ? p->as<Symbol>() : nullptr;
    }

    inline const List* as_list(const SExprPtr& p) {
        return (p && p->is<List>()) ? p->as<List>() : nullptr;
    }

    inline List* as_list(SExprPtr& p) {
        return (p && p->is<List>()) ? p->as<List>() : nullptr;
    }

    inline List* as_list_mut(SExprPtr& p) {
        return (p && p->is<List>()) ? p->as<List>() : nullptr;
    }

    inline bool is_symbol_named(const SExprPtr& p, std::string_view name) {
        if (const auto* s = as_symbol(p)) {
            return s->name == name;
        }
        return false;
    }

} // namespace eta::reader::utils
