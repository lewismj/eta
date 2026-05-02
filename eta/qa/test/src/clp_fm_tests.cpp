#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <eta/runtime/clp/fm.h>
#include <eta/runtime/clp/linear.h>
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

constexpr double kTol = 1e-9;

struct FMFixture {
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

    void add_leq(FMSystem& sys, LispVal lhs, LispVal rhs) {
        sys.leq.push_back(diff_of(lhs, rhs));
    }

    void add_geq(FMSystem& sys, LispVal lhs, LispVal rhs) {
        auto d = diff_of(lhs, rhs);
        d.constant = -d.constant;
        for (auto& t : d.terms) t.coef = -t.coef;
        sys.leq.push_back(std::move(d));
    }

    void add_eq(FMSystem& sys, LispVal lhs, LispVal rhs) {
        sys.eq.push_back(diff_of(lhs, rhs));
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

void expect_bounds(const FMBoundsResult& res, double lo, double hi) {
    BOOST_REQUIRE(static_cast<int>(res.status) == static_cast<int>(FMStatus::Feasible));
    BOOST_REQUIRE(res.bounds.has_value());
    expect_bound(res.bounds->lo, lo);
    expect_bound(res.bounds->hi, hi);
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(clp_fm_tests, FMFixture)

BOOST_AUTO_TEST_CASE(triangle_system_is_feasible) {
    const LispVal x = lvar();
    const LispVal y = lvar();

    FMSystem sys;
    add_geq(sys, x, fx(0));
    add_geq(sys, y, fx(0));
    add_leq(sys, call("+", {x, y}), fx(3));

    auto feas = fm_feasible(sys);
    BOOST_TEST(static_cast<int>(feas.status) == static_cast<int>(FMStatus::Feasible));

    auto bx = fm_bounds_for(sys, id_of(x));
    auto by = fm_bounds_for(sys, id_of(y));
    expect_bounds(bx, 0.0, 3.0);
    expect_bounds(by, 0.0, 3.0);
}

BOOST_AUTO_TEST_CASE(direct_contradiction_is_infeasible) {
    const LispVal x = lvar();

    FMSystem sys;
    add_geq(sys, x, fx(0));
    add_leq(sys, x, fx(-1));

    auto feas = fm_feasible(sys);
    BOOST_TEST(static_cast<int>(feas.status) == static_cast<int>(FMStatus::Infeasible));

    auto bx = fm_bounds_for(sys, id_of(x));
    BOOST_TEST(static_cast<int>(bx.status) == static_cast<int>(FMStatus::Infeasible));
    BOOST_TEST(!bx.bounds.has_value());
}

BOOST_AUTO_TEST_CASE(equality_expands_correctly) {
    const LispVal x = lvar();
    const LispVal y = lvar();

    FMSystem sys;
    add_eq(sys, call("+", {x, y}), fx(2));
    add_geq(sys, x, fx(0));
    add_geq(sys, y, fx(0));

    auto feas = fm_feasible(sys);
    BOOST_TEST(static_cast<int>(feas.status) == static_cast<int>(FMStatus::Feasible));

    auto bx = fm_bounds_for(sys, id_of(x));
    auto by = fm_bounds_for(sys, id_of(y));
    expect_bounds(bx, 0.0, 2.0);
    expect_bounds(by, 0.0, 2.0);
}

BOOST_AUTO_TEST_CASE(redundant_upper_bounds_keep_tightest) {
    const LispVal x = lvar();

    FMSystem sys;
    add_leq(sys, x, fx(10));
    add_leq(sys, x, fx(5));

    auto bx = fm_bounds_for(sys, id_of(x));
    expect_bounds(bx,
                  -std::numeric_limits<double>::infinity(),
                  5.0);
}

BOOST_AUTO_TEST_CASE(bound_tightening_from_two_var_constraint) {
    const LispVal x = lvar();
    const LispVal y = lvar();

    FMSystem sys;
    add_leq(sys, call("+", {x, y}), fx(3));
    add_geq(sys, x, fx(1));
    add_geq(sys, y, fx(1));

    auto bx = fm_bounds_for(sys, id_of(x));
    auto by = fm_bounds_for(sys, id_of(y));
    expect_bounds(bx, 1.0, 2.0);
    expect_bounds(by, 1.0, 2.0);
}

BOOST_AUTO_TEST_CASE(unbounded_upper_side_reports_positive_infinity) {
    const LispVal x = lvar();

    FMSystem sys;
    add_geq(sys, x, fx(1));

    auto bx = fm_bounds_for(sys, id_of(x));
    expect_bounds(bx,
                  1.0,
                  std::numeric_limits<double>::infinity());
}

BOOST_AUTO_TEST_CASE(cap_guard_reports_cap_exceeded) {
    const LispVal x = lvar();
    std::vector<LispVal> ys;
    std::vector<LispVal> zs;
    ys.reserve(8);
    zs.reserve(8);
    for (int i = 0; i < 8; ++i) {
        ys.push_back(lvar());
        zs.push_back(lvar());
    }

    FMSystem sys;
    for (int i = 0; i < 8; ++i) {
        add_leq(sys, call("+", {x, ys[static_cast<std::size_t>(i)]}), fx(100 + i));
        add_leq(sys, call("+", {call("-", {x}), zs[static_cast<std::size_t>(i)]}), fx(100 + i));
    }

    auto feas = fm_feasible(sys, FMConfig{
        .row_cap = 16,
        .eps = 1e-12,
    });
    BOOST_TEST(static_cast<int>(feas.status) == static_cast<int>(FMStatus::CapExceeded));

    auto by = fm_bounds_for(sys, id_of(ys[0]), FMConfig{
        .row_cap = 16,
        .eps = 1e-12,
    });
    BOOST_TEST(static_cast<int>(by.status) == static_cast<int>(FMStatus::CapExceeded));
    BOOST_TEST(!by.bounds.has_value());
}

BOOST_AUTO_TEST_CASE(input_order_permutations_are_deterministic) {
    const LispVal x = lvar();
    const LispVal y = lvar();

    FMSystem a;
    add_geq(a, x, fx(0));
    add_geq(a, y, fx(0));
    add_leq(a, call("+", {x, y}), fx(4));
    add_leq(a, x, fx(3));
    add_leq(a, y, fx(3));

    FMSystem b = a;
    std::reverse(b.leq.begin(), b.leq.end());

    auto feas_a = fm_feasible(a);
    auto feas_b = fm_feasible(b);
    BOOST_TEST(static_cast<int>(feas_a.status) == static_cast<int>(feas_b.status));

    auto bx_a = fm_bounds_for(a, id_of(x));
    auto bx_b = fm_bounds_for(b, id_of(x));
    auto by_a = fm_bounds_for(a, id_of(y));
    auto by_b = fm_bounds_for(b, id_of(y));

    BOOST_TEST(static_cast<int>(bx_a.status) == static_cast<int>(bx_b.status));
    BOOST_TEST(static_cast<int>(by_a.status) == static_cast<int>(by_b.status));
    expect_bounds(bx_a, 0.0, 3.0);
    expect_bounds(bx_b, 0.0, 3.0);
    expect_bounds(by_a, 0.0, 3.0);
    expect_bounds(by_b, 0.0, 3.0);
}

BOOST_AUTO_TEST_SUITE_END()
