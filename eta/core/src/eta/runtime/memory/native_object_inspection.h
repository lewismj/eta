#pragma once

#include <array>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>

#include <eta/runtime/memory/heap.h>

namespace eta::runtime::memory::heap {

    /**
     * @brief Return the native type name for a native-object entry, or empty.
     */
    [[nodiscard]] inline std::string native_object_type_name(const HeapEntry& entry) {
        if (entry.header.kind != ObjectKind::NativeObject) return {};
        auto* native_header = static_cast<const NativeObjectHeader*>(entry.ptr);
        if (native_header == nullptr || native_header->vtable == nullptr) return {};

        const char* type_name = native_header->vtable->type_name;
        if (type_name == nullptr || type_name[0] == '\0') return {};
        return type_name;
    }

    /**
     * @brief Native-object metadata used by debugger and diagnostics surfaces.
     */
    struct NativeObjectInspectionInfo {
        std::string kind_label;
        std::string type_name;
        std::string display;
    };

    /**
     * @brief Collect inspection metadata for a native-object heap entry.
     *
     * Returns std::nullopt when the entry is not ObjectKind::NativeObject.
     */
    [[nodiscard]] inline std::optional<NativeObjectInspectionInfo>
    native_object_inspection_info(const HeapEntry& entry) {
        if (entry.header.kind != ObjectKind::NativeObject) return std::nullopt;

        auto info = NativeObjectInspectionInfo{};
        info.kind_label = std::string(to_string(ObjectKind::NativeObject));
        info.type_name = native_object_type_name(entry);
        if (!info.type_name.empty()) {
            info.kind_label.append(":").append(info.type_name);
        }

        auto* native_header = static_cast<const NativeObjectHeader*>(entry.ptr);
        if (native_header == nullptr || native_header->vtable == nullptr) return info;
        if (native_header->vtable->display == nullptr) return info;

        std::FILE* tmp = std::tmpfile();
        if (tmp == nullptr) return info;

        native_header->vtable->display(native_header->user_data, tmp);
        (void) std::fflush(tmp);
        if (std::fseek(tmp, 0, SEEK_SET) != 0) {
            std::fclose(tmp);
            return info;
        }

        std::array<char, 256> chunk{};
        while (true) {
            const auto read = std::fread(chunk.data(), 1u, chunk.size(), tmp);
            if (read > 0u) info.display.append(chunk.data(), read);
            if (read < chunk.size()) {
                if (std::feof(tmp)) break;
                if (std::ferror(tmp)) {
                    info.display.clear();
                    break;
                }
            }
        }
        std::fclose(tmp);

        while (!info.display.empty()
               && (info.display.back() == '\n' || info.display.back() == '\r')) {
            info.display.pop_back();
        }
        return info;
    }

    /**
     * @brief Return the human-readable kind label for any heap entry.
     */
    [[nodiscard]] inline std::string heap_entry_kind_label(const HeapEntry& entry) {
        if (entry.header.kind != ObjectKind::NativeObject) {
            return std::string(to_string(entry.header.kind));
        }
        auto kind_label = std::string(to_string(ObjectKind::NativeObject));
        auto type_name = native_object_type_name(entry);
        if (!type_name.empty()) kind_label.append(":").append(type_name);
        return kind_label;
    }

} // namespace eta::runtime::memory::heap
