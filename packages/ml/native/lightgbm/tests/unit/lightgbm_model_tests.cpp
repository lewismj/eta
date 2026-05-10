#include "eta/lightgbm/lightgbm_model.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

[[nodiscard]] bool approx_equal(const double lhs,
                                const double rhs,
                                const double tol = 1e-9) {
    return std::abs(lhs - rhs) <= tol;
}

[[nodiscard]] bool finite_non_negative(const double value) {
    return std::isfinite(value) && value >= 0.0;
}

int expect_true(const bool condition, const char* message) {
    if (condition) return 0;
    std::cerr << "lightgbm_model_tests: " << message << '\n';
    return 1;
}

} // namespace

int main() {
    using namespace eta::lightgbm_sidecar;

    int failures = 0;

    const std::vector<double> features{
        1.0, 2.0,
        2.0, 1.0,
    };
    const std::vector<float> labels{
        0.0f,
        1.0f,
    };

    auto dataset = make_dataset_from_lists(features, labels, 2, 2);
    failures += expect_true(dataset.has_value(), "dataset creation should succeed");
    if (!dataset.has_value()) {
        return failures == 0 ? 0 : 1;
    }

    auto booster = make_booster(*dataset);
    failures += expect_true(booster.has_value(), "booster creation should succeed");
    if (!booster.has_value()) {
        return failures == 0 ? 0 : 1;
    }

    auto trees_before = num_trees(*booster);
    failures += expect_true(trees_before.has_value(), "num_trees before training should succeed");
    if (trees_before.has_value()) {
        failures += expect_true(*trees_before == 0u, "fresh booster should have zero trees");
    }

    auto train = train_one_round(*booster, *dataset);
    failures += expect_true(train.has_value(), "one training round should succeed");

    auto trees_after = num_trees(*booster);
    failures += expect_true(trees_after.has_value(), "num_trees after training should succeed");
    if (trees_after.has_value()) {
        failures += expect_true(*trees_after >= 1u, "trained booster should have at least one tree");
    }

    auto prediction = predict_raw(*booster, std::vector<double>{1.0, 2.0});
    failures += expect_true(prediction.has_value(), "prediction should succeed");
    if (prediction.has_value()) {
        failures += expect_true(std::isfinite(*prediction), "prediction should be finite");
    }

    auto quality = eval_score(*booster, *dataset);
    failures += expect_true(quality.has_value(), "eval should succeed");
    if (quality.has_value()) {
        failures += expect_true(finite_non_negative(*quality), "eval score should be finite and non-negative");
    }

    auto importance = feature_importance_scalar(*booster);
    failures += expect_true(importance.has_value(), "feature importance should succeed");
    if (importance.has_value()) {
        failures += expect_true(finite_non_negative(*importance), "feature importance should be finite and non-negative");
    }

    auto serialized = serialize_booster(*booster);
    failures += expect_true(serialized.has_value(), "serialize should succeed");

    auto loaded = serialized.has_value()
        ? deserialize_booster(*serialized)
        : std::expected<BoosterModel, std::string>{std::unexpected("serialization failed")};
    failures += expect_true(loaded.has_value(), "load should succeed");
    if (loaded.has_value() && prediction.has_value()) {
        auto reloaded_prediction = predict_raw(*loaded, std::vector<double>{1.0, 2.0});
        failures += expect_true(reloaded_prediction.has_value(), "prediction after reload should succeed");
        if (reloaded_prediction.has_value()) {
            failures += expect_true(
                approx_equal(*prediction, *reloaded_prediction),
                "prediction after reload should match original prediction");
        }
    }

    return failures == 0 ? 0 : 1;
}
