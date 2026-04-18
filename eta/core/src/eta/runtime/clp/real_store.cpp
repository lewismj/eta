#include "eta/runtime/clp/real_store.h"

#include <algorithm>

namespace eta::runtime::clp {

void RealStore::truncate(std::size_t new_size) noexcept {
    if (new_size < entries_.size()) {
        entries_.resize(new_size);
    }
}

void RealStore::append_leq(LinearExpr expr) {
    expr.canonicalize();
    auto vars = collect_vars(expr);
    entries_.push_back(RealConstraintEntry{
        .relation = RealRelation::Leq,
        .expr = std::move(expr),
        .vars = std::move(vars),
    });
}

void RealStore::append_eq(LinearExpr expr) {
    expr.canonicalize();
    auto vars = collect_vars(expr);
    entries_.push_back(RealConstraintEntry{
        .relation = RealRelation::Eq,
        .expr = std::move(expr),
        .vars = std::move(vars),
    });
}

FMSystem RealStore::to_fm_system() const {
    FMSystem sys;
    sys.leq.reserve(entries_.size());
    sys.eq.reserve(entries_.size());
    for (const auto& e : entries_) {
        if (e.relation == RealRelation::Leq) {
            sys.leq.push_back(e.expr);
        } else {
            sys.eq.push_back(e.expr);
        }
    }
    return sys;
}

std::vector<memory::heap::ObjectId> RealStore::participating_vars() const {
    std::vector<memory::heap::ObjectId> out;
    for (const auto& e : entries_) {
        out.insert(out.end(), e.vars.begin(), e.vars.end());
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<memory::heap::ObjectId> RealStore::collect_vars(const LinearExpr& expr) {
    std::vector<memory::heap::ObjectId> vars;
    vars.reserve(expr.terms.size());
    for (const auto& t : expr.terms) {
        if (vars.empty() || vars.back() != t.var_id) vars.push_back(t.var_id);
    }
    return vars;
}

} // namespace eta::runtime::clp
