#pragma once

#include <string>
#include <utility>
#include <vector>

#include "eta/runtime/error.h"

namespace eta::runtime::ad {

    inline constexpr const char* kTagMixedTape = ":ad/mixed-tape";
    inline constexpr const char* kTagStaleRef = ":ad/stale-ref";
    inline constexpr const char* kTagNoActiveTape = ":ad/no-active-tape";
    inline constexpr const char* kTagNondiffStrict = ":ad/nondiff-strict";
    inline constexpr const char* kTagCrossVmRef = ":ad/cross-vm-ref";
    inline constexpr const char* kTagDomain = ":ad/domain";

    inline error::VMErrorField field(std::string key, int64_t value) {
        return error::VMErrorField{std::move(key), value};
    }

    inline error::VMErrorField field(std::string key, uint32_t value) {
        return error::VMErrorField{std::move(key), static_cast<int64_t>(value)};
    }

    inline error::VMErrorField field(std::string key, double value) {
        return error::VMErrorField{std::move(key), value};
    }

    inline error::VMErrorField field(std::string key, std::string value) {
        return error::VMErrorField{std::move(key), std::move(value)};
    }

    inline error::RuntimeError make_error(
        std::string tag,
        std::string message,
        std::vector<error::VMErrorField> fields = {}) {
        return error::make_tagged_error(std::move(tag), std::move(message), std::move(fields));
    }

}  // namespace eta::runtime::ad

