#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <eta/runtime/clp/linear.h>
#include <eta/runtime/clp/simplex.h>
#include <eta/runtime/factory.h>
#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/intern_table.h>
#include <eta/runtime/nanbox.h>
#include <eta/runtime/types/cons.h>

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

constexpr double kTol = 1e-8;

struct SimplexFixture {
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
        LispVal out = Nil;
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

    LinearExpr diff_of(LispVal lhs, LispVal rhs) {
        auto l = linearize(lhs, heap, intern);
        auto r = linearize(rhs, heap, intern);
        BOOST_REQUIRE(l.has_value());
        BOOST_REQUIRE(r.has_value());
        LinearExpr d;
        d.constant = l->constant - r->constant;
        d.terms = l->terms;
        d.terms.reserve(l->terms.size() + r->terms.size());
        for (const auto& t : r->terms) {
            d.terms.push_back(LinearTerm{
                .var_id = t.var_id,
                .coef = -t.coef,
            });
        }
        d.canonicalize();
        return d;
    }

    void add_leq(Simplex& simplex, LispVal lhs, LispVal rhs) {
        simplex.add_leq(diff_of(lhs, rhs));
    }

    void add_geq(Simplex& simplex, LispVal lhs, LispVal rhs) {
        auto d = diff_of(lhs, rhs);
        d.constant = -d.constant;
        for (auto& t : d.terms) t.coef = -t.coef;
        simplex.add_leq(std::move(d));
    }

    void add_eq(Simplex& simplex, LispVal lhs, LispVal rhs) {
        simplex.add_eq(diff_of(lhs, rhs));
    }
};

void expect_bound(double actual, double expected) {
    if (std::isinf(expected)) {
        BOOST_TEST(std::isinf(actual));
        BOOST_TEST(std::signbit(actual) == std::signbit(expected));
        return;
    }
    BOOST_TEST(actual == expected, boost::test_tools::tolerance(kTol));
}

void expect_bounds(const SimplexBoundsResult& res, double lo, double hi) {
    BOOST_REQUIRE(static_cast<int>(res.status) == static_cast<int>(SimplexStatus::Feasible));
    BOOST_REQUIRE(res.bounds.has_value());
    expect_bound(res.bounds->lo, lo);
    expect_bound(res.bounds->hi, hi);
}

bool witness_value(const SimplexOptResult& res, ObjectId var_id, double& out) {
    for (const auto& [id, value] : res.witness) {
        if (id != var_id) continue;
        out = value;
        return true;
    }
    return false;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(clp_simplex_tests, SimplexFixture)

BOOST_AUTO_TEST_CASE(triangle_system_is_feasible) {
    const LispVal x = lvar();
    const LispVal y = lvar();

    Simplex simplex;
    add_geq(simplex, x, fx(0));
    add_geq(simplex, y, fx(0));
    add_leq(simplex, call("+", {x, y}), fx(3));

    BOOST_TEST(static_cast<int>(simplex.check()) == static_cast<int>(SimplexStatus::Feasible));
    expect_bounds(simplex.bounds_for(id_of(x)), 0.0, 3.0);
    expect_bounds(simplex.bounds_for(id_of(y)), 0.0, 3.0);
}

BOOST_AUTO_TEST_CASE(infeasible_bounds_are_detected) {
    const LispVal x = lvar();

    Simplex simplex;
    add_geq(simplex, x, fx(0));
    add_leq(simplex, x, fx(-1));

    BOOST_TEST(static_cast<int>(simplex.check()) == static_cast<int>(SimplexStatus::Infeasible));
    const auto bx = simplex.bounds_for(id_of(x));
    BOOST_TEST(static_cast<int>(bx.status) == static_cast<int>(SimplexStatus::Infeasible));
    BOOST_TEST(!bx.bounds.has_value());
}

BOOST_AUTO_TEST_CASE(equality_rows_project_correctly) {
    const LispVal x = lvar();
    const LispVal y = lvar();

    Simplex simplex;
    add_eq(simplex, call("+", {x, y}), fx(2));
    add_geq(simplex, x, fx(0));
    add_geq(simplex, y, fx(0));

    BOOST_TEST(static_cast<int>(simplex.check()) == static_cast<int>(SimplexStatus::Feasible));
    expect_bounds(simplex.bounds_for(id_of(x)), 0.0, 2.0);
    expect_bounds(simplex.bounds_for(id_of(y)), 0.0, 2.0);
}

BOOST_AUTO_TEST_CASE(asserted_bounds_tighten_and_are_idempotent) {
    const LispVal x = lvar();
    const ObjectId xid = id_of(x);

    Simplex simplex;
    add_leq(simplex, x, fx(10));

    BOOST_TEST(simplex.assert_upper(xid, Bound{.value = 7.0, .strict = false}));
    BOOST_TEST(!simplex.assert_upper(xid, Bound{.value = 9.0, .strict = false}));
    BOOST_TEST(simplex.assert_upper(xid, Bound{.value = 5.0, .strict = false}));
    BOOST_TEST(!simplex.assert_upper(xid, Bound{.value = 5.0, .strict = false}));

    BOOST_TEST(static_cast<int>(simplex.check()) == static_cast<int>(SimplexStatus::Feasible));
    expect_bounds(simplex.bounds_for(xid),
                  -std::numeric_limits<double>::infinity(),
                  5.0);
}

BOOST_AUTO_TEST_CASE(unbounded_side_is_reported_as_infinity) {
    const LispVal x = lvar();
    const ObjectId xid = id_of(x);

    Simplex simplex;
    add_geq(simplex, x, fx(1));

    BOOST_TEST(static_cast<int>(simplex.check()) == static_cast<int>(SimplexStatus::Feasible));
    expect_bounds(simplex.bounds_for(xid), 1.0, std::numeric_limits<double>::infinity());
}

BOOST_AUTO_TEST_CASE(strict_bound_conflict_is_infeasible) {
    const LispVal x = lvar();
    const ObjectId xid = id_of(x);

    Simplex simplex;
    simplex.assert_lower(xid, Bound{.value = 1.0, .strict = true});
    simplex.assert_upper(xid, Bound{.value = 1.0, .strict = false});

    BOOST_TEST(static_cast<int>(simplex.check()) == static_cast<int>(SimplexStatus::Infeasible));
}

BOOST_AUTO_TEST_CASE(optimize_max_finds_corner_witness) {
    const LispVal x = lvar();
    const LispVal y = lvar();
    const ObjectId xid = id_of(x);
    const ObjectId yid = id_of(y);

    Simplex simplex;
    add_geq(simplex, x, fx(0));
    add_geq(simplex, y, fx(0));
    add_leq(simplex, call("+", {x, y}), fx(3));

    auto objective = linearize(call("+", {call("*", {fx(2), x}), y}), heap, intern);
    BOOST_REQUIRE(objective.has_value());
    const auto result = simplex.optimize(*objective, SimplexDirection::Maximize);

    BOOST_REQUIRE(static_cast<int>(result.status) ==
                  static_cast<int>(SimplexOptResult::Status::Optimal));
    BOOST_TEST(result.value == 6.0, boost::test_tools::tolerance(kTol));

    double x_val = 0.0;
    double y_val = 0.0;
    BOOST_REQUIRE(witness_value(result, xid, x_val));
    BOOST_REQUIRE(witness_value(result, yid, y_val));
    BOOST_TEST(x_val == 3.0, boost::test_tools::tolerance(kTol));
    BOOST_TEST(y_val == 0.0, boost::test_tools::tolerance(kTol));
}

BOOST_AUTO_TEST_CASE(optimize_unbounded_status_is_reported) {
    const LispVal x = lvar();

    Simplex simplex;
    add_geq(simplex, x, fx(0));

    auto objective = linearize(x, heap, intern);
    BOOST_REQUIRE(objective.has_value());
    const auto result = simplex.optimize(*objective, SimplexDirection::Maximize);

    BOOST_TEST(static_cast<int>(result.status) ==
               static_cast<int>(SimplexOptResult::Status::Unbounded));
}

BOOST_AUTO_TEST_CASE(optimize_infeasible_status_is_reported) {
    const LispVal x = lvar();

    Simplex simplex;
    add_geq(simplex, x, fx(0));
    add_leq(simplex, x, fx(-1));

    auto objective = linearize(call("+", {x, fx(5)}), heap, intern);
    BOOST_REQUIRE(objective.has_value());
    const auto result = simplex.optimize(*objective, SimplexDirection::Minimize);

    BOOST_TEST(static_cast<int>(result.status) ==
               static_cast<int>(SimplexOptResult::Status::Infeasible));
}

BOOST_AUTO_TEST_SUITE_END()
