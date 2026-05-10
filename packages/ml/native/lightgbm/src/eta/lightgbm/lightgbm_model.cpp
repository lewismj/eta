#include "eta/lightgbm/lightgbm_model.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <vector>

namespace eta::lightgbm_sidecar {

namespace {

[[nodiscard]] std::string last_error(std::string_view context) {
    const char* detail = LGBM_GetLastError();
    std::string message(context);
    if (detail != nullptr && detail[0] != '\0') {
        message.append(": ");
        message.append(detail);
    }
    return message;
}

[[nodiscard]] std::expected<void, std::string> check_status(int status, std::string_view context) {
    if (status == 0) return {};
    return std::unexpected(last_error(context));
}

[[nodiscard]] std::expected<std::vector<double>, std::string> predict_for_matrix(
    const BoosterHandle booster,
    const std::vector<double>& matrix,
    const std::int32_t row_count,
    const std::int32_t feature_count) {
    int64_t out_len = 0;
    auto calc = check_status(
        LGBM_BoosterCalcNumPredict(
            booster,
            row_count,
            C_API_PREDICT_NORMAL,
            0,
            -1,
            &out_len),
        "lgbm: failed to compute prediction output size");
    if (!calc) return std::unexpected(calc.error());

    if (out_len <= 0) {
        return std::unexpected(std::string("lgbm: prediction output size must be positive"));
    }

    std::vector<double> output(static_cast<std::size_t>(out_len), 0.0);
    int64_t written = 0;
    auto pred = check_status(
        LGBM_BoosterPredictForMat(
            booster,
            matrix.data(),
            C_API_DTYPE_FLOAT64,
            row_count,
            feature_count,
            1,
            C_API_PREDICT_NORMAL,
            0,
            -1,
            "",
            &written,
            output.data()),
        "lgbm: prediction failed");
    if (!pred) return std::unexpected(pred.error());

    if (written <= 0) {
        return std::unexpected(std::string("lgbm: prediction wrote no outputs"));
    }

    output.resize(static_cast<std::size_t>(written));
    return output;
}

} // namespace

DatasetStorage::~DatasetStorage() {
    if (handle != nullptr) {
        (void)LGBM_DatasetFree(handle);
        handle = nullptr;
    }
}

BoosterStorage::~BoosterStorage() {
    if (handle != nullptr) {
        (void)LGBM_BoosterFree(handle);
        handle = nullptr;
    }
}

std::expected<DatasetModel, std::string> make_dataset_from_lists(
    std::vector<double> features,
    std::vector<float> labels,
    const std::int32_t row_count,
    const std::int32_t feature_count) {
    if (row_count <= 0 || feature_count <= 0) {
        return std::unexpected(std::string("lgbm: dataset requires positive row and feature counts"));
    }

    const std::size_t expected_feature_values =
        static_cast<std::size_t>(row_count) * static_cast<std::size_t>(feature_count);
    if (features.size() != expected_feature_values) {
        return std::unexpected(std::string("lgbm: feature buffer size does not match row/feature counts"));
    }
    if (labels.size() != static_cast<std::size_t>(row_count)) {
        return std::unexpected(std::string("lgbm: label count does not match row count"));
    }

    auto storage = std::make_shared<DatasetStorage>();
    storage->features = std::move(features);
    storage->labels = std::move(labels);
    storage->row_count = row_count;
    storage->feature_count = feature_count;

    auto create = check_status(
        LGBM_DatasetCreateFromMat(
            storage->features.data(),
            C_API_DTYPE_FLOAT64,
            storage->row_count,
            storage->feature_count,
            1,
            "",
            nullptr,
            &storage->handle),
        "lgbm: failed to create dataset from matrix");
    if (!create) return std::unexpected(create.error());

    auto set_label = check_status(
        LGBM_DatasetSetField(
            storage->handle,
            "label",
            storage->labels.data(),
            storage->row_count,
            C_API_DTYPE_FLOAT32),
        "lgbm: failed to attach labels to dataset");
    if (!set_label) return std::unexpected(set_label.error());

    return DatasetModel{.storage = std::move(storage)};
}

std::expected<BoosterModel, std::string> make_booster(
    const DatasetModel& dataset,
    const std::string_view parameters) {
    if (!dataset.storage || dataset.storage->handle == nullptr) {
        return std::unexpected(std::string("lgbm: dataset handle is invalid"));
    }

    auto storage = std::make_shared<BoosterStorage>();
    auto create = check_status(
        LGBM_BoosterCreate(dataset.storage->handle, std::string(parameters).c_str(), &storage->handle),
        "lgbm: failed to create booster");
    if (!create) return std::unexpected(create.error());

    storage->training_data = dataset.storage;
    return BoosterModel{.storage = std::move(storage)};
}

std::expected<void, std::string> train_one_round(
    BoosterModel& booster,
    const DatasetModel& dataset) {
    if (!booster.storage || booster.storage->handle == nullptr) {
        return std::unexpected(std::string("lgbm: booster handle is invalid"));
    }
    if (!dataset.storage || dataset.storage->handle == nullptr) {
        return std::unexpected(std::string("lgbm: dataset handle is invalid"));
    }

    if (!booster.storage->training_data
        || booster.storage->training_data->handle != dataset.storage->handle) {
        auto reset = check_status(
            LGBM_BoosterResetTrainingData(booster.storage->handle, dataset.storage->handle),
            "lgbm: failed to reset booster training dataset");
        if (!reset) return std::unexpected(reset.error());
        booster.storage->training_data = dataset.storage;
    }

    int is_finished = 0;
    auto update = check_status(
        LGBM_BoosterUpdateOneIter(booster.storage->handle, &is_finished),
        "lgbm: failed to run one training iteration");
    if (!update) return std::unexpected(update.error());
    return {};
}

std::expected<double, std::string> predict_raw(
    const BoosterModel& booster,
    const std::vector<double>& feature_row) {
    if (!booster.storage || booster.storage->handle == nullptr) {
        return std::unexpected(std::string("lgbm: booster handle is invalid"));
    }
    if (feature_row.empty()) {
        return std::unexpected(std::string("lgbm: feature row must not be empty"));
    }

    int feature_count = 0;
    auto get_num_feature = check_status(
        LGBM_BoosterGetNumFeature(booster.storage->handle, &feature_count),
        "lgbm: failed to read booster feature count");
    if (!get_num_feature) return std::unexpected(get_num_feature.error());

    if (feature_count <= 0) {
        return std::unexpected(std::string("lgbm: booster has no features"));
    }
    if (feature_row.size() != static_cast<std::size_t>(feature_count)) {
        return std::unexpected(
            "lgbm: feature row width (" + std::to_string(feature_row.size())
            + ") does not match booster feature count (" + std::to_string(feature_count) + ")");
    }

    auto outputs = predict_for_matrix(
        booster.storage->handle,
        feature_row,
        1,
        feature_count);
    if (!outputs) return std::unexpected(outputs.error());

    return outputs->front();
}

std::expected<double, std::string> eval_score(
    const BoosterModel& booster,
    const DatasetModel& dataset) {
    if (!booster.storage || booster.storage->handle == nullptr) {
        return std::unexpected(std::string("lgbm: booster handle is invalid"));
    }
    if (!dataset.storage || dataset.storage->handle == nullptr) {
        return std::unexpected(std::string("lgbm: dataset handle is invalid"));
    }
    if (dataset.storage->row_count <= 0) {
        return std::unexpected(std::string("lgbm: dataset is empty"));
    }

    auto outputs = predict_for_matrix(
        booster.storage->handle,
        dataset.storage->features,
        dataset.storage->row_count,
        dataset.storage->feature_count);
    if (!outputs) return std::unexpected(outputs.error());

    const std::size_t rows = static_cast<std::size_t>(dataset.storage->row_count);
    if (outputs->size() < rows) {
        return std::unexpected(
            "lgbm: prediction output size (" + std::to_string(outputs->size())
            + ") is smaller than row count (" + std::to_string(rows) + ")");
    }

    const std::size_t stride = outputs->size() / rows;
    if (stride == 0u) {
        return std::unexpected(std::string("lgbm: invalid prediction stride"));
    }

    double squared_error_sum = 0.0;
    for (std::size_t i = 0; i < rows; ++i) {
        const double prediction = (*outputs)[i * stride];
        const double label = static_cast<double>(dataset.storage->labels[i]);
        const double error = prediction - label;
        squared_error_sum += error * error;
    }
    return squared_error_sum / static_cast<double>(rows);
}

std::expected<std::uint64_t, std::string> num_trees(const BoosterModel& booster) {
    if (!booster.storage || booster.storage->handle == nullptr) {
        return std::unexpected(std::string("lgbm: booster handle is invalid"));
    }

    int out_trees = 0;
    auto status = check_status(
        LGBM_BoosterNumberOfTotalModel(booster.storage->handle, &out_trees),
        "lgbm: failed to read number of trees");
    if (!status) return std::unexpected(status.error());

    if (out_trees < 0) {
        return std::unexpected(std::string("lgbm: reported negative number of trees"));
    }
    return static_cast<std::uint64_t>(out_trees);
}

std::expected<double, std::string> feature_importance_scalar(const BoosterModel& booster) {
    if (!booster.storage || booster.storage->handle == nullptr) {
        return std::unexpected(std::string("lgbm: booster handle is invalid"));
    }

    int feature_count = 0;
    auto get_count = check_status(
        LGBM_BoosterGetNumFeature(booster.storage->handle, &feature_count),
        "lgbm: failed to read feature count for importance");
    if (!get_count) return std::unexpected(get_count.error());
    if (feature_count <= 0) return 0.0;

    std::vector<double> importances(static_cast<std::size_t>(feature_count), 0.0);
    auto imp = check_status(
        LGBM_BoosterFeatureImportance(
            booster.storage->handle,
            -1,
            C_API_FEATURE_IMPORTANCE_GAIN,
            importances.data()),
        "lgbm: failed to compute feature importance");
    if (!imp) return std::unexpected(imp.error());

    return std::accumulate(importances.begin(), importances.end(), 0.0);
}

std::expected<std::string, std::string> serialize_booster(const BoosterModel& booster) {
    if (!booster.storage || booster.storage->handle == nullptr) {
        return std::unexpected(std::string("lgbm: booster handle is invalid"));
    }

    int64_t output_len = 0;
    int64_t buffer_len = 4096;
    std::vector<char> buffer(static_cast<std::size_t>(buffer_len), '\0');
    auto first_pass = check_status(
        LGBM_BoosterSaveModelToString(
            booster.storage->handle,
            0,
            -1,
            C_API_FEATURE_IMPORTANCE_GAIN,
            buffer_len,
            &output_len,
            buffer.data()),
        "lgbm: failed to serialize model");
    if (!first_pass) return std::unexpected(first_pass.error());

    if (output_len <= 0) {
        return std::unexpected(std::string("lgbm: serialized model length must be positive"));
    }

    if (output_len > buffer_len) {
        buffer_len = output_len;
        buffer.assign(static_cast<std::size_t>(buffer_len), '\0');
        auto second_pass = check_status(
            LGBM_BoosterSaveModelToString(
                booster.storage->handle,
                0,
                -1,
                C_API_FEATURE_IMPORTANCE_GAIN,
                buffer_len,
                &output_len,
                buffer.data()),
            "lgbm: failed to serialize model");
        if (!second_pass) return std::unexpected(second_pass.error());
    }

    return std::string(buffer.data());
}

std::expected<BoosterModel, std::string> deserialize_booster(const std::string_view model_text) {
    if (model_text.empty()) {
        return std::unexpected(std::string("lgbm: model text must not be empty"));
    }

    auto storage = std::make_shared<BoosterStorage>();
    int out_iterations = 0;
    auto load = check_status(
        LGBM_BoosterLoadModelFromString(
            std::string(model_text).c_str(),
            &out_iterations,
            &storage->handle),
        "lgbm: failed to deserialize model");
    if (!load) return std::unexpected(load.error());

    return BoosterModel{.storage = std::move(storage)};
}

} // namespace eta::lightgbm_sidecar
