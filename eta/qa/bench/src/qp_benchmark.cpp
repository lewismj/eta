#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "eta/runtime/clp/linear.h"
#include "eta/runtime/clp/qp_solver.h"
#include "eta/runtime/clp/simplex.h"

namespace {

using eta::runtime::clp::LinearExpr;
using eta::runtime::clp::LinearTerm;
using eta::runtime::clp::QPModel;
using eta::runtime::clp::QPSolveResult;
using eta::runtime::clp::Simplex;
using eta::runtime::clp::SimplexDirection;
using eta::runtime::memory::heap::ObjectId;

constexpr double kTol = 1e-9;

/**
 * @brief Benchmark configuration parsed from CLI flags.
 */
struct BenchConfig {
    std::vector<std::size_t> sizes{8, 16, 24, 32};
    std::size_t repeats = 25;
    double lambda = 2.0;
    double upper_bound = 0.35;
    bool gate = false;
};

/**
 * @brief Deterministic synthetic portfolio coefficients for one benchmark size.
 */
struct ProblemData {
    std::size_t n = 0;
    std::vector<double> returns;
    std::vector<double> load_1;
    std::vector<double> load_2;
    std::vector<double> idio_diag;
    std::vector<double> lp_proxy;
};

/**
 * @brief Result of one optimizer call with comparable objective metrics.
 */
struct SolveOutcome {
    double reported_objective = 0.0;
    double true_objective = 0.0;
    std::vector<double> weights;
};

/**
 * @brief Aggregated benchmark row for one problem size.
 */
struct BenchRow {
    std::size_t n = 0;
    double effective_upper = 0.0;
    double lp_ms = 0.0;
    double qp_ms = 0.0;
    double speed_ratio = 0.0;
    double lp_true_score = 0.0;
    double qp_true_score = 0.0;
    double quality_gain = 0.0;
    double qp_parity_error = 0.0;
    double qp_obj_drift = 0.0;
    double qp_weight_drift = 0.0;
};

[[nodiscard]] ObjectId var_id(std::size_t idx) {
    return static_cast<ObjectId>(idx + 1);
}

[[nodiscard]] std::vector<std::size_t> parse_sizes_csv(const std::string& csv) {
    std::vector<std::size_t> out;
    std::stringstream ss(csv);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (token.empty()) {
            throw std::invalid_argument("empty entry in --sizes");
        }
        std::size_t pos = 0;
        unsigned long long v = 0;
        try {
            v = std::stoull(token, &pos, 10);
        } catch (const std::exception&) {
            throw std::invalid_argument("invalid size in --sizes: " + token);
        }
        if (pos != token.size()) {
            throw std::invalid_argument("invalid size in --sizes: " + token);
        }
        if (v < 2ULL) {
            throw std::invalid_argument("sizes must be >= 2");
        }
        out.push_back(static_cast<std::size_t>(v));
    }
    if (out.empty()) {
        throw std::invalid_argument("--sizes must not be empty");
    }
    return out;
}

[[nodiscard]] BenchConfig parse_args(int argc, char** argv) {
    BenchConfig cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            std::cout
                << "Usage: eta_qp_bench [--sizes N1,N2,...] [--repeats N] [--lambda X] [--upper X] [--gate]\n"
                << "Defaults: --sizes 8,16,24,32 --repeats 25 --lambda 2.0 --upper 0.35\n";
            std::exit(0);
        }
        if (arg == "--sizes") {
            if (i + 1 >= argc) throw std::invalid_argument("--sizes requires a value");
            cfg.sizes = parse_sizes_csv(argv[++i]);
            continue;
        }
        if (arg == "--repeats") {
            if (i + 1 >= argc) throw std::invalid_argument("--repeats requires a value");
            std::size_t pos = 0;
            unsigned long long v = 0;
            try {
                v = std::stoull(argv[++i], &pos, 10);
            } catch (const std::exception&) {
                throw std::invalid_argument("invalid --repeats value");
            }
            if (pos != std::string_view(argv[i]).size() || v == 0ULL) {
                throw std::invalid_argument("--repeats must be a positive integer");
            }
            cfg.repeats = static_cast<std::size_t>(v);
            continue;
        }
        if (arg == "--lambda") {
            if (i + 1 >= argc) throw std::invalid_argument("--lambda requires a value");
            char* end = nullptr;
            const double v = std::strtod(argv[++i], &end);
            if (end == argv[i] || *end != '\0' || !std::isfinite(v) || v <= 0.0) {
                throw std::invalid_argument("--lambda must be a finite positive number");
            }
            cfg.lambda = v;
            continue;
        }
        if (arg == "--upper") {
            if (i + 1 >= argc) throw std::invalid_argument("--upper requires a value");
            char* end = nullptr;
            const double v = std::strtod(argv[++i], &end);
            if (end == argv[i] || *end != '\0' || !std::isfinite(v) || v <= 0.0) {
                throw std::invalid_argument("--upper must be a finite positive number");
            }
            cfg.upper_bound = v;
            continue;
        }
        if (arg == "--gate") {
            cfg.gate = true;
            continue;
        }
        throw std::invalid_argument("unknown argument: " + std::string(arg));
    }
    return cfg;
}

[[nodiscard]] ProblemData make_problem(std::size_t n) {
    ProblemData data;
    data.n = n;
    data.returns.reserve(n);
    data.load_1.reserve(n);
    data.load_2.reserve(n);
    data.idio_diag.reserve(n);
    data.lp_proxy.reserve(n);

    for (std::size_t i = 0; i < n; ++i) {
        const double r = 0.06 + (0.002 * static_cast<double>((i * 7U) % 17U));
        const double l1 = 0.12 + (0.010 * static_cast<double>(i % 7U));
        const double l2 = 0.06 + (0.008 * static_cast<double>(i % 11U));
        const double d = 0.010 + (0.001 * static_cast<double>(i % 5U));

        data.returns.push_back(r);
        data.load_1.push_back(l1);
        data.load_2.push_back(l2);
        data.idio_diag.push_back(d);
        data.lp_proxy.push_back((l1 * l1) + (l2 * l2) + d);
    }

    return data;
}

[[nodiscard]] double dot(const std::vector<double>& a, const std::vector<double>& b) {
    double acc = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        acc += a[i] * b[i];
    }
    return acc;
}

[[nodiscard]] double true_risk(const ProblemData& data, const std::vector<double>& w) {
    const double f1 = dot(data.load_1, w);
    const double f2 = dot(data.load_2, w);

    double idio = 0.0;
    for (std::size_t i = 0; i < data.n; ++i) {
        idio += data.idio_diag[i] * w[i] * w[i];
    }
    return (f1 * f1) + (f2 * f2) + idio;
}

[[nodiscard]] double true_score(const ProblemData& data,
                                const std::vector<double>& w,
                                double lambda) {
    return dot(data.returns, w) - (lambda * true_risk(data, w));
}

[[nodiscard]] std::vector<double> witness_to_weights(
    const std::vector<std::pair<ObjectId, double>>& witness,
    std::size_t n) {
    std::vector<double> w(n, 0.0);
    for (const auto& [id, value] : witness) {
        if (id == 0) continue;
        const auto idx = static_cast<std::size_t>(id - 1);
        if (idx >= n) continue;
        double v = value;
        if (std::abs(v) <= 1e-12) v = 0.0;
        w[idx] = v;
    }
    return w;
}

void add_budget_and_bounds(Simplex& simplex, std::size_t n, double upper_bound) {
    LinearExpr budget;
    budget.constant = -1.0;
    budget.terms.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        budget.terms.push_back(LinearTerm{
            .var_id = var_id(i),
            .coef = 1.0,
        });
        simplex.assert_lower(var_id(i), eta::runtime::clp::Bound{0.0, false});
        simplex.assert_upper(var_id(i), eta::runtime::clp::Bound{upper_bound, false});
    }
    budget.canonicalize();
    simplex.add_eq(std::move(budget));
}

[[nodiscard]] SolveOutcome solve_lp_proxy(const ProblemData& data,
                                          double lambda,
                                          double upper_bound) {
    Simplex simplex;
    add_budget_and_bounds(simplex, data.n, upper_bound);

    LinearExpr objective;
    objective.constant = 0.0;
    objective.terms.reserve(data.n);
    for (std::size_t i = 0; i < data.n; ++i) {
        objective.terms.push_back(LinearTerm{
            .var_id = var_id(i),
            .coef = data.returns[i] - (lambda * data.lp_proxy[i]),
        });
    }
    objective.canonicalize();

    const auto opt = simplex.optimize(std::move(objective), SimplexDirection::Maximize, kTol);
    if (opt.status != eta::runtime::clp::SimplexOptResult::Status::Optimal) {
        throw std::runtime_error("LP proxy solve failed (non-optimal status)");
    }

    SolveOutcome out;
    out.reported_objective = opt.value;
    out.weights = witness_to_weights(opt.witness, data.n);
    out.true_objective = true_score(data, out.weights, lambda);
    return out;
}

[[nodiscard]] QPModel build_qp_model(const ProblemData& data,
                                     double lambda,
                                     double upper_bound) {
    QPModel model;
    model.vars.reserve(data.n);
    model.c = data.returns;
    model.q.resize(data.n * data.n, 0.0);

    for (std::size_t i = 0; i < data.n; ++i) {
        model.vars.push_back(var_id(i));
    }

    for (std::size_t r = 0; r < data.n; ++r) {
        for (std::size_t c = 0; c < data.n; ++c) {
            const double sigma =
                (data.load_1[r] * data.load_1[c]) +
                (data.load_2[r] * data.load_2[c]) +
                ((r == c) ? data.idio_diag[r] : 0.0);
            model.q[(r * data.n) + c] = -2.0 * lambda * sigma;
        }
    }

    model.a_eq.resize(data.n, 1.0);
    model.b_eq.push_back(1.0);

    std::vector<double> row(data.n, 0.0);
    for (std::size_t i = 0; i < data.n; ++i) {
        std::fill(row.begin(), row.end(), 0.0);
        row[i] = 1.0;
        model.a_leq.insert(model.a_leq.end(), row.begin(), row.end());
        model.b_leq.push_back(upper_bound);

        std::fill(row.begin(), row.end(), 0.0);
        row[i] = -1.0;
        model.a_leq.insert(model.a_leq.end(), row.begin(), row.end());
        model.b_leq.push_back(0.0);
    }

    return model;
}

[[nodiscard]] SolveOutcome solve_qp(const ProblemData& data,
                                    double lambda,
                                    double upper_bound) {
    const QPModel model = build_qp_model(data, lambda, upper_bound);

    std::vector<double> initial_x(data.n, 1.0 / static_cast<double>(data.n));
    auto solve = eta::runtime::clp::solve_quadratic_program(
        model, SimplexDirection::Maximize, std::move(initial_x));
    if (!solve.has_value()) {
        throw std::runtime_error(
            "QP solve failed: " + solve.error().tag + " (" + solve.error().message + ")");
    }
    if (solve->status != QPSolveResult::Status::Optimal) {
        throw std::runtime_error("QP solve failed (non-optimal status)");
    }

    SolveOutcome out;
    out.reported_objective = solve->value;
    out.weights = witness_to_weights(solve->witness, data.n);
    out.true_objective = true_score(data, out.weights, lambda);
    return out;
}

[[nodiscard]] double max_abs_weight_delta(const std::vector<double>& a,
                                          const std::vector<double>& b) {
    double d = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        d = std::max(d, std::abs(a[i] - b[i]));
    }
    return d;
}

[[nodiscard]] BenchRow run_benchmark_row(const ProblemData& data,
                                         std::size_t repeats,
                                         double lambda,
                                         double upper_bound) {
    using clock = std::chrono::steady_clock;

    BenchRow row;
    row.n = data.n;
    row.effective_upper = upper_bound;

    (void)solve_lp_proxy(data, lambda, upper_bound);
    (void)solve_qp(data, lambda, upper_bound);

    SolveOutcome lp_last;
    const auto lp_begin = clock::now();
    for (std::size_t i = 0; i < repeats; ++i) {
        lp_last = solve_lp_proxy(data, lambda, upper_bound);
    }
    const auto lp_end = clock::now();
    row.lp_ms = std::chrono::duration<double, std::milli>(lp_end - lp_begin).count() /
                static_cast<double>(repeats);

    SolveOutcome qp_first;
    SolveOutcome qp_last;
    bool first = true;
    double qp_obj_drift = 0.0;
    double qp_weight_drift = 0.0;

    const auto qp_begin = clock::now();
    for (std::size_t i = 0; i < repeats; ++i) {
        SolveOutcome current = solve_qp(data, lambda, upper_bound);
        if (first) {
            qp_first = current;
            first = false;
        } else {
            qp_obj_drift = std::max(
                qp_obj_drift,
                std::abs(current.reported_objective - qp_first.reported_objective));
            qp_weight_drift = std::max(
                qp_weight_drift,
                max_abs_weight_delta(current.weights, qp_first.weights));
        }
        qp_last = std::move(current);
    }
    const auto qp_end = clock::now();
    row.qp_ms = std::chrono::duration<double, std::milli>(qp_end - qp_begin).count() /
                static_cast<double>(repeats);

    row.speed_ratio = (row.lp_ms > 0.0) ? (row.qp_ms / row.lp_ms) : 0.0;
    row.lp_true_score = lp_last.true_objective;
    row.qp_true_score = qp_last.true_objective;
    row.quality_gain = row.qp_true_score - row.lp_true_score;
    row.qp_parity_error = std::abs(qp_last.reported_objective - qp_last.true_objective);
    row.qp_obj_drift = qp_obj_drift;
    row.qp_weight_drift = qp_weight_drift;
    return row;
}

[[nodiscard]] bool gate_pass(const BenchRow& row) {
    constexpr double kMaxParityError = 1e-6;
    constexpr double kMaxObjDrift = 1e-8;
    constexpr double kMaxWeightDrift = 1e-8;
    constexpr double kMinQualityGain = -1e-8;

    return std::isfinite(row.lp_ms) &&
           std::isfinite(row.qp_ms) &&
           std::isfinite(row.qp_parity_error) &&
           std::isfinite(row.qp_obj_drift) &&
           std::isfinite(row.qp_weight_drift) &&
           std::isfinite(row.quality_gain) &&
           (row.qp_parity_error <= kMaxParityError) &&
           (row.qp_obj_drift <= kMaxObjDrift) &&
           (row.qp_weight_drift <= kMaxWeightDrift) &&
           (row.quality_gain >= kMinQualityGain);
}

void print_report(const BenchConfig& cfg, const std::vector<BenchRow>& rows) {
    std::cout << "Eta QP benchmark (LP proxy vs convex QP)\n";
    std::cout << "sizes=";
    for (std::size_t i = 0; i < cfg.sizes.size(); ++i) {
        if (i > 0) std::cout << ",";
        std::cout << cfg.sizes[i];
    }
    std::cout << " repeats=" << cfg.repeats
              << " lambda=" << cfg.lambda
              << " upper=" << cfg.upper_bound
              << " gate=" << (cfg.gate ? "on" : "off")
              << "\n\n";

    std::cout << std::fixed << std::setprecision(6);
    std::cout
        << "| n | upper | lp_ms | qp_ms | qp/lp | lp_true | qp_true | gain | qp_parity | qp_obj_drift | qp_weight_drift |\n"
        << "|---:|------:|------:|------:|------:|--------:|--------:|-----:|----------:|-------------:|----------------:|\n";
    for (const auto& row : rows) {
        std::cout
            << "| " << row.n
            << " | " << row.effective_upper
            << " | " << row.lp_ms
            << " | " << row.qp_ms
            << " | " << row.speed_ratio
            << " | " << row.lp_true_score
            << " | " << row.qp_true_score
            << " | " << row.quality_gain
            << " | " << row.qp_parity_error
            << " | " << row.qp_obj_drift
            << " | " << row.qp_weight_drift
            << " |\n";
    }

    if (!cfg.gate) return;

    bool all_pass = true;
    std::cout << "\nRollout gate results:\n";
    for (const auto& row : rows) {
        const bool pass = gate_pass(row);
        all_pass = all_pass && pass;
        std::cout << "  n=" << row.n << " -> " << (pass ? "PASS" : "FAIL") << "\n";
    }
    std::cout << "  overall -> " << (all_pass ? "PASS" : "FAIL") << "\n";
}

} // namespace

int main(int argc, char** argv) {
    BenchConfig cfg;
    try {
        cfg = parse_args(argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << "eta_qp_bench: " << ex.what() << "\n";
        std::cerr << "Use --help for usage.\n";
        return 2;
    }

    std::vector<BenchRow> rows;
    rows.reserve(cfg.sizes.size());

    try {
        for (const auto n : cfg.sizes) {
            const ProblemData data = make_problem(n);
            const double min_upper = (1.0 / static_cast<double>(n)) + 1e-6;
            const double effective_upper = std::max(cfg.upper_bound, min_upper);
            rows.push_back(run_benchmark_row(data, cfg.repeats, cfg.lambda, effective_upper));
        }
    } catch (const std::exception& ex) {
        std::cerr << "eta_qp_bench: benchmark failed: " << ex.what() << "\n";
        return 1;
    }

    print_report(cfg, rows);

    if (!cfg.gate) return 0;
    for (const auto& row : rows) {
        if (!gate_pass(row)) return 1;
    }
    return 0;
}
