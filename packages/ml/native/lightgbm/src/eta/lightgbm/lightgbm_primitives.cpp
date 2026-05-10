#include "eta/lightgbm/lightgbm_primitives.h"

#include "eta/lightgbm/lightgbm_model.h"
#include "eta/runtime/error.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/types/primitive.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace eta::lightgbm_sidecar {

namespace {

using PrimitiveArgs = eta::runtime::types::PrimitiveArgs;
using PrimitiveFunc = eta::runtime::types::PrimitiveFunc;
using PrimitiveResult = std::expected<eta::runtime::nanbox::LispVal, eta::runtime::error::RuntimeError>;
using eta::runtime::error::RuntimeError;
using eta::runtime::error::RuntimeErrorCode;
using eta::runtime::error::VMError;
using eta::runtime::nanbox::LispVal;
using eta::runtime::nanbox::Nil;
using eta::runtime::nanbox::Tag;
using eta::runtime::nanbox::ops::decode;
using eta::runtime::nanbox::ops::encode;
using eta::runtime::nanbox::ops::is_boxed;
using eta::runtime::nanbox::ops::payload;
using eta::runtime::nanbox::ops::sign_extend_fixnum;
using eta::runtime::nanbox::ops::tag;

struct NativeRuntime {
    void* runtime_context{nullptr};
    EtaAllocNativeObjectFnV1 alloc_native_object{nullptr};
    EtaGetNativeObjectFnV1 get_native_object{nullptr};
};

NativeRuntime g_runtime{};
std::mutex g_saved_model_mutex;
std::unordered_map<LispVal, std::string> g_saved_models;

extern "C" void dataset_destroy(void* user_data) {
    delete static_cast<DatasetModel*>(user_data);
}

extern "C" void booster_destroy(void* user_data) {
    delete static_cast<BoosterModel*>(user_data);
}

constexpr EtaNativeObjectVTable kDatasetVTable{
    .type_name = "lgbm-dataset",
    .destroy = &dataset_destroy,
    .trace = nullptr,
    .display = nullptr,
};

constexpr EtaNativeObjectVTable kBoosterVTable{
    .type_name = "lgbm-booster",
    .destroy = &booster_destroy,
    .trace = nullptr,
    .display = nullptr,
};

[[nodiscard]] RuntimeError type_error(std::string message) {
    return RuntimeError{VMError{RuntimeErrorCode::TypeError, std::move(message)}};
}

[[nodiscard]] RuntimeError internal_error(std::string message) {
    return RuntimeError{VMError{RuntimeErrorCode::InternalError, std::move(message)}};
}

[[nodiscard]] PrimitiveResult encode_fixnum(const std::int64_t value) {
    auto boxed = encode(value);
    if (!boxed) return std::unexpected(RuntimeError{boxed.error()});
    return *boxed;
}

[[nodiscard]] PrimitiveResult encode_flonum(const double value) {
    auto boxed = encode(value);
    if (!boxed) return std::unexpected(RuntimeError{boxed.error()});
    return *boxed;
}

template <typename Payload>
[[nodiscard]] Payload* get_payload(const LispVal value, const EtaNativeObjectVTable* vtable) {
    if (g_runtime.runtime_context == nullptr || g_runtime.get_native_object == nullptr) {
        return nullptr;
    }
    return static_cast<Payload*>(
        g_runtime.get_native_object(g_runtime.runtime_context, value, vtable));
}

[[nodiscard]] PrimitiveResult alloc_payload(const EtaNativeObjectVTable* vtable, void* payload_ptr) {
    if (g_runtime.runtime_context == nullptr || g_runtime.alloc_native_object == nullptr) {
        return std::unexpected(internal_error("lgbm: native-object allocator unavailable"));
    }

    std::uint64_t raw_value = 0;
    const int status = g_runtime.alloc_native_object(
        g_runtime.runtime_context,
        vtable,
        payload_ptr,
        &raw_value);
    if (status != ETA_NATIVE_STATUS_OK) {
        return std::unexpected(internal_error("lgbm: failed to allocate native object"));
    }
    return static_cast<LispVal>(raw_value);
}

[[nodiscard]] std::expected<double, RuntimeError> decode_scalar(
    const LispVal value,
    const char* who,
    const char* what) {
    double numeric = 0.0;
    if (is_boxed(value)) {
        if (tag(value) != Tag::Fixnum) {
            return std::unexpected(type_error(
                std::string(who) + ": " + what + " must be numeric"));
        }
        numeric = static_cast<double>(sign_extend_fixnum(payload(value)));
    } else {
        auto decoded = decode<double>(value);
        if (!decoded) {
            return std::unexpected(type_error(
                std::string(who) + ": " + what + " must be numeric"));
        }
        numeric = *decoded;
    }

    if (!std::isfinite(numeric)) {
        return std::unexpected(type_error(
            std::string(who) + ": " + what + " must be finite"));
    }
    return numeric;
}

[[nodiscard]] std::expected<std::int64_t, RuntimeError> decode_non_negative_int(
    const LispVal value,
    const char* who,
    const char* what) {
    auto scalar = decode_scalar(value, who, what);
    if (!scalar) return std::unexpected(scalar.error());

    const double integral = std::floor(*scalar);
    if (integral != *scalar) {
        return std::unexpected(type_error(
            std::string(who) + ": " + what + " must be an integer"));
    }
    if (integral < 0.0) {
        return std::unexpected(type_error(
            std::string(who) + ": " + what + " must be non-negative"));
    }
    if (integral > static_cast<double>((std::numeric_limits<std::int64_t>::max)())) {
        return std::unexpected(type_error(
            std::string(who) + ": " + what + " exceeds int64 range"));
    }
    return static_cast<std::int64_t>(integral);
}

[[nodiscard]] std::expected<LispVal, RuntimeError> decode_path_token(
    const LispVal value,
    const char* who,
    const char* what) {
    if (!is_boxed(value) || tag(value) != Tag::String) {
        return std::unexpected(type_error(
            std::string(who) + ": " + what + " must be a string"));
    }
    return value;
}

[[nodiscard]] std::expected<std::pair<std::int32_t, std::int32_t>, RuntimeError>
decode_dataset_shape(const PrimitiveArgs args) {
    auto rows = decode_non_negative_int(args[0], "lgbm/dataset-from-list", "row count");
    if (!rows) return std::unexpected(rows.error());
    auto cols = decode_non_negative_int(args[1], "lgbm/dataset-from-list", "feature count");
    if (!cols) return std::unexpected(cols.error());

    if (*rows <= 0 || *cols <= 0) {
        return std::unexpected(type_error(
            "lgbm/dataset-from-list: row and feature counts must be positive"));
    }
    if (*rows > static_cast<std::int64_t>((std::numeric_limits<std::int32_t>::max)())) {
        return std::unexpected(type_error(
            "lgbm/dataset-from-list: row count exceeds int32 limit"));
    }
    if (*cols > static_cast<std::int64_t>((std::numeric_limits<std::int32_t>::max)())) {
        return std::unexpected(type_error(
            "lgbm/dataset-from-list: feature count exceeds int32 limit"));
    }

    return std::make_pair(static_cast<std::int32_t>(*rows), static_cast<std::int32_t>(*cols));
}

PrimitiveResult primitive_dataset_from_list(const PrimitiveArgs args) {
    if (args.size() < 2u) {
        return std::unexpected(type_error(
            "lgbm/dataset-from-list: expected row count, feature count, labels, and features"));
    }

    auto shape = decode_dataset_shape(args);
    if (!shape) return std::unexpected(shape.error());

    const std::int32_t row_count = shape->first;
    const std::int32_t feature_count = shape->second;
    const std::size_t label_count = static_cast<std::size_t>(row_count);
    const std::size_t feature_value_count =
        static_cast<std::size_t>(row_count) * static_cast<std::size_t>(feature_count);
    const std::size_t expected_arg_count = 2u + label_count + feature_value_count;

    if (args.size() != expected_arg_count) {
        return std::unexpected(type_error(
            "lgbm/dataset-from-list: flattened argument count does not match row/feature counts"));
    }

    std::vector<float> labels;
    labels.reserve(label_count);
    for (std::size_t i = 0; i < label_count; ++i) {
        auto label = decode_scalar(
            args[2u + i],
            "lgbm/dataset-from-list",
            "label");
        if (!label) return std::unexpected(label.error());
        labels.push_back(static_cast<float>(*label));
    }

    std::vector<double> features;
    features.reserve(feature_value_count);
    for (std::size_t i = 0; i < feature_value_count; ++i) {
        auto value = decode_scalar(
            args[2u + label_count + i],
            "lgbm/dataset-from-list",
            "feature value");
        if (!value) return std::unexpected(value.error());
        features.push_back(*value);
    }

    auto dataset = make_dataset_from_lists(
        std::move(features),
        std::move(labels),
        row_count,
        feature_count);
    if (!dataset) {
        return std::unexpected(internal_error(
            "lgbm/dataset-from-list: " + dataset.error()));
    }

    auto payload_ptr = std::make_unique<DatasetModel>(std::move(*dataset));
    auto boxed = alloc_payload(&kDatasetVTable, payload_ptr.get());
    if (!boxed) return boxed;
    payload_ptr.release();
    return boxed;
}

PrimitiveResult primitive_booster_create(const PrimitiveArgs args) {
    if (args.size() != 1u) {
        return std::unexpected(type_error("lgbm/booster-create: expected dataset argument"));
    }

    auto* dataset = get_payload<DatasetModel>(args[0], &kDatasetVTable);
    if (dataset == nullptr) {
        return std::unexpected(type_error("lgbm/booster-create: expected lgbm dataset"));
    }

    auto booster = make_booster(*dataset);
    if (!booster) {
        return std::unexpected(internal_error(
            "lgbm/booster-create: " + booster.error()));
    }

    auto payload_ptr = std::make_unique<BoosterModel>(std::move(*booster));
    auto boxed = alloc_payload(&kBoosterVTable, payload_ptr.get());
    if (!boxed) return boxed;
    payload_ptr.release();
    return boxed;
}

PrimitiveResult primitive_train(const PrimitiveArgs args) {
    if (args.size() != 2u) {
        return std::unexpected(type_error("lgbm/train!: expected booster and dataset"));
    }

    auto* booster = get_payload<BoosterModel>(args[0], &kBoosterVTable);
    auto* dataset = get_payload<DatasetModel>(args[1], &kDatasetVTable);
    if (booster == nullptr || dataset == nullptr) {
        return std::unexpected(type_error("lgbm/train!: invalid booster or dataset"));
    }

    auto trained = train_one_round(*booster, *dataset);
    if (!trained) {
        return std::unexpected(internal_error(
            "lgbm/train!: " + trained.error()));
    }
    return args[0];
}

PrimitiveResult primitive_predict(const PrimitiveArgs args) {
    if (args.size() < 2u) {
        return std::unexpected(type_error(
            "lgbm/predict: expected booster and at least one feature value"));
    }

    auto* booster = get_payload<BoosterModel>(args[0], &kBoosterVTable);
    if (booster == nullptr) {
        return std::unexpected(type_error("lgbm/predict: expected lgbm booster"));
    }

    std::vector<double> feature_row;
    feature_row.reserve(args.size() - 1u);
    for (std::size_t i = 1u; i < args.size(); ++i) {
        auto value = decode_scalar(args[i], "lgbm/predict", "feature value");
        if (!value) return std::unexpected(value.error());
        feature_row.push_back(*value);
    }

    auto prediction = predict_raw(*booster, feature_row);
    if (!prediction) {
        return std::unexpected(internal_error(
            "lgbm/predict: " + prediction.error()));
    }
    return encode_flonum(*prediction);
}

PrimitiveResult primitive_save(const PrimitiveArgs args) {
    if (args.size() != 2u) {
        return std::unexpected(type_error("lgbm/save: expected booster and output path"));
    }

    auto* booster = get_payload<BoosterModel>(args[0], &kBoosterVTable);
    if (booster == nullptr) {
        return std::unexpected(type_error("lgbm/save: expected lgbm booster"));
    }

    auto key = decode_path_token(args[1], "lgbm/save", "second argument");
    if (!key) return std::unexpected(key.error());

    auto serialized = serialize_booster(*booster);
    if (!serialized) {
        return std::unexpected(internal_error(
            "lgbm/save: " + serialized.error()));
    }

    {
        const std::lock_guard<std::mutex> guard(g_saved_model_mutex);
        g_saved_models[*key] = std::move(*serialized);
    }
    return Nil;
}

PrimitiveResult primitive_load(const PrimitiveArgs args) {
    if (args.size() != 1u) {
        return std::unexpected(type_error("lgbm/load: expected input path"));
    }

    auto key = decode_path_token(args[0], "lgbm/load", "first argument");
    if (!key) return std::unexpected(key.error());

    std::string serialized;
    {
        const std::lock_guard<std::mutex> guard(g_saved_model_mutex);
        auto found = g_saved_models.find(*key);
        if (found == g_saved_models.end()) {
            return std::unexpected(type_error(
                "lgbm/load: no model has been saved for this path"));
        }
        serialized = found->second;
    }

    auto loaded = deserialize_booster(serialized);
    if (!loaded) {
        return std::unexpected(internal_error(
            "lgbm/load: " + loaded.error()));
    }

    auto payload_ptr = std::make_unique<BoosterModel>(std::move(*loaded));
    auto boxed = alloc_payload(&kBoosterVTable, payload_ptr.get());
    if (!boxed) return boxed;
    payload_ptr.release();
    return boxed;
}

PrimitiveResult primitive_eval(const PrimitiveArgs args) {
    if (args.size() != 2u) {
        return std::unexpected(type_error("lgbm/eval: expected booster and dataset"));
    }

    auto* booster = get_payload<BoosterModel>(args[0], &kBoosterVTable);
    auto* dataset = get_payload<DatasetModel>(args[1], &kDatasetVTable);
    if (booster == nullptr || dataset == nullptr) {
        return std::unexpected(type_error("lgbm/eval: invalid booster or dataset"));
    }

    auto score = eval_score(*booster, *dataset);
    if (!score) {
        return std::unexpected(internal_error(
            "lgbm/eval: " + score.error()));
    }
    return encode_flonum(*score);
}

PrimitiveResult primitive_num_trees(const PrimitiveArgs args) {
    if (args.size() != 1u) {
        return std::unexpected(type_error("lgbm/num-trees: expected booster"));
    }

    auto* booster = get_payload<BoosterModel>(args[0], &kBoosterVTable);
    if (booster == nullptr) {
        return std::unexpected(type_error("lgbm/num-trees: expected lgbm booster"));
    }

    auto trees = num_trees(*booster);
    if (!trees) {
        return std::unexpected(internal_error(
            "lgbm/num-trees: " + trees.error()));
    }
    if (*trees > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        return std::unexpected(internal_error(
            "lgbm/num-trees: value exceeds Eta fixnum range"));
    }
    return encode_fixnum(static_cast<std::int64_t>(*trees));
}

PrimitiveResult primitive_feature_importance(const PrimitiveArgs args) {
    if (args.size() != 1u) {
        return std::unexpected(type_error("lgbm/feature-importance: expected booster"));
    }

    auto* booster = get_payload<BoosterModel>(args[0], &kBoosterVTable);
    if (booster == nullptr) {
        return std::unexpected(type_error("lgbm/feature-importance: expected lgbm booster"));
    }

    auto importance = feature_importance_scalar(*booster);
    if (!importance) {
        return std::unexpected(internal_error(
            "lgbm/feature-importance: " + importance.error()));
    }
    return encode_flonum(*importance);
}

PrimitiveFunc g_dataset_from_list = primitive_dataset_from_list;
PrimitiveFunc g_booster_create = primitive_booster_create;
PrimitiveFunc g_train = primitive_train;
PrimitiveFunc g_predict = primitive_predict;
PrimitiveFunc g_save = primitive_save;
PrimitiveFunc g_load = primitive_load;
PrimitiveFunc g_eval = primitive_eval;
PrimitiveFunc g_num_trees = primitive_num_trees;
PrimitiveFunc g_feature_importance = primitive_feature_importance;

[[nodiscard]] bool native_object_api_available(const EtaNativeApiV1* api) {
    if (api == nullptr || api->runtime_context == nullptr) return false;

    const bool has_alloc = ETA_NATIVE_API_V1_HAS_FIELD(api, alloc_native_object)
        && api->alloc_native_object != nullptr;
    const bool has_get = ETA_NATIVE_API_V1_HAS_FIELD(api, get_native_object)
        && api->get_native_object != nullptr;
    return has_alloc && has_get;
}

int register_one(const EtaNativeApiV1* api,
                 const char* name,
                 const std::uint32_t arity,
                 const std::uint8_t has_rest,
                 void* callable) {
    if (api == nullptr || api->register_primitive == nullptr) {
        return ETA_NATIVE_STATUS_ERROR;
    }
    return api->register_primitive(api->user_data, name, arity, has_rest, callable);
}

} // namespace

int register_lightgbm_primitives(const EtaNativeApiV1* api) {
    if (api == nullptr || api->register_primitive == nullptr) {
        return ETA_NATIVE_STATUS_ERROR;
    }

    if (!native_object_api_available(api)) {
        if (api->report_error != nullptr) {
            api->report_error(
                api->user_data,
                "lgbm sidecar requires NativeObject API support");
        }
        return ETA_NATIVE_STATUS_ERROR;
    }

    g_runtime.runtime_context = api->runtime_context;
    g_runtime.alloc_native_object = api->alloc_native_object;
    g_runtime.get_native_object = api->get_native_object;

    if (register_one(
            api,
            "lgbm/dataset-from-list",
            2u,
            1u,
            static_cast<void*>(&g_dataset_from_list))
        != ETA_NATIVE_STATUS_OK) {
        return ETA_NATIVE_STATUS_ERROR;
    }

    if (register_one(
            api,
            "lgbm/booster-create",
            1u,
            0u,
            static_cast<void*>(&g_booster_create))
        != ETA_NATIVE_STATUS_OK) {
        return ETA_NATIVE_STATUS_ERROR;
    }

    if (register_one(api, "lgbm/train!", 2u, 0u, static_cast<void*>(&g_train))
        != ETA_NATIVE_STATUS_OK) {
        return ETA_NATIVE_STATUS_ERROR;
    }

    if (register_one(api, "lgbm/predict", 1u, 1u, static_cast<void*>(&g_predict))
        != ETA_NATIVE_STATUS_OK) {
        return ETA_NATIVE_STATUS_ERROR;
    }

    if (register_one(api, "lgbm/save", 2u, 0u, static_cast<void*>(&g_save))
        != ETA_NATIVE_STATUS_OK) {
        return ETA_NATIVE_STATUS_ERROR;
    }

    if (register_one(api, "lgbm/load", 1u, 0u, static_cast<void*>(&g_load))
        != ETA_NATIVE_STATUS_OK) {
        return ETA_NATIVE_STATUS_ERROR;
    }

    if (register_one(api, "lgbm/eval", 2u, 0u, static_cast<void*>(&g_eval))
        != ETA_NATIVE_STATUS_OK) {
        return ETA_NATIVE_STATUS_ERROR;
    }

    if (register_one(api, "lgbm/num-trees", 1u, 0u, static_cast<void*>(&g_num_trees))
        != ETA_NATIVE_STATUS_OK) {
        return ETA_NATIVE_STATUS_ERROR;
    }

    if (register_one(
            api,
            "lgbm/feature-importance",
            1u,
            0u,
            static_cast<void*>(&g_feature_importance))
        != ETA_NATIVE_STATUS_OK) {
        return ETA_NATIVE_STATUS_ERROR;
    }

    return ETA_NATIVE_STATUS_OK;
}

} // namespace eta::lightgbm_sidecar
