#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cmath>
#include <expected>
#include <vector>

#include <eta/runtime/clp/quadratic.h>
#include <eta/runtime/factory.h>
#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/intern_table.h>
#include <eta/runtime/nanbox.h>
#include <eta/runtime/types/logic_var.h>

using namespace eta::runtime;
using namespace eta::runtime::clp;
using namespace eta::runtime::memory::factory;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::nanbox;

namespace {

template <typename T, typename E>
T require_ok(const std::expected<T, E>& r) {
    BOOST_REQUIRE(r.has_value());
    return *r;
}

constexpr double kTol = 1e-12;

struct QPLinearizeFixture {
    Heap heap{4 * 1024 * 1024};
    InternTable intern;

    LispVal sym(const char* s) {
        return require_ok(make_symbol(intern, s));
    }

    LispVal fx(int64_t v) {
        return require_ok(make_fixnum(heap, v));
    }

    LispVal lvar() {
        return require_ok(make_logic_var(heap));
    }

    LispVal list(const std::vector<LispVal>& elems) {
        LispVal out = nanbox::Nil;
        for (auto it = elems.rbegin(); it != elems.rend(); ++it) {
            out = require_ok(make_cons(heap, *it, out));
        }
        return out;
    }

    LispVal call(const char* op, std::initializer_list<LispVal> args) {
        std::vector<LispVal> elems;
        elems.reserve(args.size() + 1);
        elems.push_back(sym(op));
        elems.insert(elems.end(), args.begin(), args.end());
        return list(elems);
    }

    ObjectId id_of(LispVal v) const {
        BOOST_REQUIRE(nanbox::ops::is_boxed(v));
        BOOST_REQUIRE(nanbox::ops::tag(v) == Tag::HeapObject);
        return nanbox::ops::payload(v);
    }

    double linear_coef_of(const QuadraticExpr& expr, ObjectId id) const {
        for (const auto& t : expr.linear_terms) {
            if (t.var_id == id) return t.coef;
        }
        return 0.0;
    }

    double quadratic_coef_of(const QuadraticExpr& expr, ObjectId a, ObjectId b) const {
        const ObjectId i = std::min(a, b);
        const ObjectId j = std::max(a, b);
        for (const auto& t : expr.quadratic_terms) {
            if (t.var_i == i && t.var_j == j) return t.coef;
        }
        return 0.0;
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(clp_qp_linearize_tests, QPLinearizeFixture)

BOOST_AUTO_TEST_CASE(canonicalize_merges_duplicates_and_normalizes_pairs) {
    const LispVal x = lvar();
    const LispVal y = lvar();
    const auto xid = id_of(x);
    const auto yid = id_of(y);

    QuadraticExpr expr;
    expr.constant = 3.0;
    expr.linear_terms.push_back(LinearTerm{.var_id = xid, .coef = 1.5});
    expr.linear_terms.push_back(LinearTerm{.var_id = xid, .coef = -1.5});
    expr.linear_terms.push_back(LinearTerm{.var_id = yid, .coef = 2.0});
    expr.quadratic_terms.push_back(QuadraticTerm{.var_i = yid, .var_j = xid, .coef = 1.0});
    expr.quadratic_terms.push_back(QuadraticTerm{.var_i = xid, .var_j = yid, .coef = 2.0});
    expr.quadratic_terms.push_back(QuadraticTerm{.var_i = xid, .var_j = xid, .coef = 4.0});
    expr.quadratic_terms.push_back(QuadraticTerm{.var_i = xid, .var_j = xid, .coef = -4.0});

    expr.canonicalize();

    BOOST_REQUIRE_EQUAL(expr.linear_terms.size(), 1u);
    BOOST_REQUIRE_EQUAL(expr.quadratic_terms.size(), 1u);
    BOOST_TEST(expr.constant == 3.0, boost::test_tools::tolerance(kTol));
    BOOST_TEST(linear_coef_of(expr, yid) == 2.0, boost::test_tools::tolerance(kTol));
    BOOST_TEST(quadratic_coef_of(expr, xid, yid) == 3.0, boost::test_tools::tolerance(kTol));
}

BOOST_AUTO_TEST_CASE(variable_product_is_accepted_for_objective_linearization) {
    const LispVal x = lvar();
    const LispVal y = lvar();
    const auto xid = id_of(x);
    const auto yid = id_of(y);

    auto res = linearize_quadratic_objective(call("*", {x, y}), heap, intern);
    BOOST_REQUIRE(res.has_value());
    BOOST_TEST(res->linear_terms.empty());
    BOOST_REQUIRE_EQUAL(res->quadratic_terms.size(), 1u);
    BOOST_TEST(quadratic_coef_of(*res, xid, yid) == 1.0, boost::test_tools::tolerance(kTol));
    BOOST_TEST(res->constant == 0.0, boost::test_tools::tolerance(kTol));
}

BOOST_AUTO_TEST_CASE(affine_product_expands_to_quadratic_linear_and_constant_parts) {
    const LispVal x = lvar();
    const LispVal y = lvar();
    const auto xid = id_of(x);
    const auto yid = id_of(y);

    const LispVal expr = call("*", {
        call("+", {x, fx(2)}),
        call("+", {y, fx(3)}),
    });

    auto res = linearize_quadratic_objective(expr, heap, intern);
    BOOST_REQUIRE(res.has_value());
    BOOST_REQUIRE_EQUAL(res->quadratic_terms.size(), 1u);
    BOOST_REQUIRE_EQUAL(res->linear_terms.size(), 2u);
    BOOST_TEST(quadratic_coef_of(*res, xid, yid) == 1.0, boost::test_tools::tolerance(kTol));
    BOOST_TEST(linear_coef_of(*res, xid) == 3.0, boost::test_tools::tolerance(kTol));
    BOOST_TEST(linear_coef_of(*res, yid) == 2.0, boost::test_tools::tolerance(kTol));
    BOOST_TEST(res->constant == 6.0, boost::test_tools::tolerance(kTol));
}

BOOST_AUTO_TEST_CASE(cubic_objective_term_is_rejected) {
    const LispVal x = lvar();
    const LispVal y = lvar();
    const LispVal z = lvar();

    auto res = linearize_quadratic_objective(call("*", {x, y, z}), heap, intern);
    BOOST_REQUIRE(!res.has_value());
    BOOST_TEST(res.error().tag == "clp.qp.linearize.non-quadratic");
    const bool has_x = std::find(res.error().offending_vars.begin(),
                                 res.error().offending_vars.end(),
                                 id_of(x)) != res.error().offending_vars.end();
    const bool has_y = std::find(res.error().offending_vars.begin(),
                                 res.error().offending_vars.end(),
                                 id_of(y)) != res.error().offending_vars.end();
    const bool has_z = std::find(res.error().offending_vars.begin(),
                                 res.error().offending_vars.end(),
                                 id_of(z)) != res.error().offending_vars.end();
    BOOST_TEST(has_x);
    BOOST_TEST(has_y);
    BOOST_TEST(has_z);
}

BOOST_AUTO_TEST_CASE(depth_guard_rejects_very_deep_nesting) {
    LispVal expr = lvar();
    for (int i = 0; i < 520; ++i) {
        expr = call("+", {expr, fx(1)});
    }

    auto res = linearize_quadratic_objective(expr, heap, intern);
    BOOST_REQUIRE(!res.has_value());
    BOOST_TEST(res.error().tag == "clp.qp.linearize.depth-exceeded");
}

BOOST_AUTO_TEST_SUITE_END()

