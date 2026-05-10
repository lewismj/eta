#pragma once

/**
 * @file lightgbm_model.h
 * @brief LightGBM-backed model bridge used by sidecar primitives.
 */

#include <LightGBM/c_api.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace eta::lightgbm_sidecar {

/**
 * @brief Backing storage for one LightGBM dataset.
 */
struct DatasetStorage {
    DatasetHandle handle{nullptr};
    std::int32_t row_count{0};
    std::int32_t feature_count{0};
    std::vector<double> features{};
    std::vector<float> labels{};

    DatasetStorage() = default;
    DatasetStorage(const DatasetStorage&) = delete;
    DatasetStorage& operator=(const DatasetStorage&) = delete;
    DatasetStorage(DatasetStorage&&) = delete;
    DatasetStorage& operator=(DatasetStorage&&) = delete;
    ~DatasetStorage();
};

/**
 * @brief User-visible dataset wrapper held by Eta native objects.
 */
struct DatasetModel {
    std::shared_ptr<DatasetStorage> storage{};
};

/**
 * @brief Backing storage for one LightGBM booster.
 */
struct BoosterStorage {
    BoosterHandle handle{nullptr};
    std::shared_ptr<DatasetStorage> training_data{};

    BoosterStorage() = default;
    BoosterStorage(const BoosterStorage&) = delete;
    BoosterStorage& operator=(const BoosterStorage&) = delete;
    BoosterStorage(BoosterStorage&&) = delete;
    BoosterStorage& operator=(BoosterStorage&&) = delete;
    ~BoosterStorage();
};

/**
 * @brief User-visible booster wrapper held by Eta native objects.
 */
struct BoosterModel {
    std::shared_ptr<BoosterStorage> storage{};
};

/**
 * @brief Construct one dataset from dense row-major matrix + labels.
 */
[[nodiscard]] std::expected<DatasetModel, std::string> make_dataset_from_lists(
    std::vector<double> features,
    std::vector<float> labels,
    std::int32_t row_count,
    std::int32_t feature_count);

/**
 * @brief Construct one booster from a dataset using default parameters.
 */
[[nodiscard]] std::expected<BoosterModel, std::string> make_booster(
    const DatasetModel& dataset,
    std::string_view parameters = "objective=regression metric=l2 min_data_in_leaf=1 num_leaves=8 verbosity=-1");

/**
 * @brief Run one real LightGBM training update.
 */
[[nodiscard]] std::expected<void, std::string> train_one_round(
    BoosterModel& booster,
    const DatasetModel& dataset);

/**
 * @brief Predict one scalar score from one feature row.
 */
[[nodiscard]] std::expected<double, std::string> predict_raw(
    const BoosterModel& booster,
    const std::vector<double>& feature_row);

/**
 * @brief Evaluate mean-squared error on a dataset.
 */
[[nodiscard]] std::expected<double, std::string> eval_score(
    const BoosterModel& booster,
    const DatasetModel& dataset);

/**
 * @brief Return total number of trees in the booster.
 */
[[nodiscard]] std::expected<std::uint64_t, std::string> num_trees(
    const BoosterModel& booster);

/**
 * @brief Return scalar summary of gain-based feature importance.
 */
[[nodiscard]] std::expected<double, std::string> feature_importance_scalar(
    const BoosterModel& booster);

/**
 * @brief Serialize booster to LightGBM model text.
 */
[[nodiscard]] std::expected<std::string, std::string> serialize_booster(
    const BoosterModel& booster);

/**
 * @brief Load booster from LightGBM model text.
 */
[[nodiscard]] std::expected<BoosterModel, std::string> deserialize_booster(std::string_view model_text);

} // namespace eta::lightgbm_sidecar
