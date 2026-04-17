#include <boost/test/unit_test.hpp>

#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/intern_table.h>
#include <eta/runtime/factory.h>
#include <eta/runtime/nanbox.h>
#include <eta/runtime/builtin_env.h>
#include <eta/runtime/numeric_value.h>
#include <eta/runtime/types/fact_table.h>
#include <eta/runtime/stats_math.h>
#include <eta/runtime/stats_extract.h>

#include <eta/stats/stats_primitives.h>

using namespace eta::runtime::memory;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::memory::factory;
using namespace eta::runtime::nanbox;
using namespace eta::runtime::nanbox::ops;
using namespace eta::runtime;
using namespace eta::stats_bindings;

namespace {
    template <typename T, typename E>
    T expect_ok(const std::expected<T,E>& r) {
        BOOST_REQUIRE(r.has_value());
        return *r;
    }

    /// Build an Eta list of flonums from a vector of doubles.
    LispVal doubles_to_list(Heap& heap, const std::vector<double>& xs) {
        LispVal result = Nil;
        for (auto it = xs.rbegin(); it != xs.rend(); ++it) {
            auto enc = ops::encode(*it);
            BOOST_REQUIRE(enc.has_value());
            auto cell = make_cons(heap, *enc, result);
            BOOST_REQUIRE(cell.has_value());
            result = *cell;
        }
        return result;
    }

    /// Build a FactTable with named columns from vectors of doubles.
    LispVal make_test_fact_table(Heap& heap,
                                  const std::vector<std::string>& col_names,
                                  const std::vector<std::vector<double>>& data) {
        types::FactTable ft;
        ft.col_names = col_names;
        ft.columns.resize(col_names.size());
        ft.indexes.resize(col_names.size());
        ft.row_count = 0;

        if (!data.empty()) {
            std::size_t nrows = data[0].size();
            for (std::size_t r = 0; r < nrows; ++r) {
                std::vector<LispVal> row;
                for (std::size_t c = 0; c < col_names.size(); ++c) {
                    auto enc = ops::encode(data[c][r]);
                    BOOST_REQUIRE(enc.has_value());
                    row.push_back(*enc);
                }
                ft.add_row(row);
            }
        }

        auto val = heap.allocate<types::FactTable, ObjectKind::FactTable>(std::move(ft));
        BOOST_REQUIRE(val.has_value());
        return ops::box(Tag::HeapObject, *val);
    }

    /// Build an Eta list of fixnums from a vector of ints.
    LispVal ints_to_list(Heap& heap, const std::vector<int>& xs) {
        LispVal result = Nil;
        for (auto it = xs.rbegin(); it != xs.rend(); ++it) {
            auto enc = ops::encode(static_cast<int64_t>(*it));
            BOOST_REQUIRE(enc.has_value());
            auto cell = make_cons(heap, *enc, result);
            BOOST_REQUIRE(cell.has_value());
            result = *cell;
        }
        return result;
    }

    /// Walk an Eta list and collect doubles.
    std::vector<double> list_to_vec(Heap& heap, LispVal lst) {
        std::vector<double> result;
        LispVal cur = lst;
        while (cur != Nil) {
            auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
            BOOST_REQUIRE(cons != nullptr);
            auto nv = classify_numeric(cons->car, heap);
            BOOST_REQUIRE(nv.is_valid());
            result.push_back(nv.as_double());
            cur = cons->cdr;
        }
        return result;
    }

    /// Walk a nested list (list of lists) into a 2D vector.
    std::vector<std::vector<double>> nested_list_to_matrix(Heap& heap, LispVal lst) {
        std::vector<std::vector<double>> result;
        LispVal cur = lst;
        while (cur != Nil) {
            auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
            BOOST_REQUIRE(cons != nullptr);
            result.push_back(list_to_vec(heap, cons->car));
            cur = cons->cdr;
        }
        return result;
    }

    /// Build an Eta list of Eta lists (list of sequences) from vectors of doubles.
    LispVal make_seq_list(Heap& heap, const std::vector<std::vector<double>>& seqs) {
        LispVal result = Nil;
        for (auto it = seqs.rbegin(); it != seqs.rend(); ++it) {
            auto lst = doubles_to_list(heap, *it);
            auto cell = make_cons(heap, lst, result);
            BOOST_REQUIRE(cell.has_value());
            result = *cell;
        }
        return result;
    }
}

BOOST_AUTO_TEST_SUITE(stats_tests)


BOOST_AUTO_TEST_CASE(builtin_names_smoke) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_stats_primitives(env, heap, intern, nullptr);

    BOOST_TEST(env.lookup("%stats-mean-vec").has_value());
    BOOST_TEST(env.lookup("%stats-var-vec").has_value());
    BOOST_TEST(env.lookup("%stats-cov-matrix").has_value());
    BOOST_TEST(env.lookup("%stats-cor-matrix").has_value());
    BOOST_TEST(env.lookup("%stats-quantile-vec").has_value());
    BOOST_TEST(env.lookup("%stats-ols-multi").has_value());
}


BOOST_AUTO_TEST_CASE(mean_vec_fact_table) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_stats_primitives(env, heap, intern, nullptr);

    /// 3 rows, 2 columns: x=[1,2,3], y=[4,5,6]
    auto ft = make_test_fact_table(heap, {"x", "y"}, {{1,2,3}, {4,5,6}});
    auto cols = ints_to_list(heap, {0, 1});

    auto idx = env.lookup("%stats-mean-vec");
    BOOST_REQUIRE(idx.has_value());
    std::vector<LispVal> args = {ft, cols};
    auto result = env.specs()[*idx].func(args);
    BOOST_REQUIRE(result.has_value());

    auto means = list_to_vec(heap, *result);
    BOOST_REQUIRE_EQUAL(means.size(), 2u);
    BOOST_TEST(std::abs(means[0] - 2.0) < 1e-10);
    BOOST_TEST(std::abs(means[1] - 5.0) < 1e-10);
}


BOOST_AUTO_TEST_CASE(mean_vec_list_of_seqs) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_stats_primitives(env, heap, intern, nullptr);

    auto seqs = make_seq_list(heap, {{1,2,3}, {4,5,6}});

    auto idx = env.lookup("%stats-mean-vec");
    BOOST_REQUIRE(idx.has_value());
    std::vector<LispVal> args = {seqs};
    auto result = env.specs()[*idx].func(args);
    BOOST_REQUIRE(result.has_value());

    auto means = list_to_vec(heap, *result);
    BOOST_REQUIRE_EQUAL(means.size(), 2u);
    BOOST_TEST(std::abs(means[0] - 2.0) < 1e-10);
    BOOST_TEST(std::abs(means[1] - 5.0) < 1e-10);
}


BOOST_AUTO_TEST_CASE(var_vec_fact_table) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_stats_primitives(env, heap, intern, nullptr);

    /// x=[2,4,6], y=[1,1,1]
    auto ft = make_test_fact_table(heap, {"x", "y"}, {{2,4,6}, {1,1,1}});
    auto cols = ints_to_list(heap, {0, 1});

    auto idx = env.lookup("%stats-var-vec");
    BOOST_REQUIRE(idx.has_value());
    std::vector<LispVal> args = {ft, cols};
    auto result = env.specs()[*idx].func(args);
    BOOST_REQUIRE(result.has_value());

    auto vars = list_to_vec(heap, *result);
    BOOST_REQUIRE_EQUAL(vars.size(), 2u);
    BOOST_TEST(std::abs(vars[0] - 4.0) < 1e-10);  ///< var([2,4,6]) = 4
    BOOST_TEST(std::abs(vars[1] - 0.0) < 1e-10);  ///< var([1,1,1]) = 0
}


BOOST_AUTO_TEST_CASE(cov_matrix_fact_table) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_stats_primitives(env, heap, intern, nullptr);

    /// x=[1,2,3,4,5], y=[2,4,6,8,10]  (y = 2x, perfect correlation)
    auto ft = make_test_fact_table(heap, {"x", "y"}, {{1,2,3,4,5}, {2,4,6,8,10}});
    auto cols = ints_to_list(heap, {0, 1});

    auto idx = env.lookup("%stats-cov-matrix");
    BOOST_REQUIRE(idx.has_value());
    std::vector<LispVal> args = {ft, cols};
    auto result = env.specs()[*idx].func(args);
    BOOST_REQUIRE(result.has_value());

    auto mat = nested_list_to_matrix(heap, *result);
    BOOST_REQUIRE_EQUAL(mat.size(), 2u);
    BOOST_REQUIRE_EQUAL(mat[0].size(), 2u);
    /// cov(x,x) = 2.5, cov(x,y) = 5.0, cov(y,y) = 10.0
    BOOST_TEST(std::abs(mat[0][0] - 2.5) < 1e-10);
    BOOST_TEST(std::abs(mat[0][1] - 5.0) < 1e-10);
    BOOST_TEST(std::abs(mat[1][0] - 5.0) < 1e-10);
    BOOST_TEST(std::abs(mat[1][1] - 10.0) < 1e-10);
}

BOOST_AUTO_TEST_CASE(cov_matrix_list_of_seqs) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_stats_primitives(env, heap, intern, nullptr);

    auto seqs = make_seq_list(heap, {{1,2,3,4,5}, {2,4,6,8,10}});

    auto idx = env.lookup("%stats-cov-matrix");
    BOOST_REQUIRE(idx.has_value());
    std::vector<LispVal> args = {seqs};
    auto result = env.specs()[*idx].func(args);
    BOOST_REQUIRE(result.has_value());

    auto mat = nested_list_to_matrix(heap, *result);
    BOOST_REQUIRE_EQUAL(mat.size(), 2u);
    BOOST_TEST(std::abs(mat[0][0] - 2.5) < 1e-10);
    BOOST_TEST(std::abs(mat[0][1] - 5.0) < 1e-10);
    BOOST_TEST(std::abs(mat[1][0] - 5.0) < 1e-10);
    BOOST_TEST(std::abs(mat[1][1] - 10.0) < 1e-10);
}


BOOST_AUTO_TEST_CASE(cor_matrix_perfect) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_stats_primitives(env, heap, intern, nullptr);

    auto ft = make_test_fact_table(heap, {"x", "y"}, {{1,2,3,4,5}, {2,4,6,8,10}});
    auto cols = ints_to_list(heap, {0, 1});

    auto idx = env.lookup("%stats-cor-matrix");
    BOOST_REQUIRE(idx.has_value());
    std::vector<LispVal> args = {ft, cols};
    auto result = env.specs()[*idx].func(args);
    BOOST_REQUIRE(result.has_value());

    auto mat = nested_list_to_matrix(heap, *result);
    BOOST_REQUIRE_EQUAL(mat.size(), 2u);
    BOOST_TEST(std::abs(mat[0][0] - 1.0) < 1e-10);
    BOOST_TEST(std::abs(mat[0][1] - 1.0) < 1e-10);
    BOOST_TEST(std::abs(mat[1][0] - 1.0) < 1e-10);
    BOOST_TEST(std::abs(mat[1][1] - 1.0) < 1e-10);
}


BOOST_AUTO_TEST_CASE(quantile_vec_median_ft) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_stats_primitives(env, heap, intern, nullptr);

    auto ft = make_test_fact_table(heap, {"x"}, {{1,2,3,4,5}});
    auto cols = ints_to_list(heap, {0});
    auto p = *ops::encode(0.5);

    auto idx = env.lookup("%stats-quantile-vec");
    BOOST_REQUIRE(idx.has_value());
    std::vector<LispVal> args = {ft, cols, p};
    auto result = env.specs()[*idx].func(args);
    BOOST_REQUIRE(result.has_value());

    auto qs = list_to_vec(heap, *result);
    BOOST_REQUIRE_EQUAL(qs.size(), 1u);
    BOOST_TEST(std::abs(qs[0] - 3.0) < 1e-10);  ///< median of [1,2,3,4,5]
}

BOOST_AUTO_TEST_CASE(quantile_vec_median_seqs) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_stats_primitives(env, heap, intern, nullptr);

    auto seqs = make_seq_list(heap, {{1,2,3,4,5}});
    auto p = *ops::encode(0.5);

    auto idx = env.lookup("%stats-quantile-vec");
    BOOST_REQUIRE(idx.has_value());
    std::vector<LispVal> args = {seqs, p};
    auto result = env.specs()[*idx].func(args);
    BOOST_REQUIRE(result.has_value());

    auto qs = list_to_vec(heap, *result);
    BOOST_REQUIRE_EQUAL(qs.size(), 1u);
    BOOST_TEST(std::abs(qs[0] - 3.0) < 1e-10);
}


BOOST_AUTO_TEST_CASE(ols_multi_simple_linear_ft) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_stats_primitives(env, heap, intern, nullptr);

    /// y = 2x + 1 (exact fit: intercept=1, slope=2)
    auto ft = make_test_fact_table(heap, {"x", "y"}, {{1,2,3,4,5}, {3,5,7,9,11}});
    auto y_col = *ops::encode(static_cast<int64_t>(1));
    auto x_cols = ints_to_list(heap, {0});

    auto idx = env.lookup("%stats-ols-multi");
    BOOST_REQUIRE(idx.has_value());
    std::vector<LispVal> args = {ft, y_col, x_cols};
    auto result = env.specs()[*idx].func(args);
    BOOST_REQUIRE(result.has_value());

    /// Walk the alist and extract coefficients
    LispVal cur = *result;
    auto* first = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
    BOOST_REQUIRE(first != nullptr);
    auto* coeff_pair = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(first->car));
    BOOST_REQUIRE(coeff_pair != nullptr);
    auto coeffs = list_to_vec(heap, coeff_pair->cdr);
    BOOST_REQUIRE_EQUAL(coeffs.size(), 2u);
    BOOST_TEST(std::abs(coeffs[0] - 1.0) < 1e-8);  ///< intercept
    BOOST_TEST(std::abs(coeffs[1] - 2.0) < 1e-8);  ///< slope

    /// Walk to r-squared (5th entry in alist)
    cur = first->cdr;
    for (int i = 0; i < 3; ++i) {
        auto* c = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
        BOOST_REQUIRE(c != nullptr);
        cur = c->cdr;
    }
    auto* r2_entry = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
    BOOST_REQUIRE(r2_entry != nullptr);
    auto* r2_pair = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(r2_entry->car));
    BOOST_REQUIRE(r2_pair != nullptr);
    auto r2v = classify_numeric(r2_pair->cdr, heap);
    BOOST_REQUIRE(r2v.is_valid());
    BOOST_TEST(std::abs(r2v.as_double() - 1.0) < 1e-8);
}

BOOST_AUTO_TEST_CASE(ols_multi_simple_linear_seqs) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_stats_primitives(env, heap, intern, nullptr);

    /// y = 2x + 1
    auto y_seq = doubles_to_list(heap, {3,5,7,9,11});
    auto x_seqs = make_seq_list(heap, {{1,2,3,4,5}});

    auto idx = env.lookup("%stats-ols-multi");
    BOOST_REQUIRE(idx.has_value());
    std::vector<LispVal> args = {y_seq, x_seqs};
    auto result = env.specs()[*idx].func(args);
    BOOST_REQUIRE(result.has_value());

    auto* first = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(*result));
    BOOST_REQUIRE(first != nullptr);
    auto* coeff_pair = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(first->car));
    BOOST_REQUIRE(coeff_pair != nullptr);
    auto coeffs = list_to_vec(heap, coeff_pair->cdr);
    BOOST_REQUIRE_EQUAL(coeffs.size(), 2u);
    BOOST_TEST(std::abs(coeffs[0] - 1.0) < 1e-8);
    BOOST_TEST(std::abs(coeffs[1] - 2.0) < 1e-8);
}

BOOST_AUTO_TEST_CASE(ols_multi_two_predictors) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_stats_primitives(env, heap, intern, nullptr);

    /// y = 1 + 2*x1 + 3*x2  (exact)
    auto ft = make_test_fact_table(heap, {"x1", "x2", "y"},
                                    {{1,0,0,1,2}, {0,1,0,1,1}, {3,4,1,6,8}});
    auto y_col = *ops::encode(static_cast<int64_t>(2));
    auto x_cols = ints_to_list(heap, {0, 1});

    auto idx = env.lookup("%stats-ols-multi");
    BOOST_REQUIRE(idx.has_value());
    std::vector<LispVal> args = {ft, y_col, x_cols};
    auto result = env.specs()[*idx].func(args);
    BOOST_REQUIRE(result.has_value());

    auto* first = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(*result));
    BOOST_REQUIRE(first != nullptr);
    auto* coeff_pair = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(first->car));
    BOOST_REQUIRE(coeff_pair != nullptr);
    auto coeffs = list_to_vec(heap, coeff_pair->cdr);
    BOOST_REQUIRE_EQUAL(coeffs.size(), 3u);
    BOOST_TEST(std::abs(coeffs[0] - 1.0) < 1e-8);  ///< intercept
    BOOST_TEST(std::abs(coeffs[1] - 2.0) < 1e-8);  ///< x1 coefficient
    BOOST_TEST(std::abs(coeffs[2] - 3.0) < 1e-8);  ///< x2 coefficient
}

/// Eigen-backed stats_math.h unit tests

BOOST_AUTO_TEST_CASE(eigen_mean_basic) {
    Eigen::VectorXd v(5);
    v << 1, 2, 3, 4, 5;
    BOOST_TEST(std::abs(stats::mean(v) - 3.0) < 1e-12);
}

BOOST_AUTO_TEST_CASE(eigen_mean_empty) {
    Eigen::VectorXd v(0);
    BOOST_TEST(stats::mean(v) == 0.0);
}

BOOST_AUTO_TEST_CASE(eigen_variance_basic) {
    Eigen::VectorXd v(3);
    v << 2, 4, 6;
    /// sample variance = ((2-4)^2 + (4-4)^2 + (6-4)^2) / 2 = 4.0
    BOOST_TEST(std::abs(stats::variance(v) - 4.0) < 1e-12);
}

BOOST_AUTO_TEST_CASE(eigen_stddev_basic) {
    Eigen::VectorXd v(3);
    v << 2, 4, 6;
    BOOST_TEST(std::abs(stats::stddev(v) - 2.0) < 1e-12);
}

BOOST_AUTO_TEST_CASE(eigen_sem_basic) {
    Eigen::VectorXd v(4);
    v << 2, 4, 6, 8;
    /// stddev = sqrt(20/3), sem = stddev / sqrt(4)
    double expected_sem = std::sqrt(20.0 / 3.0) / 2.0;
    BOOST_TEST(std::abs(stats::sem(v) - expected_sem) < 1e-12);
}

BOOST_AUTO_TEST_CASE(eigen_percentile_median) {
    Eigen::VectorXd v(5);
    v << 5, 1, 3, 2, 4;
    BOOST_TEST(std::abs(stats::percentile(v, 0.5) - 3.0) < 1e-12);
}

BOOST_AUTO_TEST_CASE(eigen_covariance_perfect) {
    Eigen::VectorXd x(5), y(5);
    x << 1, 2, 3, 4, 5;
    y << 2, 4, 6, 8, 10;
    auto cov = stats::covariance(x, y);
    BOOST_REQUIRE(cov.has_value());
    BOOST_TEST(std::abs(*cov - 5.0) < 1e-12);
}

BOOST_AUTO_TEST_CASE(eigen_correlation_perfect) {
    Eigen::VectorXd x(5), y(5);
    x << 1, 2, 3, 4, 5;
    y << 2, 4, 6, 8, 10;
    auto cor = stats::correlation(x, y);
    BOOST_REQUIRE(cor.has_value());
    BOOST_TEST(std::abs(*cor - 1.0) < 1e-10);
}

BOOST_AUTO_TEST_CASE(eigen_t_cdf_symmetry) {
    /// CDF at 0 should be 0.5 for any df
    BOOST_TEST(std::abs(stats::t_cdf(0.0, 10.0) - 0.5) < 1e-10);
    /// CDF is monotonically increasing: cdf(2, 10) > cdf(0, 10)
    BOOST_TEST(stats::t_cdf(2.0, 10.0) > 0.5);
}

BOOST_AUTO_TEST_CASE(eigen_t_quantile_roundtrip) {
    double t = 1.5, df = 15.0;
    double p = stats::t_cdf(t, df);
    double t_back = stats::t_quantile(p, df);
    BOOST_TEST(std::abs(t_back - t) < 1e-6);
}

BOOST_AUTO_TEST_CASE(eigen_ols_perfect_fit) {
    Eigen::VectorXd x(5), y(5);
    x << 1, 2, 3, 4, 5;
    y << 3, 5, 7, 9, 11;  ///< y = 2x + 1
    auto r = stats::ols(x, y);
    BOOST_REQUIRE(r.has_value());
    BOOST_TEST(std::abs(r->slope - 2.0) < 1e-8);
    BOOST_TEST(std::abs(r->intercept - 1.0) < 1e-8);
    BOOST_TEST(std::abs(r->r_squared - 1.0) < 1e-8);
}

BOOST_AUTO_TEST_CASE(eigen_t_test_identical_samples) {
    Eigen::VectorXd x(5), y(5);
    x << 1, 2, 3, 4, 5;
    y << 1, 2, 3, 4, 5;
    auto r = stats::t_test_2(x, y);
    BOOST_REQUIRE(r.has_value());
    BOOST_TEST(std::abs(r->t_stat) < 1e-10);
    BOOST_TEST(std::abs(r->mean_diff) < 1e-10);    ///< no difference
    BOOST_TEST(r->p_value > 0.99);                 ///< not significant
}

BOOST_AUTO_TEST_CASE(eigen_ci_mean_basic) {
    Eigen::VectorXd v(5);
    v << 1, 2, 3, 4, 5;
    auto ci = stats::ci_mean(v, 0.95);
    BOOST_REQUIRE(ci.has_value());
    BOOST_TEST(ci->point_estimate == 3.0);
    BOOST_TEST(ci->lower < 3.0);
    BOOST_TEST(ci->upper > 3.0);
    BOOST_TEST(ci->margin > 0.0);
}


BOOST_AUTO_TEST_CASE(stdvec_overloads_work) {
    std::vector<double> v = {1, 2, 3, 4, 5};
    BOOST_TEST(std::abs(stats::mean(v) - 3.0) < 1e-12);
    BOOST_TEST(std::abs(stats::variance(v) - 2.5) < 1e-12);
    BOOST_TEST(std::abs(stats::stddev(v) - std::sqrt(2.5)) < 1e-12);
    BOOST_TEST(std::abs(stats::percentile(v, 0.5) - 3.0) < 1e-12);

    std::vector<double> y = {2, 4, 6, 8, 10};
    auto cov = stats::covariance(v, y);
    BOOST_REQUIRE(cov.has_value());
    BOOST_TEST(std::abs(*cov - 5.0) < 1e-12);

    auto r = stats::ols(v, y);
    BOOST_REQUIRE(r.has_value());
    BOOST_TEST(std::abs(r->slope - 2.0) < 1e-8);
    BOOST_TEST(std::abs(r->intercept - 0.0) < 1e-8);
}

/// Polymorphic to_eigen extraction tests (stats_extract.h)

BOOST_AUTO_TEST_CASE(to_eigen_from_list) {
    Heap heap(1ull << 22);
    auto lst = doubles_to_list(heap, {10.0, 20.0, 30.0});
    auto result = stats::to_eigen(heap, lst, "test");
    BOOST_REQUIRE(result.has_value());
    BOOST_REQUIRE_EQUAL(result->size(), 3);
    BOOST_TEST(std::abs((*result)(0) - 10.0) < 1e-12);
    BOOST_TEST(std::abs((*result)(1) - 20.0) < 1e-12);
    BOOST_TEST(std::abs((*result)(2) - 30.0) < 1e-12);
}

BOOST_AUTO_TEST_CASE(to_eigen_from_empty_list) {
    Heap heap(1ull << 22);
    auto result = stats::to_eigen(heap, Nil, "test");
    BOOST_REQUIRE(result.has_value());
    BOOST_TEST(result->size() == 0);
}

BOOST_AUTO_TEST_CASE(to_eigen_from_vector) {
    Heap heap(1ull << 22);
    /// Build a Vector of flonums
    auto v1 = *ops::encode(1.5);
    auto v2 = *ops::encode(2.5);
    auto v3 = *ops::encode(3.5);
    auto vec = make_vector(heap, {v1, v2, v3});
    BOOST_REQUIRE(vec.has_value());

    auto result = stats::to_eigen(heap, *vec, "test");
    BOOST_REQUIRE(result.has_value());
    BOOST_REQUIRE_EQUAL(result->size(), 3);
    BOOST_TEST(std::abs((*result)(0) - 1.5) < 1e-12);
    BOOST_TEST(std::abs((*result)(1) - 2.5) < 1e-12);
    BOOST_TEST(std::abs((*result)(2) - 3.5) < 1e-12);
}

BOOST_AUTO_TEST_CASE(to_eigen_rejects_non_numeric) {
    Heap heap(1ull << 22);
    /// A symbol is not a numeric sequence
    auto sym = ops::box(Tag::Symbol, 42);
    auto result = stats::to_eigen(heap, sym, "test");
    BOOST_TEST(!result.has_value());
}

BOOST_AUTO_TEST_SUITE_END()



