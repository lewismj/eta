/**
 * @file source_file_registry.cpp
 * @brief Source-file identifier registry implementation.
 */

#include "eta/session/source_file_registry.h"

#include "eta/semantics/emitter.h"
#include "eta/util/path.h"

namespace eta::session {

namespace fs = std::filesystem;

diagnostic::FileResolver SourceFileRegistry::file_resolver() const {
    return [this](uint32_t id) -> std::string {
        auto it = file_id_to_path_.find(id);
        if (it != file_id_to_path_.end()) {
            return it->second.filename().string();
        }
        return {};
    };
}

const std::filesystem::path* SourceFileRegistry::path_for_file_id(const uint32_t id) const noexcept {
    auto it = file_id_to_path_.find(id);
    return it != file_id_to_path_.end() ? &it->second : nullptr;
}

uint32_t SourceFileRegistry::ensure_file_id(const std::filesystem::path& path) {
    auto canon = util::canonical_path_key(path);
    auto it = path_to_file_id_.find(canon);
    if (it != path_to_file_id_.end()) {
        return it->second;
    }

    const uint32_t id = next_file_id_++;
    file_id_to_path_[id] = path;
    path_to_file_id_[canon] = id;
    return id;
}

uint32_t SourceFileRegistry::file_id_for_path(const std::string& path) const {
    auto canon = util::canonical_path_key(fs::path(path));
    auto it = path_to_file_id_.find(canon);
    return it != path_to_file_id_.end() ? it->second : 0u;
}

std::set<uint32_t> SourceFileRegistry::valid_lines_for(
    const uint32_t file_id,
    const semantics::BytecodeFunctionRegistry& registry) const {
    std::set<uint32_t> lines;
    if (file_id == 0) {
        return lines;
    }

    for (const auto& fn : registry.all()) {
        for (const auto& sp : fn.source_map) {
            if (sp.file_id == file_id && sp.start.line != 0) {
                lines.insert(sp.start.line);
            }
        }
    }
    return lines;
}

uint32_t SourceFileRegistry::allocate_file_id(const std::string& raw_path) {
    return ensure_file_id(fs::path(raw_path));
}

} // namespace eta::session
