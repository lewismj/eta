#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cmath>
#include <expected>
#include <vector>

#include <eta/runtime/clp/qp_solver.h>
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

constexpr double kTol = 1e-6;

struct QPSolverFixture {
    Heap heap{4 * 1024 * 1024};

    LispVal lvar() {
        return require_ok(make_logic_var(heap));
    }

    ObjectId id_of(LispVal v) const {
        BOOST_REQUIRE(nanbox::ops::is_boxed(v));
        BOOST_REQUIRE(nanbox::ops::tag(v) == Tag::HeapObject);
        return nanbox::ops::payload(v);
    }

    QPModel make_model(const std::vector<ObjectId>& vars,
                       const std::vector<double>& q,
                       const std::vector<double>& c,
                       double k) {
        QPModel model;
        model.vars = vars;
        model.q = q;
        model.c = c;
        model.k = k;
        return model;
    }

    void add_leq(QPModel& model, const std::vector<double>& row, double rhs) {
        model.a_leq.insert(model.a_leq.end(), row.begin(), row.end());
        model.b_leq.push_back(rhs);
    }

    double witness_value(const QPSolveResult& res, ObjectId id) const {
        for (const auto& [var_id, value] : res.witness) {
            if (var_id == id) return value;
        }
        BOOST_FAIL("missing witness value for variable");
        return 0.0;
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(clp_qp_solver_tests, QPSolverFixture)

BOOST_AUTO_TEST_CASE(solves_bounded_convex_qp) {
    const LispVal x = lvar();
    const LispVal y = lvar();
    const auto xid = id_of(x);
    const auto yid = id_of(y);

    QPModel model = make_model(
        {xid, yid},
        {
            2.0, 0.0,
            0.0, 2.0,
        },
        {-2.0, -4.0},
        5.0);

    add_leq(model, {-1.0, 0.0}, 0.0); // x >= 0
    add_leq(model, {0.0, -1.0}, 0.0); // y >= 0
    add_leq(model, {1.0, 1.0}, 2.0);  // x + y <= 2

    auto res = solve_quadratic_program(model, SimplexDirection::Minimize, {0.0, 0.0});
    BOOST_REQUIRE(res.has_value());
    BOOST_TEST(static_cast<int>(res->status) == static_cast<int>(QPSolveResult::Status::Optimal));
    BOOST_TEST(res->value == 0.5, boost::test_tools::tolerance(kTol));
    BOOST_TEST(witness_value(*res, xid) == 0.5, boost::test_tools::tolerance(kTol));
    BOOST_TEST(witness_value(*res, yid) == 1.5, boost::test_tools::tolerance(kTol));
}

BOOST_AUTO_TEST_CASE(reports_infeasible_when_constraints_conflict) {
    const LispVal x = lvar();
    const auto xid = id_of(x);

    QPModel model = make_model(
        {xid},
        {2.0},
        {-2.0},
        1.0);

    add_leq(model, {1.0}, 0.0);   // x <= 0
    add_leq(model, {-1.0}, -1.0); // x >= 1

    auto res = solve_quadratic_program(model, SimplexDirection::Minimize, {0.0});
    BOOST_REQUIRE(res.has_value());
    BOOST_TEST(static_cast<int>(res->status) == static_cast<int>(QPSolveResult::Status::Infeasible));
}

BOOST_AUTO_TEST_CASE(detects_unbounded_psd_qp_with_nullspace_descent) {
    const LispVal x = lvar();
    const LispVal y = lvar();
    const auto xid = id_of(x);
    const auto yid = id_of(y);

    QPModel model = make_model(
        {xid, yid},
        {
            2.0, 0.0,
            0.0, 0.0,
        },
        {0.0, -1.0},
        0.0);

    auto res = solve_quadratic_program(model, SimplexDirection::Minimize, {0.0, 0.0});
    BOOST_REQUIRE(res.has_value());
    BOOST_TEST(static_cast<int>(res->status) == static_cast<int>(QPSolveResult::Status::Unbounded));
}

BOOST_AUTO_TEST_CASE(solves_concave_maximization_via_sign_flip) {
    const LispVal x = lvar();
    const auto xid = id_of(x);

    QPModel model = make_model(
        {xid},
        {-2.0},
        {2.0},
        -1.0);

    auto res = solve_quadratic_program(model, SimplexDirection::Maximize, {0.0});
    BOOST_REQUIRE(res.has_value());
    BOOST_TEST(static_cast<int>(res->status) == static_cast<int>(QPSolveResult::Status::Optimal));
    BOOST_TEST(res->value == 0.0, boost::test_tools::tolerance(kTol));
    BOOST_TEST(witness_value(*res, xid) == 1.0, boost::test_tools::tolerance(kTol));
}

BOOST_AUTO_TEST_CASE(solves_ill_conditioned_hessian_with_feasible_witness) {
    const LispVal x = lvar();
    const LispVal y = lvar();
    const auto xid = id_of(x);
    const auto yid = id_of(y);

    QPModel model = make_model(
        {xid, yid},
        {
            2.0e-12, 0.0,
            0.0, 2.0,
        },
        {0.0, 0.0},
        0.0);

    add_leq(model, {-1.0, 0.0}, 0.0); // x >= 0
    add_leq(model, {0.0, -1.0}, 0.0); // y >= 0
    model.a_eq = {1.0, 1.0};          // x + y == 1
    model.b_eq = {1.0};

    auto res = solve_quadratic_program(model, SimplexDirection::Minimize, {0.5, 0.5});
    BOOST_REQUIRE(res.has_value());
    BOOST_TEST(static_cast<int>(res->status) == static_cast<int>(QPSolveResult::Status::Optimal));

    const double xval = witness_value(*res, xid);
    const double yval = witness_value(*res, yid);
    BOOST_TEST(std::isfinite(xval));
    BOOST_TEST(std::isfinite(yval));
    BOOST_TEST(xval + yval == 1.0, boost::test_tools::tolerance(1e-6));
    BOOST_TEST(xval >= -kTol);
    BOOST_TEST(yval >= -kTol);
    BOOST_TEST(xval == 1.0, boost::test_tools::tolerance(1e-4));
    BOOST_TEST(yval == 0.0, boost::test_tools::tolerance(1e-4));
}

BOOST_AUTO_TEST_SUITE_END()
