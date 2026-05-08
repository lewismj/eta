#include "eta/runtime/prof/report.h"

#include <algorithm>
#include <sstream>
#include <vector>

#include "eta/util/json.h"

namespace eta::runtime::prof {

namespace {

struct FlatRow {
    FrameId id{0};
    FlatStats stats{};
};

struct TreeRow {
    EdgeKey edge{};
    EdgeStats stats{};
};

[[nodiscard]] std::vector<FlatRow> sorted_flat_rows(const Aggregator& aggregator) {
    const auto flat = aggregator.flat_snapshot();
    std::vector<FlatRow> flat_rows;
    flat_rows.reserve(flat.size());
    for (const auto& [frame_id, stats] : flat) {
        flat_rows.push_back(FlatRow{frame_id, stats});
    }

    std::sort(flat_rows.begin(), flat_rows.end(), [](const FlatRow& lhs, const FlatRow& rhs) {
        if (lhs.stats.self_ns != rhs.stats.self_ns) return lhs.stats.self_ns > rhs.stats.self_ns;
        if (lhs.stats.inclusive_ns != rhs.stats.inclusive_ns) {
            return lhs.stats.inclusive_ns > rhs.stats.inclusive_ns;
        }
        if (lhs.stats.bytes_allocated != rhs.stats.bytes_allocated) {
            return lhs.stats.bytes_allocated > rhs.stats.bytes_allocated;
        }
        if (lhs.stats.calls != rhs.stats.calls) return lhs.stats.calls > rhs.stats.calls;
        return lhs.id < rhs.id;
    });
    return flat_rows;
}

[[nodiscard]] std::vector<TreeRow> sorted_tree_rows(const Aggregator& aggregator) {
    const auto tree = aggregator.tree_snapshot();
    std::vector<TreeRow> tree_rows;
    tree_rows.reserve(tree.size());
    for (const auto& [edge, stats] : tree) {
        tree_rows.push_back(TreeRow{edge, stats});
    }

    std::sort(tree_rows.begin(), tree_rows.end(), [](const TreeRow& lhs, const TreeRow& rhs) {
        if (lhs.stats.inclusive_ns != rhs.stats.inclusive_ns) {
            return lhs.stats.inclusive_ns > rhs.stats.inclusive_ns;
        }
        if (lhs.stats.calls != rhs.stats.calls) return lhs.stats.calls > rhs.stats.calls;
        if (lhs.edge.parent != rhs.edge.parent) return lhs.edge.parent < rhs.edge.parent;
        return lhs.edge.child < rhs.edge.child;
    });
    return tree_rows;
}

[[nodiscard]] std::vector<std::pair<std::string, std::uint64_t>> sorted_counters(
    const std::unordered_map<std::string, std::uint64_t>& counters) {
    std::vector<std::pair<std::string, std::uint64_t>> out;
    out.reserve(counters.size());
    for (const auto& [name, value] : counters) {
        out.emplace_back(name, value);
    }
    std::sort(out.begin(),
              out.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
    return out;
}

} ///< namespace

std::string render_pretty_report(const Aggregator& aggregator,
                                 const FrameIdInterner& interner,
                                 const std::unordered_map<std::string, std::uint64_t>& counters,
                                 const std::size_t top_n) {
    const auto flat_rows = sorted_flat_rows(aggregator);
    const auto tree_rows = sorted_tree_rows(aggregator);
    std::ostringstream out;
    out << "Profiler summary\n";
    out << "flat,frame,self_ns,inclusive_ns,calls,bytes_allocated\n";

    const auto flat_limit = (std::min)(top_n, flat_rows.size());
    for (std::size_t i = 0; i < flat_limit; ++i) {
        const auto key = interner.key_for(flat_rows[i].id);
        const std::string frame_name = key ? key->qualified_name : "<unknown>";
        out << frame_name << ","
            << flat_rows[i].stats.self_ns << ","
            << flat_rows[i].stats.inclusive_ns << ","
            << flat_rows[i].stats.calls << ","
            << flat_rows[i].stats.bytes_allocated << "\n";
    }

    out << "tree,parent,child,inclusive_ns,calls\n";
    const auto tree_limit = (std::min)(top_n, tree_rows.size());
    for (std::size_t i = 0; i < tree_limit; ++i) {
        const auto parent_key = interner.key_for(tree_rows[i].edge.parent);
        const auto child_key = interner.key_for(tree_rows[i].edge.child);
        const std::string parent_name = parent_key ? parent_key->qualified_name : "<unknown>";
        const std::string child_name = child_key ? child_key->qualified_name : "<unknown>";
        out << parent_name << ","
            << child_name << ","
            << tree_rows[i].stats.inclusive_ns << ","
            << tree_rows[i].stats.calls << "\n";
    }

    const auto counter_rows = sorted_counters(counters);
    if (!counter_rows.empty()) {
        out << "counters,name,value\n";
        for (const auto& [name, value] : counter_rows) {
            out << name << "," << value << "\n";
        }
    }

    return out.str();
}

std::string render_json_report(const Aggregator& aggregator,
                               const FrameIdInterner& interner,
                               const std::unordered_map<std::string, std::uint64_t>& counters,
                               const std::size_t top_n) {
    const auto flat_rows = sorted_flat_rows(aggregator);
    const auto tree_rows = sorted_tree_rows(aggregator);
    const auto counter_rows = sorted_counters(counters);

    std::ostringstream out;
    out << "{";
    out << "\"flat\":[";
    const auto flat_limit = (std::min)(top_n, flat_rows.size());
    for (std::size_t i = 0; i < flat_limit; ++i) {
        if (i != 0) out << ",";
        const auto key = interner.key_for(flat_rows[i].id);
        const std::string frame_name = key ? key->qualified_name : "<unknown>";
        out << "{";
        out << "\"frame\":";
        eta::json::escape_string(out, frame_name);
        out << ",\"self_ns\":" << flat_rows[i].stats.self_ns;
        out << ",\"inclusive_ns\":" << flat_rows[i].stats.inclusive_ns;
        out << ",\"calls\":" << flat_rows[i].stats.calls;
        out << ",\"bytes_allocated\":" << flat_rows[i].stats.bytes_allocated;
        out << "}";
    }
    out << "],";

    out << "\"tree\":[";
    const auto tree_limit = (std::min)(top_n, tree_rows.size());
    for (std::size_t i = 0; i < tree_limit; ++i) {
        if (i != 0) out << ",";
        const auto parent_key = interner.key_for(tree_rows[i].edge.parent);
        const auto child_key = interner.key_for(tree_rows[i].edge.child);
        const std::string parent_name = parent_key ? parent_key->qualified_name : "<unknown>";
        const std::string child_name = child_key ? child_key->qualified_name : "<unknown>";
        out << "{";
        out << "\"parent\":";
        eta::json::escape_string(out, parent_name);
        out << ",\"child\":";
        eta::json::escape_string(out, child_name);
        out << ",\"inclusive_ns\":" << tree_rows[i].stats.inclusive_ns;
        out << ",\"calls\":" << tree_rows[i].stats.calls;
        out << "}";
    }
    out << "],";

    out << "\"counters\":{";
    for (std::size_t i = 0; i < counter_rows.size(); ++i) {
        if (i != 0) out << ",";
        eta::json::escape_string(out, counter_rows[i].first);
        out << ":" << counter_rows[i].second;
    }
    out << "}";
    out << "}";
    return out.str();
}

} ///< namespace eta::runtime::prof
