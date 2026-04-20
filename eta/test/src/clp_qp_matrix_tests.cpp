#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cmath>
#include <expected>
#include <limits>
#include <vector>

#include <eta/runtime/clp/quadratic.h>
#include <eta/runtime/factory.h>
#include <eta/runtime/memory/heap.h>
#include <eta/runtime/nanbox.h>

using namespace eta::runtime;
using namespace eta::runtime::clp;
using namespace eta::runtime::memory::factory;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::nanbox;

namespace {

template <typename T, typename E>
T require_ok(const std::expected<T, E>& r) {
    BOOST_REQUIRE(r.has_value());
    return *r;
}

constexpr double kTol = 1e-12;

struct QPMatrixFixture {
    Heap heap{4 * 1024 * 1024};

    LispVal lvar() {
        return require_ok(make_logic_var(heap));
    }

    ObjectId id_of(LispVal v) const {
        BOOST_REQUIRE(nanbox::ops::is_boxed(v));
        BOOST_REQUIRE(nanbox::ops::tag(v) == Tag::HeapObject);
        return nanbox::ops::payload(v);
    }

    std::size_t index_of(const QuadraticObjectiveMatrix& m, ObjectId id) const {
        const auto it = std::find(m.vars.begin(), m.vars.end(), id);
        BOOST_REQUIRE(it != m.vars.end());
        return static_cast<std::size_t>(std::distance(m.vars.begin(), it));
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(clp_qp_matrix_tests, QPMatrixFixture)

BOOST_AUTO_TEST_CASE(materialize_matrix_builds_deterministic_q_c_k) {
    const LispVal x = lvar();
    const LispVal y = lvar();
    const LispVal z = lvar();
    const auto xid = id_of(x);
    const auto yid = id_of(y);
    const auto zid = id_of(z);

    QuadraticExpr expr;
    expr.constant = 7.0;
    expr.linear_terms.push_back(LinearTerm{.var_id = zid, .coef = -1.0});
    expr.linear_terms.push_back(LinearTerm{.var_id = xid, .coef = 2.0});
    expr.linear_terms.push_back(LinearTerm{.var_id = zid, .coef = 3.0});
    expr.quadratic_terms.push_back(QuadraticTerm{.var_i = yid, .var_j = xid, .coef = 4.0});
    expr.quadratic_terms.push_back(QuadraticTerm{.var_i = xid, .var_j = yid, .coef = 1.0});
    expr.quadratic_terms.push_back(QuadraticTerm{.var_i = zid, .var_j = zid, .coef = 2.0});
    expr.quadratic_terms.push_back(QuadraticTerm{.var_i = yid, .var_j = zid, .coef = -3.0});
    expr.quadratic_terms.push_back(QuadraticTerm{.var_i = zid, .var_j = yid, .coef = 1.0});

    auto first = materialize_quadratic_objective_matrix(expr);
    auto second = materialize_quadratic_objective_matrix(expr);
    BOOST_REQUIRE(first.has_value());
    BOOST_REQUIRE(second.has_value());
    BOOST_TEST(first->vars == second->vars);
    BOOST_TEST(first->q == second->q);
    BOOST_TEST(first->c == second->c);
    BOOST_TEST(first->k == second->k, boost::test_tools::tolerance(kTol));

    BOOST_REQUIRE_EQUAL(first->dim(), 3u);
    BOOST_TEST(std::is_sorted(first->vars.begin(), first->vars.end()));
    BOOST_TEST(first->k == 7.0, boost::test_tools::tolerance(kTol));

    const std::size_t ix = index_of(*first, xid);
    const std::size_t iy = index_of(*first, yid);
    const std::size_t iz = index_of(*first, zid);
    BOOST_TEST(first->c[ix] == 2.0, boost::test_tools::tolerance(kTol));
    BOOST_TEST(first->c[iy] == 0.0, boost::test_tools::tolerance(kTol));
    BOOST_TEST(first->c[iz] == 2.0, boost::test_tools::tolerance(kTol));

    BOOST_TEST(first->q_at(ix, ix) == 0.0, boost::test_tools::tolerance(kTol));
    BOOST_TEST(first->q_at(iy, iy) == 0.0, boost::test_tools::tolerance(kTol));
    BOOST_TEST(first->q_at(iz, iz) == 4.0, boost::test_tools::tolerance(kTol));
    BOOST_TEST(first->q_at(ix, iy) == 5.0, boost::test_tools::tolerance(kTol));
    BOOST_TEST(first->q_at(iy, ix) == 5.0, boost::test_tools::tolerance(kTol));
    BOOST_TEST(first->q_at(iy, iz) == -2.0, boost::test_tools::tolerance(kTol));
    BOOST_TEST(first->q_at(iz, iy) == -2.0, boost::test_tools::tolerance(kTol));
}

BOOST_AUTO_TEST_CASE(convexity_check_accepts_psd_hessian) {
    const LispVal x = lvar();
    const LispVal y = lvar();
    const auto xid = id_of(x);
    const auto yid = id_of(y);

    QuadraticExpr expr;
    expr.quadratic_terms.push_back(QuadraticTerm{.var_i = xid, .var_j = xid, .coef = 1.0});
    expr.quadratic_terms.push_back(QuadraticTerm{.var_i = xid, .var_j = yid, .coef = 2.0});
    expr.quadratic_terms.push_back(QuadraticTerm{.var_i = yid, .var_j = yid, .coef = 1.0});
    auto matrix = materialize_quadratic_objective_matrix(expr);
    BOOST_REQUIRE(matrix.has_value());

    auto res = check_quadratic_convexity(*matrix);
    BOOST_TEST(res.has_value());
}

BOOST_AUTO_TEST_CASE(convexity_check_rejects_non_convex_hessian) {
    const LispVal x = lvar();
    const LispVal y = lvar();
    const auto xid = id_of(x);
    const auto yid = id_of(y);

    QuadraticExpr expr;
    expr.quadratic_terms.push_back(QuadraticTerm{.var_i = xid, .var_j = xid, .coef = -1.0});
    expr.quadratic_terms.push_back(QuadraticTerm{.var_i = yid, .var_j = yid, .coef = 1.0});
    auto matrix = materialize_quadratic_objective_matrix(expr);
    BOOST_REQUIRE(matrix.has_value());

    auto res = check_quadratic_convexity(*matrix);
    BOOST_REQUIRE(!res.has_value());
    BOOST_TEST(res.error().tag == "clp.qp.non-convex");
    const bool has_x = std::find(res.error().offending_vars.begin(),
                                 res.error().offending_vars.end(),
                                 xid) != res.error().offending_vars.end();
    const bool has_y = std::find(res.error().offending_vars.begin(),
                                 res.error().offending_vars.end(),
                                 yid) != res.error().offending_vars.end();
    BOOST_TEST(has_x);
    BOOST_TEST(has_y);
}

BOOST_AUTO_TEST_CASE(convexity_check_for_maximize_accepts_concave_objective) {
    const LispVal x = lvar();
    const auto xid = id_of(x);

    QuadraticExpr expr;
    expr.quadratic_terms.push_back(QuadraticTerm{.var_i = xid, .var_j = xid, .coef = -1.0});
    auto matrix = materialize_quadratic_objective_matrix(expr);
    BOOST_REQUIRE(matrix.has_value());

    auto res = check_quadratic_convexity(*matrix, -1.0);
    BOOST_TEST(res.has_value());
}

BOOST_AUTO_TEST_CASE(materialize_rejects_non_finite_coefficients) {
    const LispVal x = lvar();
    const auto xid = id_of(x);

    QuadraticExpr expr;
    expr.linear_terms.push_back(LinearTerm{
        .var_id = xid,
        .coef = std::numeric_limits<double>::quiet_NaN(),
    });

    auto res = materialize_quadratic_objective_matrix(expr);
    BOOST_REQUIRE(!res.has_value());
    BOOST_TEST(res.error().tag == "clp.qp.numeric-failure");
}

BOOST_AUTO_TEST_SUITE_END()
