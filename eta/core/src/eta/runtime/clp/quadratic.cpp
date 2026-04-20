#include "eta/runtime/clp/quadratic.h"

#include <Eigen/Eigenvalues>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "eta/runtime/numeric_value.h"
#include "eta/runtime/types/compound.h"
#include "eta/runtime/types/cons.h"
#include "eta/runtime/types/logic_var.h"

namespace eta::runtime::clp {
namespace {

using nanbox::LispVal;
using nanbox::Tag;
using memory::heap::Heap;
using memory::heap::ObjectId;
using memory::heap::ObjectKind;
using memory::intern::InternId;
using memory::intern::InternTable;

/**
 * Keep recursion depth comfortably below default Windows test-runner stack
 * limits so depth-exceeded errors are reported instead of stack overflow.
 */
constexpr std::size_t kMaxDepth = 256;

enum class ArithmeticOp : std::uint8_t {
    Add,
    Sub,
    Mul,
};

struct OperatorIds {
    InternId add = 0;
    InternId sub = 0;
    InternId mul = 0;
};

struct DecodedCall {
    ArithmeticOp op = ArithmeticOp::Add;
    std::vector<LispVal> args;
};

QuadraticLinearizeErrorInfo make_error(std::string tag,
                                       std::string message,
                                       std::vector<ObjectId> offending_vars = {}) {
    return QuadraticLinearizeErrorInfo{
        .tag = std::move(tag),
        .message = std::move(message),
        .offending_vars = std::move(offending_vars),
    };
}

std::string symbol_name(LispVal v, InternTable& intern_table) {
    if (!nanbox::ops::is_boxed(v) || nanbox::ops::tag(v) != Tag::Symbol) {
        return "<non-symbol>";
    }
    auto s = intern_table.get_string(nanbox::ops::payload(v));
    if (!s) return "<invalid-symbol>";
    return std::string(*s);
}

std::string describe_atom(LispVal v, InternTable& intern_table) {
    if (v == nanbox::Nil) return "'()";
    if (v == nanbox::True) return "#t";
    if (v == nanbox::False) return "#f";
    if (!nanbox::ops::is_boxed(v)) return "<flonum>";

    switch (nanbox::ops::tag(v)) {
        case Tag::Fixnum: return "<fixnum>";
        case Tag::Char: return "<char>";
        case Tag::String: return "<string>";
        case Tag::Symbol: return "'" + symbol_name(v, intern_table);
        case Tag::Nan: return "<nan>";
        case Tag::HeapObject: return "<heap-object>";
        case Tag::TapeRef: return "<tape-ref>";
        case Tag::Nil: break;
    }
    return "<atom>";
}

std::expected<OperatorIds, QuadraticLinearizeErrorInfo>
load_operator_ids(InternTable& intern_table) {
    auto plus = intern_table.intern("+");
    if (!plus) {
        return std::unexpected(make_error(
            "clp.qp.linearize.type-error",
            "failed to intern '+' operator symbol"));
    }
    auto minus = intern_table.intern("-");
    if (!minus) {
        return std::unexpected(make_error(
            "clp.qp.linearize.type-error",
            "failed to intern '-' operator symbol"));
    }
    auto mul = intern_table.intern("*");
    if (!mul) {
        return std::unexpected(make_error(
            "clp.qp.linearize.type-error",
            "failed to intern '*' operator symbol"));
    }
    return OperatorIds{
        .add = *plus,
        .sub = *minus,
        .mul = *mul,
    };
}

std::expected<LispVal, QuadraticLinearizeErrorInfo>
deref_term(LispVal term, Heap& heap) {
    std::unordered_set<ObjectId> seen;
    std::size_t depth = 0;
    LispVal v = term;
    for (;;) {
        if (++depth > kMaxDepth) {
            return std::unexpected(make_error(
                "clp.qp.linearize.depth-exceeded",
                "dereference depth exceeded limit"));
        }
        if (!nanbox::ops::is_boxed(v) || nanbox::ops::tag(v) != Tag::HeapObject) {
            return v;
        }
        const ObjectId id = nanbox::ops::payload(v);
        auto* lv = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id);
        if (!lv || !lv->binding.has_value()) return v;
        if (!seen.insert(id).second) {
            return std::unexpected(make_error(
                "clp.qp.linearize.depth-exceeded",
                "cycle detected while dereferencing logic variables"));
        }
        v = *lv->binding;
    }
}

QuadraticExpr expr_add(QuadraticExpr lhs, const QuadraticExpr& rhs) {
    lhs.constant += rhs.constant;
    lhs.linear_terms.insert(lhs.linear_terms.end(),
                            rhs.linear_terms.begin(),
                            rhs.linear_terms.end());
    lhs.quadratic_terms.insert(lhs.quadratic_terms.end(),
                               rhs.quadratic_terms.begin(),
                               rhs.quadratic_terms.end());
    return lhs;
}

QuadraticExpr expr_sub(QuadraticExpr lhs, const QuadraticExpr& rhs) {
    lhs.constant -= rhs.constant;
    lhs.linear_terms.reserve(lhs.linear_terms.size() + rhs.linear_terms.size());
    lhs.quadratic_terms.reserve(lhs.quadratic_terms.size() + rhs.quadratic_terms.size());
    for (const auto& t : rhs.linear_terms) {
        lhs.linear_terms.push_back(LinearTerm{
            .var_id = t.var_id,
            .coef = -t.coef,
        });
    }
    for (const auto& t : rhs.quadratic_terms) {
        lhs.quadratic_terms.push_back(QuadraticTerm{
            .var_i = t.var_i,
            .var_j = t.var_j,
            .coef = -t.coef,
        });
    }
    return lhs;
}

QuadraticExpr expr_scale(QuadraticExpr expr, double k) {
    if (k == 0.0) {
        expr.linear_terms.clear();
        expr.quadratic_terms.clear();
        expr.constant = 0.0;
        return expr;
    }
    expr.constant *= k;
    for (auto& t : expr.linear_terms) t.coef *= k;
    for (auto& t : expr.quadratic_terms) t.coef *= k;
    return expr;
}

bool is_constant_expr(const QuadraticExpr& expr) {
    return expr.linear_terms.empty() && expr.quadratic_terms.empty();
}

bool is_affine_expr(const QuadraticExpr& expr) {
    return expr.quadratic_terms.empty();
}

std::vector<ObjectId> collect_offending_vars(const QuadraticExpr& lhs,
                                             const QuadraticExpr& rhs) {
    std::vector<ObjectId> ids;
    ids.reserve(lhs.linear_terms.size() + rhs.linear_terms.size() +
                (lhs.quadratic_terms.size() + rhs.quadratic_terms.size()) * 2);
    for (const auto& t : lhs.linear_terms) ids.push_back(t.var_id);
    for (const auto& t : rhs.linear_terms) ids.push_back(t.var_id);
    for (const auto& t : lhs.quadratic_terms) {
        ids.push_back(t.var_i);
        ids.push_back(t.var_j);
    }
    for (const auto& t : rhs.quadratic_terms) {
        ids.push_back(t.var_i);
        ids.push_back(t.var_j);
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

std::expected<QuadraticExpr, QuadraticLinearizeErrorInfo>
expr_mul(QuadraticExpr lhs, QuadraticExpr rhs) {
    if (is_constant_expr(lhs)) {
        return expr_scale(std::move(rhs), lhs.constant);
    }
    if (is_constant_expr(rhs)) {
        return expr_scale(std::move(lhs), rhs.constant);
    }

    if (!is_affine_expr(lhs) || !is_affine_expr(rhs)) {
        return std::unexpected(make_error(
            "clp.qp.linearize.non-quadratic",
            "multiplication exceeds quadratic objective degree",
            collect_offending_vars(lhs, rhs)));
    }

    QuadraticExpr out;
    out.constant = lhs.constant * rhs.constant;

    out.linear_terms.reserve(lhs.linear_terms.size() + rhs.linear_terms.size());
    for (const auto& t : lhs.linear_terms) {
        out.linear_terms.push_back(LinearTerm{
            .var_id = t.var_id,
            .coef = t.coef * rhs.constant,
        });
    }
    for (const auto& t : rhs.linear_terms) {
        out.linear_terms.push_back(LinearTerm{
            .var_id = t.var_id,
            .coef = t.coef * lhs.constant,
        });
    }

    out.quadratic_terms.reserve(lhs.linear_terms.size() * rhs.linear_terms.size());
    for (const auto& lt : lhs.linear_terms) {
        for (const auto& rt : rhs.linear_terms) {
            out.quadratic_terms.push_back(QuadraticTerm{
                .var_i = lt.var_id,
                .var_j = rt.var_id,
                .coef = lt.coef * rt.coef,
            });
        }
    }
    out.canonicalize();
    return out;
}

std::expected<ArithmeticOp, QuadraticLinearizeErrorInfo>
decode_operator_symbol(LispVal op_term,
                       const OperatorIds& op_ids,
                       InternTable& intern_table) {
    if (!nanbox::ops::is_boxed(op_term) || nanbox::ops::tag(op_term) != Tag::Symbol) {
        return std::unexpected(make_error(
            "clp.qp.linearize.type-error",
            "arithmetic call operator must be a symbol"));
    }
    const auto sid = static_cast<InternId>(nanbox::ops::payload(op_term));
    if (sid == op_ids.add) return ArithmeticOp::Add;
    if (sid == op_ids.sub) return ArithmeticOp::Sub;
    if (sid == op_ids.mul) return ArithmeticOp::Mul;
    return std::unexpected(make_error(
        "clp.qp.linearize.unknown-atom",
        "unsupported operator " + describe_atom(op_term, intern_table)));
}

std::expected<std::vector<LispVal>, QuadraticLinearizeErrorInfo>
collect_cons_args(LispVal cursor,
                  Heap& heap,
                  const std::unordered_set<ObjectId>& active_stack) {
    std::vector<LispVal> args;
    std::unordered_set<ObjectId> seen_cells;
    while (cursor != nanbox::Nil) {
        if (!nanbox::ops::is_boxed(cursor) || nanbox::ops::tag(cursor) != Tag::HeapObject) {
            return std::unexpected(make_error(
                "clp.qp.linearize.type-error",
                "arithmetic call arguments must be in a proper list"));
        }
        const ObjectId cid = nanbox::ops::payload(cursor);
        if (active_stack.contains(cid) || !seen_cells.insert(cid).second) {
            return std::unexpected(make_error(
                "clp.qp.linearize.depth-exceeded",
                "cycle detected while traversing cons call arguments"));
        }
        auto* cell = heap.try_get_as<ObjectKind::Cons, types::Cons>(cid);
        if (!cell) {
            return std::unexpected(make_error(
                "clp.qp.linearize.type-error",
                "arithmetic call arguments must be in a proper list"));
        }
        args.push_back(cell->car);
        cursor = cell->cdr;
    }
    return args;
}

std::expected<DecodedCall, QuadraticLinearizeErrorInfo>
decode_call(LispVal term,
            Heap& heap,
            InternTable& intern_table,
            const OperatorIds& op_ids,
            const std::unordered_set<ObjectId>& active_stack) {
    if (!nanbox::ops::is_boxed(term) || nanbox::ops::tag(term) != Tag::HeapObject) {
        return std::unexpected(make_error(
            "clp.qp.linearize.type-error",
            "expected arithmetic call term"));
    }

    const ObjectId id = nanbox::ops::payload(term);
    if (auto* ct = heap.try_get_as<ObjectKind::CompoundTerm, types::CompoundTerm>(id)) {
        auto op = decode_operator_symbol(ct->functor, op_ids, intern_table);
        if (!op) return std::unexpected(op.error());
        return DecodedCall{
            .op = *op,
            .args = ct->args,
        };
    }

    if (auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(id)) {
        auto op = decode_operator_symbol(cons->car, op_ids, intern_table);
        if (!op) return std::unexpected(op.error());
        auto args = collect_cons_args(cons->cdr, heap, active_stack);
        if (!args) return std::unexpected(args.error());
        return DecodedCall{
            .op = *op,
            .args = std::move(*args),
        };
    }

    return std::unexpected(make_error(
        "clp.qp.linearize.type-error",
        "expected a compound term or cons call"));
}

class ActiveNodeGuard {
public:
    ActiveNodeGuard(std::unordered_set<ObjectId>& active_stack, ObjectId id)
        : active_stack_(active_stack), id_(id), inserted_(active_stack_.insert(id).second) {}

    ~ActiveNodeGuard() {
        if (inserted_) active_stack_.erase(id_);
    }

    [[nodiscard]] bool inserted() const noexcept {
        return inserted_;
    }

private:
    std::unordered_set<ObjectId>& active_stack_;
    ObjectId id_;
    bool inserted_;
};

std::expected<QuadraticExpr, QuadraticLinearizeErrorInfo>
linearize_rec(LispVal term,
              Heap& heap,
              InternTable& intern_table,
              const OperatorIds& op_ids,
              std::unordered_set<ObjectId>& active_stack,
              std::size_t depth);

std::expected<QuadraticExpr, QuadraticLinearizeErrorInfo>
linearize_call(const DecodedCall& call,
               Heap& heap,
               InternTable& intern_table,
               const OperatorIds& op_ids,
               std::unordered_set<ObjectId>& active_stack,
               std::size_t depth) {
    switch (call.op) {
        case ArithmeticOp::Add: {
            QuadraticExpr acc;
            for (const auto& arg : call.args) {
                auto part = linearize_rec(arg, heap, intern_table, op_ids, active_stack, depth + 1);
                if (!part) return std::unexpected(part.error());
                part->canonicalize();
                acc = expr_add(std::move(acc), *part);
                acc.canonicalize();
            }
            return acc;
        }
        case ArithmeticOp::Sub: {
            if (call.args.empty()) {
                return std::unexpected(make_error(
                    "clp.qp.linearize.type-error",
                    "operator '-' expects at least one argument"));
            }
            auto first = linearize_rec(call.args[0], heap, intern_table, op_ids, active_stack, depth + 1);
            if (!first) return std::unexpected(first.error());
            first->canonicalize();
            if (call.args.size() == 1) {
                auto neg = expr_scale(std::move(*first), -1.0);
                neg.canonicalize();
                return neg;
            }
            QuadraticExpr acc = std::move(*first);
            for (std::size_t i = 1; i < call.args.size(); ++i) {
                auto part = linearize_rec(call.args[i], heap, intern_table, op_ids, active_stack, depth + 1);
                if (!part) return std::unexpected(part.error());
                part->canonicalize();
                acc = expr_sub(std::move(acc), *part);
                acc.canonicalize();
            }
            return acc;
        }
        case ArithmeticOp::Mul: {
            QuadraticExpr acc;
            acc.constant = 1.0;
            for (const auto& arg : call.args) {
                auto part = linearize_rec(arg, heap, intern_table, op_ids, active_stack, depth + 1);
                if (!part) return std::unexpected(part.error());
                part->canonicalize();
                auto next = expr_mul(std::move(acc), std::move(*part));
                if (!next) return std::unexpected(next.error());
                acc = std::move(*next);
                acc.canonicalize();
            }
            return acc;
        }
    }
    return std::unexpected(make_error(
        "clp.qp.linearize.type-error",
        "unknown arithmetic operator"));
}

std::expected<QuadraticExpr, QuadraticLinearizeErrorInfo>
linearize_rec(LispVal term,
              Heap& heap,
              InternTable& intern_table,
              const OperatorIds& op_ids,
              std::unordered_set<ObjectId>& active_stack,
              std::size_t depth) {
    if (depth > kMaxDepth) {
        return std::unexpected(make_error(
            "clp.qp.linearize.depth-exceeded",
            "expression nesting depth exceeded limit"));
    }

    auto deref = deref_term(term, heap);
    if (!deref) return std::unexpected(deref.error());
    LispVal v = *deref;

    const auto num = classify_numeric(v, heap);
    if (num.is_valid()) {
        QuadraticExpr expr;
        expr.constant = num.as_double();
        return expr;
    }

    if (nanbox::ops::is_boxed(v) && nanbox::ops::tag(v) == Tag::HeapObject) {
        const ObjectId id = nanbox::ops::payload(v);
        if (auto* lv = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id)) {
            if (!lv->binding.has_value()) {
                QuadraticExpr expr;
                expr.linear_terms.push_back(LinearTerm{
                    .var_id = id,
                    .coef = 1.0,
                });
                return expr;
            }
            return std::unexpected(make_error(
                "clp.qp.linearize.type-error",
                "internal dereference error: expected unbound logic variable"));
        }

        if (heap.try_get_as<ObjectKind::Cons, types::Cons>(id) ||
            heap.try_get_as<ObjectKind::CompoundTerm, types::CompoundTerm>(id)) {
            ActiveNodeGuard guard(active_stack, id);
            if (!guard.inserted()) {
                return std::unexpected(make_error(
                    "clp.qp.linearize.depth-exceeded",
                    "cycle detected while traversing expression term"));
            }
            auto call = decode_call(v, heap, intern_table, op_ids, active_stack);
            if (!call) return std::unexpected(call.error());
            return linearize_call(*call, heap, intern_table, op_ids, active_stack, depth);
        }
    }

    return std::unexpected(make_error(
        "clp.qp.linearize.unknown-atom",
        "unsupported atom " + describe_atom(v, intern_table)));
}

} // namespace

void QuadraticExpr::canonicalize() {
    for (auto& qt : quadratic_terms) {
        if (qt.var_j < qt.var_i) std::swap(qt.var_i, qt.var_j);
    }
    std::sort(quadratic_terms.begin(), quadratic_terms.end());

    std::vector<QuadraticTerm> merged_quadratic;
    merged_quadratic.reserve(quadratic_terms.size());
    for (const auto& t : quadratic_terms) {
        if (!merged_quadratic.empty() &&
            merged_quadratic.back().var_i == t.var_i &&
            merged_quadratic.back().var_j == t.var_j) {
            merged_quadratic.back().coef += t.coef;
        } else {
            merged_quadratic.push_back(t);
        }
    }

    std::vector<QuadraticTerm> filtered_quadratic;
    filtered_quadratic.reserve(merged_quadratic.size());
    for (const auto& t : merged_quadratic) {
        if (t.coef != 0.0) filtered_quadratic.push_back(t);
    }
    quadratic_terms = std::move(filtered_quadratic);

    std::sort(linear_terms.begin(), linear_terms.end());

    std::vector<LinearTerm> merged_linear;
    merged_linear.reserve(linear_terms.size());
    for (const auto& t : linear_terms) {
        if (!merged_linear.empty() && merged_linear.back().var_id == t.var_id) {
            merged_linear.back().coef += t.coef;
        } else {
            merged_linear.push_back(t);
        }
    }

    std::vector<LinearTerm> filtered_linear;
    filtered_linear.reserve(merged_linear.size());
    for (const auto& t : merged_linear) {
        if (t.coef != 0.0) filtered_linear.push_back(t);
    }
    linear_terms = std::move(filtered_linear);

    if (constant == 0.0) constant = 0.0;
}

std::expected<QuadraticExpr, QuadraticLinearizeErrorInfo>
linearize_quadratic_objective(LispVal term, Heap& heap, InternTable& intern_table) {
    auto op_ids = load_operator_ids(intern_table);
    if (!op_ids) return std::unexpected(op_ids.error());

    std::unordered_set<ObjectId> active_stack;
    auto expr = linearize_rec(term, heap, intern_table, *op_ids, active_stack, 0);
    if (!expr) return std::unexpected(expr.error());

    expr->canonicalize();
    return *expr;
}

namespace {

QuadraticModelErrorInfo make_model_error(std::string tag,
                                         std::string message,
                                         std::vector<ObjectId> offending_vars = {}) {
    return QuadraticModelErrorInfo{
        .tag = std::move(tag),
        .message = std::move(message),
        .offending_vars = std::move(offending_vars),
    };
}

[[nodiscard]] bool add_checked(double& dst, double delta) noexcept {
    dst += delta;
    return std::isfinite(dst);
}

[[nodiscard]] bool valid_convexity_config(const QuadraticConvexityConfig& cfg) noexcept {
    if (!std::isfinite(cfg.abs_tol) || cfg.abs_tol < 0.0) return false;
    if (!std::isfinite(cfg.rel_tol) || cfg.rel_tol < 0.0) return false;
    if (!std::isfinite(cfg.jitter_abs) || cfg.jitter_abs < 0.0) return false;
    if (!std::isfinite(cfg.jitter_rel) || cfg.jitter_rel < 0.0) return false;
    return true;
}

} // namespace

std::expected<QuadraticObjectiveMatrix, QuadraticModelErrorInfo>
materialize_quadratic_objective_matrix(const QuadraticExpr& expr) {
    QuadraticExpr canonical = expr;
    canonical.canonicalize();

    if (!std::isfinite(canonical.constant)) {
        return std::unexpected(make_model_error(
            "clp.qp.numeric-failure",
            "objective constant is not finite"));
    }

    std::vector<ObjectId> vars;
    vars.reserve(canonical.linear_terms.size() + canonical.quadratic_terms.size() * 2);

    for (const auto& t : canonical.linear_terms) {
        if (!std::isfinite(t.coef)) {
            return std::unexpected(make_model_error(
                "clp.qp.numeric-failure",
                "objective linear coefficient is not finite",
                {t.var_id}));
        }
        vars.push_back(t.var_id);
    }
    for (const auto& t : canonical.quadratic_terms) {
        if (!std::isfinite(t.coef)) {
            return std::unexpected(make_model_error(
                "clp.qp.numeric-failure",
                "objective quadratic coefficient is not finite",
                {t.var_i, t.var_j}));
        }
        vars.push_back(t.var_i);
        vars.push_back(t.var_j);
    }

    std::sort(vars.begin(), vars.end());
    vars.erase(std::unique(vars.begin(), vars.end()), vars.end());

    QuadraticObjectiveMatrix out;
    out.vars = std::move(vars);
    out.k = canonical.constant;

    const auto n = out.dim();
    if (n > 0 && n > (std::numeric_limits<std::size_t>::max() / n)) {
        return std::unexpected(make_model_error(
            "clp.qp.numeric-failure",
            "objective matrix dimension overflow"));
    }
    out.c.assign(n, 0.0);
    out.q.assign(n * n, 0.0);

    std::unordered_map<ObjectId, std::size_t> index_of;
    index_of.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        index_of.emplace(out.vars[i], i);
    }

    for (const auto& t : canonical.linear_terms) {
        const auto it = index_of.find(t.var_id);
        if (it == index_of.end()) {
            return std::unexpected(make_model_error(
                "clp.qp.numeric-failure",
                "internal objective index lookup failed",
                {t.var_id}));
        }
        if (!add_checked(out.c[it->second], t.coef)) {
            return std::unexpected(make_model_error(
                "clp.qp.numeric-failure",
                "objective linear accumulation is not finite",
                {t.var_id}));
        }
    }

    for (const auto& t : canonical.quadratic_terms) {
        const auto li = index_of.find(t.var_i);
        const auto ri = index_of.find(t.var_j);
        if (li == index_of.end() || ri == index_of.end()) {
            return std::unexpected(make_model_error(
                "clp.qp.numeric-failure",
                "internal objective index lookup failed",
                {t.var_i, t.var_j}));
        }
        const auto i = li->second;
        const auto j = ri->second;
        if (i == j) {
            if (!add_checked(out.q[i * n + j], 2.0 * t.coef)) {
                return std::unexpected(make_model_error(
                    "clp.qp.numeric-failure",
                    "objective Hessian diagonal accumulation is not finite",
                    {t.var_i}));
            }
            continue;
        }
        if (!add_checked(out.q[i * n + j], t.coef) ||
            !add_checked(out.q[j * n + i], t.coef)) {
            return std::unexpected(make_model_error(
                "clp.qp.numeric-failure",
                "objective Hessian accumulation is not finite",
                {t.var_i, t.var_j}));
        }
    }

    return out;
}

std::expected<void, QuadraticModelErrorInfo>
check_quadratic_convexity(const QuadraticObjectiveMatrix& objective,
                          double hessian_sign,
                          QuadraticConvexityConfig cfg) {
    const auto n = objective.dim();
    if (objective.c.size() != n || objective.q.size() != n * n) {
        return std::unexpected(make_model_error(
            "clp.qp.numeric-failure",
            "objective matrix has inconsistent dimensions"));
    }
    if (!std::isfinite(hessian_sign) || hessian_sign == 0.0) {
        return std::unexpected(make_model_error(
            "clp.qp.numeric-failure",
            "invalid Hessian sign"));
    }
    if (!valid_convexity_config(cfg)) {
        return std::unexpected(make_model_error(
            "clp.qp.numeric-failure",
            "invalid convexity tolerance configuration"));
    }

    if (n == 0) return {};

    Eigen::MatrixXd hessian(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(n));
    double scale = 1.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double ci = objective.c[i];
        if (!std::isfinite(ci)) {
            return std::unexpected(make_model_error(
                "clp.qp.numeric-failure",
                "objective linear vector is not finite",
                {objective.vars[i]}));
        }
        for (std::size_t j = 0; j < n; ++j) {
            const double qij = objective.q_at(i, j);
            if (!std::isfinite(qij)) {
                std::vector<ObjectId> offenders{objective.vars[i]};
                if (objective.vars[i] != objective.vars[j]) {
                    offenders.push_back(objective.vars[j]);
                }
                return std::unexpected(make_model_error(
                    "clp.qp.numeric-failure",
                    "objective Hessian entry is not finite",
                    std::move(offenders)));
            }
            const double signed_entry = qij * hessian_sign;
            if (!std::isfinite(signed_entry)) {
                std::vector<ObjectId> offenders{objective.vars[i]};
                if (objective.vars[i] != objective.vars[j]) {
                    offenders.push_back(objective.vars[j]);
                }
                return std::unexpected(make_model_error(
                    "clp.qp.numeric-failure",
                    "signed objective Hessian entry is not finite",
                    std::move(offenders)));
            }
            scale = (std::max)(scale, std::abs(signed_entry));
            hessian(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = signed_entry;
        }
    }

    hessian = 0.5 * (hessian + hessian.transpose());

    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eig;
    eig.compute(hessian, Eigen::EigenvaluesOnly);
    if (eig.info() != Eigen::Success) {
        return std::unexpected(make_model_error(
            "clp.qp.numeric-failure",
            "failed to compute objective Hessian eigenvalues"));
    }
    if (eig.eigenvalues().size() == 0) return {};

    const double min_eig = eig.eigenvalues().minCoeff();
    if (!std::isfinite(min_eig)) {
        return std::unexpected(make_model_error(
            "clp.qp.numeric-failure",
            "objective Hessian eigenvalues are not finite"));
    }

    const double tol = cfg.abs_tol + (cfg.rel_tol * scale);
    const double jitter_budget = cfg.jitter_abs + (cfg.jitter_rel * scale);
    if (min_eig >= -tol) return {};
    if ((min_eig + jitter_budget) >= -tol) return {};

    std::ostringstream oss;
    oss << "objective Hessian is not positive semidefinite"
        << " (min-eigenvalue=" << min_eig
        << ", tolerance=" << tol
        << ", jitter-budget=" << jitter_budget
        << ")";
    return std::unexpected(make_model_error(
        "clp.qp.non-convex",
        oss.str(),
        objective.vars));
}

} // namespace eta::runtime::clp
