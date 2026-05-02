#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cmath>
#include <expected>
#include <vector>

#include <eta/runtime/clp/linear.h>
#include <eta/runtime/factory.h>
#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/intern_table.h>
#include <eta/runtime/nanbox.h>
#include <eta/runtime/types/cons.h>
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

struct LinearFixture {
    Heap heap{4 * 1024 * 1024};
    InternTable intern;

    LispVal sym(const char* s) {
        return require_ok(make_symbol(intern, s));
    }

    LispVal fx(int64_t v) {
        return require_ok(make_fixnum(heap, v));
    }

    LispVal fl(double v) {
        return require_ok(make_flonum(v));
    }

    LispVal lvar() {
        return require_ok(make_logic_var(heap));
    }

    LispVal list(std::initializer_list<LispVal> elems) {
        std::vector<LispVal> vec(elems);
        return list(vec);
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

    LispVal compound_call(const char* op, std::vector<LispVal> args) {
        return require_ok(make_compound(heap, sym(op), std::move(args)));
    }

    ObjectId id_of(LispVal v) const {
        BOOST_REQUIRE(nanbox::ops::is_boxed(v));
        BOOST_REQUIRE(nanbox::ops::tag(v) == Tag::HeapObject);
        return nanbox::ops::payload(v);
    }

    double coef_of(const LinearExpr& expr, ObjectId id) const {
        for (const auto& t : expr.terms) {
            if (t.var_id == id) return t.coef;
        }
        return 0.0;
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(clp_linear_tests, LinearFixture)

BOOST_AUTO_TEST_CASE(constants_only_fold_to_constant) {
    const LispVal expr = call("+", {fx(1), fx(2), call("*", {fx(3), fx(4)})});
    auto res = linearize(expr, heap, intern);
    BOOST_REQUIRE(res.has_value());
    BOOST_TEST(res->terms.empty());
    BOOST_TEST(res->constant == 15.0, boost::test_tools::tolerance(kTol));
}

BOOST_AUTO_TEST_CASE(simple_unbound_var_is_unit_term) {
    const LispVal x = lvar();
    auto res = linearize(x, heap, intern);
    BOOST_REQUIRE(res.has_value());
    BOOST_REQUIRE_EQUAL(res->terms.size(), 1u);
    BOOST_TEST(res->terms[0].var_id == id_of(x));
    BOOST_TEST(res->terms[0].coef == 1.0, boost::test_tools::tolerance(kTol));
    BOOST_TEST(res->constant == 0.0, boost::test_tools::tolerance(kTol));
}

BOOST_AUTO_TEST_CASE(accepts_compound_and_cons_forms) {
    const LispVal x = lvar();
    const LispVal cons_expr = call("+", {x, fx(2)});
    const LispVal cmp_expr = compound_call("+", {x, fx(2)});

    auto a = linearize(cons_expr, heap, intern);
    auto b = linearize(cmp_expr, heap, intern);
    BOOST_REQUIRE(a.has_value());
    BOOST_REQUIRE(b.has_value());
    BOOST_REQUIRE_EQUAL(a->terms.size(), 1u);
    BOOST_REQUIRE_EQUAL(b->terms.size(), 1u);
    BOOST_TEST(a->terms[0].var_id == b->terms[0].var_id);
    BOOST_TEST(a->terms[0].coef == b->terms[0].coef, boost::test_tools::tolerance(kTol));
    BOOST_TEST(a->constant == b->constant, boost::test_tools::tolerance(kTol));
}

BOOST_AUTO_TEST_CASE(scaling_distributes_over_sum) {
    const LispVal x = lvar();
    const LispVal expr = compound_call("*", {fx(2), compound_call("+", {x, fx(3)})});

    auto res = linearize(expr, heap, intern);
    BOOST_REQUIRE(res.has_value());
    BOOST_REQUIRE_EQUAL(res->terms.size(), 1u);
    BOOST_TEST(res->terms[0].var_id == id_of(x));
    BOOST_TEST(res->terms[0].coef == 2.0, boost::test_tools::tolerance(kTol));
    BOOST_TEST(res->constant == 6.0, boost::test_tools::tolerance(kTol));
}

BOOST_AUTO_TEST_CASE(cancellation_drops_zero_terms) {
    const LispVal x = lvar();
    const LispVal expr = call("+", {x, call("-", {x})});

    auto res = linearize(expr, heap, intern);
    BOOST_REQUIRE(res.has_value());
    BOOST_TEST(res->terms.empty());
    BOOST_TEST(res->constant == 0.0, boost::test_tools::tolerance(kTol));
}

BOOST_AUTO_TEST_CASE(nested_mixed_terms_normalize_canonically) {
    const LispVal x = lvar();
    const LispVal y = lvar();
    const LispVal expr = call("+", {
        call("*", {fx(2), call("+", {x, y})}),
        call("*", {fx(-1), x}),
    });

    auto res = linearize(expr, heap, intern);
    BOOST_REQUIRE(res.has_value());
    BOOST_REQUIRE_EQUAL(res->terms.size(), 2u);
    BOOST_TEST(coef_of(*res, id_of(x)) == 1.0, boost::test_tools::tolerance(kTol));
    BOOST_TEST(coef_of(*res, id_of(y)) == 2.0, boost::test_tools::tolerance(kTol));
    BOOST_TEST(res->constant == 0.0, boost::test_tools::tolerance(kTol));
    BOOST_TEST(res->terms[0].var_id < res->terms[1].var_id);
}

BOOST_AUTO_TEST_CASE(non_linear_product_is_rejected) {
    const LispVal x = lvar();
    const LispVal y = lvar();
    const LispVal expr = call("*", {x, y});

    auto res = linearize(expr, heap, intern);
    BOOST_REQUIRE(!res.has_value());
    BOOST_TEST(res.error().tag == "clp.linearize.non-linear");
    const bool has_x = std::find(res.error().offending_vars.begin(),
                                 res.error().offending_vars.end(),
                                 id_of(x)) != res.error().offending_vars.end();
    const bool has_y = std::find(res.error().offending_vars.begin(),
                                 res.error().offending_vars.end(),
                                 id_of(y)) != res.error().offending_vars.end();
    BOOST_TEST(has_x);
    BOOST_TEST(has_y);
}

BOOST_AUTO_TEST_CASE(deref_chain_resolves_before_classification) {
    const LispVal x = lvar();
    const LispVal y = lvar();
    const LispVal z = lvar();

    auto* lvx = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id_of(x));
    auto* lvy = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id_of(y));
    BOOST_REQUIRE(lvx);
    BOOST_REQUIRE(lvy);
    lvx->binding = y;
    lvy->binding = fx(5);

    const LispVal expr = call("+", {x, z});
    auto res = linearize(expr, heap, intern);
    BOOST_REQUIRE(res.has_value());
    BOOST_REQUIRE_EQUAL(res->terms.size(), 1u);
    BOOST_TEST(res->terms[0].var_id == id_of(z));
    BOOST_TEST(res->terms[0].coef == 1.0, boost::test_tools::tolerance(kTol));
    BOOST_TEST(res->constant == 5.0, boost::test_tools::tolerance(kTol));
}

BOOST_AUTO_TEST_CASE(shared_dag_subterm_is_allowed) {
    const LispVal x = lvar();
    const LispVal shared = call("+", {x, fx(1)});
    const LispVal expr = call("+", {shared, shared});

    auto res = linearize(expr, heap, intern);
    BOOST_REQUIRE(res.has_value());
    BOOST_REQUIRE_EQUAL(res->terms.size(), 1u);
    BOOST_TEST(res->terms[0].var_id == id_of(x));
    BOOST_TEST(res->terms[0].coef == 2.0, boost::test_tools::tolerance(kTol));
    BOOST_TEST(res->constant == 2.0, boost::test_tools::tolerance(kTol));
}

BOOST_AUTO_TEST_CASE(cycle_in_cons_term_is_rejected) {
    const LispVal root = require_ok(make_cons(heap, sym("+"), nanbox::Nil));
    auto* root_cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(id_of(root));
    BOOST_REQUIRE(root_cons);
    const LispVal arg_cell = require_ok(make_cons(heap, root, nanbox::Nil));
    root_cons->cdr = arg_cell;

    auto res = linearize(root, heap, intern);
    BOOST_REQUIRE(!res.has_value());
    BOOST_TEST(res.error().tag == "clp.linearize.depth-exceeded");
}

BOOST_AUTO_TEST_CASE(depth_guard_rejects_very_deep_nesting) {
    LispVal expr = lvar();
    for (int i = 0; i < 520; ++i) {
        expr = call("+", {expr, fx(1)});
    }

    auto res = linearize(expr, heap, intern);
    BOOST_REQUIRE(!res.has_value());
    BOOST_TEST(res.error().tag == "clp.linearize.depth-exceeded");
}

BOOST_AUTO_TEST_SUITE_END()
