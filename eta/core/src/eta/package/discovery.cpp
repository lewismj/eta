#include "eta/package/discovery.h"

#include <system_error>

namespace eta::package {

namespace {

[[nodiscard]] fs::path canonicalize_path(const fs::path& path) {
    std::error_code ec;
    auto canonical = fs::weakly_canonical(path, ec);
    if (!ec) return canonical;
    return path.lexically_normal();
}

} // namespace

ManifestDiscoveryResult discover_manifest_context(const fs::path& start_dir) {
    ManifestDiscovery discovery;
    discovery.start_dir = canonicalize_path(start_dir);

    fs::path cursor = discovery.start_dir;
    while (true) {
        const auto candidate = cursor / "eta.toml";
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec) && !ec) {
            const auto manifest_path = canonicalize_path(candidate);
            auto document = read_manifest_document(manifest_path);
            if (!document) return std::unexpected(document.error());

            if (!discovery.package_manifest_path.has_value()
                && document->package.has_value()) {
                discovery.package_manifest_path = manifest_path;
            }

            if (document->workspace.has_value()) {
                discovery.workspace_manifest_path = manifest_path;
                break;
            }
        }

        const auto parent = cursor.parent_path();
        if (parent.empty() || parent == cursor) break;
        cursor = parent;
    }

    if (discovery.workspace_manifest_path.has_value()) {
        const auto workspace_root = discovery.workspace_manifest_path->parent_path();
        if (discovery.start_dir == workspace_root) {
            discovery.context = ManifestContextKind::WorkspaceRoot;
        } else if (discovery.package_manifest_path.has_value()) {
            discovery.context = ManifestContextKind::WorkspaceMember;
        } else {
            discovery.context = ManifestContextKind::WorkspaceNonMember;
        }
    } else if (discovery.package_manifest_path.has_value()) {
        discovery.context = ManifestContextKind::StandalonePackage;
    }

    if (discovery.package_manifest_path.has_value()) {
        discovery.active_manifest_path = discovery.package_manifest_path;
    } else if (discovery.workspace_manifest_path.has_value()) {
        discovery.active_manifest_path = discovery.workspace_manifest_path;
    }

    return discovery;
}

} // namespace eta::package
