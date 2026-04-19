#include "eta/runtime/clp/linear.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <sstream>
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

constexpr std::size_t kMaxDepth = 512;

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

LinearizeErrorInfo make_error(std::string tag,
                              std::string message,
                              std::vector<ObjectId> offending_vars = {}) {
    return LinearizeErrorInfo{
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

std::expected<OperatorIds, LinearizeErrorInfo>
load_operator_ids(InternTable& intern_table) {
    auto plus = intern_table.intern("+");
    if (!plus) {
        return std::unexpected(make_error(
            "clp.linearize.type-error",
            "failed to intern '+' operator symbol"));
    }
    auto minus = intern_table.intern("-");
    if (!minus) {
        return std::unexpected(make_error(
            "clp.linearize.type-error",
            "failed to intern '-' operator symbol"));
    }
    auto mul = intern_table.intern("*");
    if (!mul) {
        return std::unexpected(make_error(
            "clp.linearize.type-error",
            "failed to intern '*' operator symbol"));
    }
    return OperatorIds{
        .add = *plus,
        .sub = *minus,
        .mul = *mul,
    };
}

std::expected<LispVal, LinearizeErrorInfo> deref_term(LispVal term, Heap& heap) {
    std::unordered_set<ObjectId> seen;
    std::size_t depth = 0;
    LispVal v = term;
    for (;;) {
        if (++depth > kMaxDepth) {
            return std::unexpected(make_error(
                "clp.linearize.depth-exceeded",
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
                "clp.linearize.depth-exceeded",
                "cycle detected while dereferencing logic variables"));
        }
        v = *lv->binding;
    }
}

LinearExpr expr_add(LinearExpr lhs, const LinearExpr& rhs) {
    lhs.constant += rhs.constant;
    lhs.terms.insert(lhs.terms.end(), rhs.terms.begin(), rhs.terms.end());
    return lhs;
}

LinearExpr expr_sub(LinearExpr lhs, const LinearExpr& rhs) {
    lhs.constant -= rhs.constant;
    lhs.terms.reserve(lhs.terms.size() + rhs.terms.size());
    for (const auto& t : rhs.terms) {
        lhs.terms.push_back(LinearTerm{.var_id = t.var_id, .coef = -t.coef});
    }
    return lhs;
}

LinearExpr expr_scale(LinearExpr expr, double k) {
    if (k == 0.0) {
        expr.terms.clear();
        expr.constant = 0.0;
        return expr;
    }
    expr.constant *= k;
    for (auto& t : expr.terms) t.coef *= k;
    return expr;
}

bool is_constant_expr(const LinearExpr& expr) {
    return expr.terms.empty();
}

std::vector<ObjectId> collect_offending_vars(const LinearExpr& lhs, const LinearExpr& rhs) {
    std::vector<ObjectId> ids;
    ids.reserve(lhs.terms.size() + rhs.terms.size());
    for (const auto& t : lhs.terms) ids.push_back(t.var_id);
    for (const auto& t : rhs.terms) ids.push_back(t.var_id);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

std::expected<LinearExpr, LinearizeErrorInfo>
expr_mul(LinearExpr lhs, LinearExpr rhs) {
    if (is_constant_expr(lhs)) {
        return expr_scale(std::move(rhs), lhs.constant);
    }
    if (is_constant_expr(rhs)) {
        return expr_scale(std::move(lhs), rhs.constant);
    }
    return std::unexpected(make_error(
        "clp.linearize.non-linear",
        "multiplication combines two variable-bearing expressions",
        collect_offending_vars(lhs, rhs)));
}

std::expected<ArithmeticOp, LinearizeErrorInfo>
decode_operator_symbol(LispVal op_term,
                       const OperatorIds& op_ids,
                       InternTable& intern_table) {
    if (!nanbox::ops::is_boxed(op_term) || nanbox::ops::tag(op_term) != Tag::Symbol) {
        return std::unexpected(make_error(
            "clp.linearize.type-error",
            "arithmetic call operator must be a symbol"));
    }
    const auto sid = static_cast<InternId>(nanbox::ops::payload(op_term));
    if (sid == op_ids.add) return ArithmeticOp::Add;
    if (sid == op_ids.sub) return ArithmeticOp::Sub;
    if (sid == op_ids.mul) return ArithmeticOp::Mul;
    return std::unexpected(make_error(
        "clp.linearize.unknown-atom",
        "unsupported operator " + describe_atom(op_term, intern_table)));
}

std::expected<std::vector<LispVal>, LinearizeErrorInfo>
collect_cons_args(LispVal cursor,
                  Heap& heap,
                  const std::unordered_set<ObjectId>& active_stack) {
    std::vector<LispVal> args;
    std::unordered_set<ObjectId> seen_cells;
    while (cursor != nanbox::Nil) {
        if (!nanbox::ops::is_boxed(cursor) || nanbox::ops::tag(cursor) != Tag::HeapObject) {
            return std::unexpected(make_error(
                "clp.linearize.type-error",
                "arithmetic call arguments must be in a proper list"));
        }
        const ObjectId cid = nanbox::ops::payload(cursor);
        if (active_stack.contains(cid) || !seen_cells.insert(cid).second) {
            return std::unexpected(make_error(
                "clp.linearize.depth-exceeded",
                "cycle detected while traversing cons call arguments"));
        }
        auto* cell = heap.try_get_as<ObjectKind::Cons, types::Cons>(cid);
        if (!cell) {
            return std::unexpected(make_error(
                "clp.linearize.type-error",
                "arithmetic call arguments must be in a proper list"));
        }
        args.push_back(cell->car);
        cursor = cell->cdr;
    }
    return args;
}

std::expected<DecodedCall, LinearizeErrorInfo>
decode_call(LispVal term,
            Heap& heap,
            InternTable& intern_table,
            const OperatorIds& op_ids,
            const std::unordered_set<ObjectId>& active_stack) {
    if (!nanbox::ops::is_boxed(term) || nanbox::ops::tag(term) != Tag::HeapObject) {
        return std::unexpected(make_error(
            "clp.linearize.type-error",
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
        "clp.linearize.type-error",
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

std::expected<LinearExpr, LinearizeErrorInfo>
linearize_rec(LispVal term,
              Heap& heap,
              InternTable& intern_table,
              const OperatorIds& op_ids,
              std::unordered_set<ObjectId>& active_stack,
              std::size_t depth);

std::expected<LinearExpr, LinearizeErrorInfo>
linearize_call(const DecodedCall& call,
               Heap& heap,
               InternTable& intern_table,
               const OperatorIds& op_ids,
               std::unordered_set<ObjectId>& active_stack,
               std::size_t depth) {
    switch (call.op) {
        case ArithmeticOp::Add: {
            LinearExpr acc;
            acc.constant = 0.0;
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
                    "clp.linearize.type-error",
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
            LinearExpr acc = std::move(*first);
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
            LinearExpr acc;
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
        "clp.linearize.type-error",
        "unknown arithmetic operator"));
}

std::expected<LinearExpr, LinearizeErrorInfo>
linearize_rec(LispVal term,
              Heap& heap,
              InternTable& intern_table,
              const OperatorIds& op_ids,
              std::unordered_set<ObjectId>& active_stack,
              std::size_t depth) {
    if (depth > kMaxDepth) {
        return std::unexpected(make_error(
            "clp.linearize.depth-exceeded",
            "expression nesting depth exceeded limit"));
    }

    auto deref = deref_term(term, heap);
    if (!deref) return std::unexpected(deref.error());
    LispVal v = *deref;

    const auto num = classify_numeric(v, heap);
    if (num.is_valid()) {
        LinearExpr expr;
        expr.constant = num.as_double();
        return expr;
    }

    if (nanbox::ops::is_boxed(v) && nanbox::ops::tag(v) == Tag::HeapObject) {
        const ObjectId id = nanbox::ops::payload(v);
        if (auto* lv = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id)) {
            if (!lv->binding.has_value()) {
                LinearExpr expr;
                expr.terms.push_back(LinearTerm{.var_id = id, .coef = 1.0});
                return expr;
            }
            return std::unexpected(make_error(
                "clp.linearize.type-error",
                "internal dereference error: expected unbound logic variable"));
        }

        if (heap.try_get_as<ObjectKind::Cons, types::Cons>(id) ||
            heap.try_get_as<ObjectKind::CompoundTerm, types::CompoundTerm>(id)) {
            ActiveNodeGuard guard(active_stack, id);
            if (!guard.inserted()) {
                return std::unexpected(make_error(
                    "clp.linearize.depth-exceeded",
                    "cycle detected while traversing expression term"));
            }
            auto call = decode_call(v, heap, intern_table, op_ids, active_stack);
            if (!call) return std::unexpected(call.error());
            return linearize_call(*call, heap, intern_table, op_ids, active_stack, depth);
        }
    }

    return std::unexpected(make_error(
        "clp.linearize.unknown-atom",
        "unsupported atom " + describe_atom(v, intern_table)));
}

} // namespace

void LinearExpr::canonicalize() {
    std::sort(terms.begin(), terms.end());

    std::vector<LinearTerm> merged;
    merged.reserve(terms.size());
    for (const auto& t : terms) {
        if (!merged.empty() && merged.back().var_id == t.var_id) {
            merged.back().coef += t.coef;
        } else {
            merged.push_back(t);
        }
    }

    std::vector<LinearTerm> filtered;
    filtered.reserve(merged.size());
    for (const auto& t : merged) {
        if (t.coef != 0.0) filtered.push_back(t);
    }
    terms = std::move(filtered);
    if (constant == 0.0) constant = 0.0;
}

std::expected<LinearExpr, LinearizeErrorInfo>
linearize(LispVal term, Heap& heap, InternTable& intern_table) {
    auto op_ids = load_operator_ids(intern_table);
    if (!op_ids) return std::unexpected(op_ids.error());

    std::unordered_set<ObjectId> active_stack;
    auto expr = linearize_rec(term, heap, intern_table, *op_ids, active_stack, 0);
    if (!expr) return std::unexpected(expr.error());

    expr->canonicalize();
    return *expr;
}

} // namespace eta::runtime::clp
