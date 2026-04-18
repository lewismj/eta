#pragma once

#include <expected>
#include <string>
#include <vector>

#include "eta/runtime/nanbox.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"

namespace eta::runtime::clp {

struct LinearTerm {
    memory::heap::ObjectId var_id = 0;
    double coef = 0.0;

    [[nodiscard]] bool operator<(const LinearTerm& other) const noexcept {
        return var_id < other.var_id;
    }
};

struct LinearExpr {
    std::vector<LinearTerm> terms;
    double constant = 0.0;

    void canonicalize();
};

struct LinearizeErrorInfo {
    std::string tag;
    std::string message;
    std::vector<memory::heap::ObjectId> offending_vars;
};

std::expected<LinearExpr, LinearizeErrorInfo>
linearize(nanbox::LispVal term,
          memory::heap::Heap& heap,
          memory::intern::InternTable& intern_table);

} ///< namespace eta::runtime::clp
