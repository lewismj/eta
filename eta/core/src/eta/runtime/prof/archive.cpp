#include "eta/runtime/prof/archive.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "eta/runtime/prof/chrome.h"
#include "eta/runtime/prof/report.h"
#include "eta/runtime/prof/speedscope.h"
#include "eta/util/json.h"

namespace eta::runtime::prof {

namespace {

[[nodiscard]] eta::json::Value json_u64(const std::uint64_t value) {
    if (value <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        return eta::json::Value(static_cast<std::int64_t>(value));
    }
    return eta::json::Value(static_cast<double>(value));
}

[[nodiscard]] std::expected<std::uint64_t, std::string> parse_u64(
    const eta::json::Value& value,
    std::string_view field_name) {
    if (value.is_int()) {
        const auto parsed = value.as_int();
        if (parsed < 0) {
            return std::unexpected("eta-prof: field '" + std::string(field_name)
                                   + "' must be non-negative");
        }
        return static_cast<std::uint64_t>(parsed);
    }
    if (value.is_double()) {
        const auto parsed = value.as_double();
        if (parsed < 0.0) {
            return std::unexpected("eta-prof: field '" + std::string(field_name)
                                   + "' must be non-negative");
        }
        return static_cast<std::uint64_t>(parsed);
    }
    return std::unexpected("eta-prof: field '" + std::string(field_name) + "' must be numeric");
}

[[nodiscard]] std::expected<FrameKind, std::string> parse_frame_kind(std::string_view text) {
    if (text == "eta-function") return FrameKind::EtaFunction;
    if (text == "builtin") return FrameKind::Builtin;
    if (text == "anonymous-lambda") return FrameKind::AnonymousLambda;
    if (text == "top-level") return FrameKind::TopLevel;
    if (text == "continuation-resume") return FrameKind::ContinuationResume;
    if (text == "user-region") return FrameKind::UserRegion;
    return std::unexpected("eta-prof: unknown frame kind '" + std::string(text) + "'");
}

[[nodiscard]] eta::json::Value serialize_span(const eta::reader::lexer::Span& span) {
    return eta::json::object({
        {"file_id", eta::json::Value(static_cast<std::int64_t>(span.file_id))},
        {"start_line", eta::json::Value(static_cast<std::int64_t>(span.start.line))},
        {"start_column", eta::json::Value(static_cast<std::int64_t>(span.start.column))},
        {"end_line", eta::json::Value(static_cast<std::int64_t>(span.end.line))},
        {"end_column", eta::json::Value(static_cast<std::int64_t>(span.end.column))},
    });
}

[[nodiscard]] std::expected<eta::reader::lexer::Span, std::string> parse_span(
    const eta::json::Value& value) {
    if (!value.is_object()) {
        return std::unexpected("eta-prof: frame span must be an object");
    }

    eta::reader::lexer::Span span{};
    const auto file_id = parse_u64(value["file_id"], "file_id");
    if (!file_id) return std::unexpected(file_id.error());
    span.file_id = static_cast<std::uint32_t>(*file_id);

    const auto start_line = parse_u64(value["start_line"], "start_line");
    if (!start_line) return std::unexpected(start_line.error());
    span.start.line = static_cast<std::uint32_t>(*start_line);

    const auto start_column = parse_u64(value["start_column"], "start_column");
    if (!start_column) return std::unexpected(start_column.error());
    span.start.column = static_cast<std::uint32_t>(*start_column);

    const auto end_line = parse_u64(value["end_line"], "end_line");
    if (!end_line) return std::unexpected(end_line.error());
    span.end.line = static_cast<std::uint32_t>(*end_line);

    const auto end_column = parse_u64(value["end_column"], "end_column");
    if (!end_column) return std::unexpected(end_column.error());
    span.end.column = static_cast<std::uint32_t>(*end_column);
    return span;
}

[[nodiscard]] bool valid_frame_id(const std::size_t frame_count, const FrameId frame_id) {
    return static_cast<std::size_t>(frame_id) < frame_count;
}

[[nodiscard]] std::uint64_t sample_weight_ns(const SpeedscopeThreadProfile& profile,
                                             const std::size_t sample_index) noexcept {
    if (sample_index + 1u < profile.timestamps_ns.size()) {
        const auto begin = profile.timestamps_ns[sample_index];
        const auto end = profile.timestamps_ns[sample_index + 1u];
        if (end > begin) return end - begin;
    }
    if (sample_index > 0u && sample_index < profile.timestamps_ns.size()) {
        const auto prev = profile.timestamps_ns[sample_index - 1u];
        const auto now = profile.timestamps_ns[sample_index];
        if (now > prev) return now - prev;
    }
    return 1u;
}

void build_interner_from_frames(const std::vector<FrameKey>& frames, FrameIdInterner& interner) {
    for (const auto& frame_key : frames) {
        (void) interner.intern(frame_key);
    }
}

void aggregate_from_sample_profiles(const ArchiveSession& session, Aggregator& aggregator) {
    for (const auto& profile : session.sample_profiles) {
        const auto sample_count = (std::min)(profile.samples.size(), profile.timestamps_ns.size());
        for (std::size_t i = 0; i < sample_count; ++i) {
            const auto weight_ns = sample_weight_ns(profile, i);
            const auto& stack = profile.samples[i];
            if (stack.empty()) continue;

            for (const auto frame_id : stack) {
                aggregator.record_flat(frame_id, 0, weight_ns, 1);
            }

            const auto leaf = stack.back();
            aggregator.record_flat(leaf, weight_ns, 0, 0);

            for (std::size_t depth = 0; depth + 1u < stack.size(); ++depth) {
                aggregator.record_edge(stack[depth], stack[depth + 1u], weight_ns, 1);
            }
        }
    }
}

[[nodiscard]] std::vector<SpeedscopeThreadProfile> trace_summary_profiles(const ArchiveSession& session) {
    struct Row {
        FrameId frame_id{0};
        FlatStats stats{};
    };
    std::vector<Row> rows;
    rows.reserve(session.flat_rows.size());
    for (const auto& row : session.flat_rows) {
        rows.push_back(Row{row.frame_id, row.stats});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& lhs, const Row& rhs) {
        if (lhs.stats.inclusive_ns != rhs.stats.inclusive_ns) {
            return lhs.stats.inclusive_ns > rhs.stats.inclusive_ns;
        }
        if (lhs.stats.self_ns != rhs.stats.self_ns) return lhs.stats.self_ns > rhs.stats.self_ns;
        return lhs.frame_id < rhs.frame_id;
    });

    SpeedscopeThreadProfile profile;
    profile.name = "trace-summary";
    profile.timestamps_ns.reserve(rows.size());
    profile.samples.reserve(rows.size());

    std::uint64_t now_ns = 0;
    for (const auto& row : rows) {
        const auto duration = row.stats.inclusive_ns > 0 ? row.stats.inclusive_ns : 1u;
        now_ns += duration;
        profile.timestamps_ns.push_back(now_ns);
        profile.samples.push_back({row.frame_id});
    }
    return {std::move(profile)};
}

void sort_archive_rows(ArchiveSession& session) {
    std::sort(session.flat_rows.begin(),
              session.flat_rows.end(),
              [](const ArchiveFlatRow& lhs, const ArchiveFlatRow& rhs) {
                  return lhs.frame_id < rhs.frame_id;
              });
    std::sort(session.tree_rows.begin(),
              session.tree_rows.end(),
              [](const ArchiveTreeRow& lhs, const ArchiveTreeRow& rhs) {
                  if (lhs.parent_frame_id != rhs.parent_frame_id) {
                      return lhs.parent_frame_id < rhs.parent_frame_id;
                  }
                  return lhs.child_frame_id < rhs.child_frame_id;
              });
}

} ///< namespace

const char* to_string(const ArchiveMode mode) noexcept {
    switch (mode) {
        case ArchiveMode::Trace: return "trace";
        case ArchiveMode::Sample: return "sample";
    }
    return "trace";
}

std::expected<ArchiveMode, std::string> parse_archive_mode(const std::string_view text) {
    if (text == "trace") return ArchiveMode::Trace;
    if (text == "sample") return ArchiveMode::Sample;
    return std::unexpected("eta-prof: unknown mode '" + std::string(text) + "'");
}

std::string write_eta_prof_archive(const ArchiveSession& session) {
    eta::json::Array frames_json;
    frames_json.reserve(session.frames.size());
    for (const auto& frame : session.frames) {
        frames_json.push_back(eta::json::object({
            {"kind", eta::json::Value(std::string(to_string(frame.kind)))},
            {"name", eta::json::Value(frame.qualified_name)},
            {"span", serialize_span(frame.source_span)},
        }));
    }

    eta::json::Array flat_json;
    flat_json.reserve(session.flat_rows.size());
    for (const auto& row : session.flat_rows) {
        flat_json.push_back(eta::json::object({
            {"frame", eta::json::Value(static_cast<std::int64_t>(row.frame_id))},
            {"self_ns", json_u64(row.stats.self_ns)},
            {"inclusive_ns", json_u64(row.stats.inclusive_ns)},
            {"calls", json_u64(row.stats.calls)},
            {"bytes_allocated", json_u64(row.stats.bytes_allocated)},
        }));
    }

    eta::json::Array tree_json;
    tree_json.reserve(session.tree_rows.size());
    for (const auto& row : session.tree_rows) {
        tree_json.push_back(eta::json::object({
            {"parent", eta::json::Value(static_cast<std::int64_t>(row.parent_frame_id))},
            {"child", eta::json::Value(static_cast<std::int64_t>(row.child_frame_id))},
            {"inclusive_ns", json_u64(row.stats.inclusive_ns)},
            {"calls", json_u64(row.stats.calls)},
        }));
    }

    eta::json::Array profiles_json;
    profiles_json.reserve(session.sample_profiles.size());
    for (const auto& profile : session.sample_profiles) {
        eta::json::Array timestamps_json;
        timestamps_json.reserve(profile.timestamps_ns.size());
        for (const auto timestamp_ns : profile.timestamps_ns) {
            timestamps_json.push_back(json_u64(timestamp_ns));
        }

        eta::json::Array samples_json;
        samples_json.reserve(profile.samples.size());
        for (const auto& sample_stack : profile.samples) {
            eta::json::Array stack_json;
            stack_json.reserve(sample_stack.size());
            for (const auto frame_id : sample_stack) {
                stack_json.push_back(eta::json::Value(static_cast<std::int64_t>(frame_id)));
            }
            samples_json.push_back(eta::json::Value(std::move(stack_json)));
        }

        profiles_json.push_back(eta::json::object({
            {"name", eta::json::Value(profile.name)},
            {"timestamps_ns", eta::json::Value(std::move(timestamps_json))},
            {"samples", eta::json::Value(std::move(samples_json))},
        }));
    }

    eta::json::Object counters_json;
    for (const auto& [name, value] : session.counters) {
        counters_json.insert_or_assign(name, json_u64(value));
    }

    eta::json::Object root;
    root.insert_or_assign("format", eta::json::Value("eta-prof"));
    root.insert_or_assign("version", eta::json::Value(static_cast<std::int64_t>(ArchiveSession::kVersion)));
    root.insert_or_assign("mode", eta::json::Value(std::string(to_string(session.mode))));
    root.insert_or_assign("frames", eta::json::Value(std::move(frames_json)));
    root.insert_or_assign("flat", eta::json::Value(std::move(flat_json)));
    root.insert_or_assign("tree", eta::json::Value(std::move(tree_json)));
    root.insert_or_assign("profiles", eta::json::Value(std::move(profiles_json)));
    root.insert_or_assign("counters", eta::json::Value(std::move(counters_json)));
    return eta::json::to_string(eta::json::Value(std::move(root)));
}

std::expected<ArchiveSession, std::string> parse_eta_prof_archive(const std::string_view input) {
    eta::json::Value root;
    try {
        root = eta::json::parse(input);
    } catch (const std::exception& ex) {
        return std::unexpected("eta-prof: " + std::string(ex.what()));
    }

    if (!root.is_object()) {
        return std::unexpected("eta-prof: root JSON value must be an object");
    }

    const auto& format = root["format"];
    if (format.is_string() && format.as_string() != "eta-prof") {
        return std::unexpected("eta-prof: unsupported format marker '" + format.as_string() + "'");
    }

    const auto& mode_value = root["mode"];
    if (!mode_value.is_string()) {
        return std::unexpected("eta-prof: field 'mode' must be a string");
    }
    auto parsed_mode = parse_archive_mode(mode_value.as_string());
    if (!parsed_mode) return std::unexpected(parsed_mode.error());

    ArchiveSession session;
    session.mode = *parsed_mode;

    const auto& frames_value = root["frames"];
    if (!frames_value.is_array()) {
        return std::unexpected("eta-prof: field 'frames' must be an array");
    }
    for (const auto& frame_value : frames_value.as_array()) {
        if (!frame_value.is_object()) {
            return std::unexpected("eta-prof: frame entries must be objects");
        }
        const auto kind_value = frame_value["kind"];
        if (!kind_value.is_string()) {
            return std::unexpected("eta-prof: frame.kind must be a string");
        }
        const auto name_value = frame_value["name"];
        if (!name_value.is_string()) {
            return std::unexpected("eta-prof: frame.name must be a string");
        }
        auto kind = parse_frame_kind(kind_value.as_string());
        if (!kind) return std::unexpected(kind.error());

        FrameKey key;
        key.kind = *kind;
        key.qualified_name = name_value.as_string();

        const auto& span_value = frame_value["span"];
        if (!span_value.is_null()) {
            auto parsed_span = parse_span(span_value);
            if (!parsed_span) return std::unexpected(parsed_span.error());
            key.source_span = *parsed_span;
        }
        session.frames.push_back(std::move(key));
    }

    const auto frame_count = session.frames.size();

    const auto& flat_value = root["flat"];
    if (!flat_value.is_null()) {
        if (!flat_value.is_array()) {
            return std::unexpected("eta-prof: field 'flat' must be an array");
        }
        for (const auto& row_value : flat_value.as_array()) {
            if (!row_value.is_object()) {
                return std::unexpected("eta-prof: flat rows must be objects");
            }
            const auto frame_id_u64 = parse_u64(row_value["frame"], "flat.frame");
            if (!frame_id_u64) return std::unexpected(frame_id_u64.error());
            const auto frame_id = static_cast<FrameId>(*frame_id_u64);
            if (!valid_frame_id(frame_count, frame_id)) {
                return std::unexpected("eta-prof: flat.frame out of range");
            }

            const auto self_ns = parse_u64(row_value["self_ns"], "flat.self_ns");
            if (!self_ns) return std::unexpected(self_ns.error());
            const auto inclusive_ns = parse_u64(row_value["inclusive_ns"], "flat.inclusive_ns");
            if (!inclusive_ns) return std::unexpected(inclusive_ns.error());
            const auto calls = parse_u64(row_value["calls"], "flat.calls");
            if (!calls) return std::unexpected(calls.error());
            std::uint64_t bytes_allocated = 0;
            if (!row_value["bytes_allocated"].is_null()) {
                const auto parsed_bytes = parse_u64(
                    row_value["bytes_allocated"],
                    "flat.bytes_allocated");
                if (!parsed_bytes) return std::unexpected(parsed_bytes.error());
                bytes_allocated = *parsed_bytes;
            }

            session.flat_rows.push_back(ArchiveFlatRow{
                .frame_id = frame_id,
                .stats = FlatStats{
                    .self_ns = *self_ns,
                    .inclusive_ns = *inclusive_ns,
                    .calls = *calls,
                    .bytes_allocated = bytes_allocated,
                },
            });
        }
    }

    const auto& tree_value = root["tree"];
    if (!tree_value.is_null()) {
        if (!tree_value.is_array()) {
            return std::unexpected("eta-prof: field 'tree' must be an array");
        }
        for (const auto& row_value : tree_value.as_array()) {
            if (!row_value.is_object()) {
                return std::unexpected("eta-prof: tree rows must be objects");
            }
            const auto parent_u64 = parse_u64(row_value["parent"], "tree.parent");
            if (!parent_u64) return std::unexpected(parent_u64.error());
            const auto child_u64 = parse_u64(row_value["child"], "tree.child");
            if (!child_u64) return std::unexpected(child_u64.error());

            const auto parent_id = static_cast<FrameId>(*parent_u64);
            const auto child_id = static_cast<FrameId>(*child_u64);
            if (!valid_frame_id(frame_count, parent_id) || !valid_frame_id(frame_count, child_id)) {
                return std::unexpected("eta-prof: tree edge frame id out of range");
            }

            const auto inclusive_ns = parse_u64(row_value["inclusive_ns"], "tree.inclusive_ns");
            if (!inclusive_ns) return std::unexpected(inclusive_ns.error());
            const auto calls = parse_u64(row_value["calls"], "tree.calls");
            if (!calls) return std::unexpected(calls.error());

            session.tree_rows.push_back(ArchiveTreeRow{
                .parent_frame_id = parent_id,
                .child_frame_id = child_id,
                .stats = EdgeStats{
                    .inclusive_ns = *inclusive_ns,
                    .calls = *calls,
                },
            });
        }
    }

    const auto& profiles_value = root["profiles"];
    if (!profiles_value.is_null()) {
        if (!profiles_value.is_array()) {
            return std::unexpected("eta-prof: field 'profiles' must be an array");
        }
        for (const auto& profile_value : profiles_value.as_array()) {
            if (!profile_value.is_object()) {
                return std::unexpected("eta-prof: profile entries must be objects");
            }
            SpeedscopeThreadProfile profile;
            const auto& name_value = profile_value["name"];
            if (name_value.is_string()) profile.name = name_value.as_string();

            const auto& timestamps_value = profile_value["timestamps_ns"];
            if (!timestamps_value.is_array()) {
                return std::unexpected("eta-prof: profile.timestamps_ns must be an array");
            }
            for (const auto& ts_value : timestamps_value.as_array()) {
                const auto timestamp_ns = parse_u64(ts_value, "timestamps_ns[]");
                if (!timestamp_ns) return std::unexpected(timestamp_ns.error());
                profile.timestamps_ns.push_back(*timestamp_ns);
            }

            const auto& samples_value = profile_value["samples"];
            if (!samples_value.is_array()) {
                return std::unexpected("eta-prof: profile.samples must be an array");
            }
            for (const auto& sample_value : samples_value.as_array()) {
                if (!sample_value.is_array()) {
                    return std::unexpected("eta-prof: profile sample stacks must be arrays");
                }
                std::vector<FrameId> stack;
                stack.reserve(sample_value.as_array().size());
                for (const auto& id_value : sample_value.as_array()) {
                    const auto id_u64 = parse_u64(id_value, "profile.samples[]");
                    if (!id_u64) return std::unexpected(id_u64.error());
                    const auto frame_id = static_cast<FrameId>(*id_u64);
                    if (!valid_frame_id(frame_count, frame_id)) {
                        return std::unexpected("eta-prof: profile sample frame id out of range");
                    }
                    stack.push_back(frame_id);
                }
                profile.samples.push_back(std::move(stack));
            }
            session.sample_profiles.push_back(std::move(profile));
        }
    }

    const auto& counters_value = root["counters"];
    if (!counters_value.is_null()) {
        if (!counters_value.is_object()) {
            return std::unexpected("eta-prof: field 'counters' must be an object");
        }
        for (const auto& [name, value] : counters_value.as_object()) {
            const auto parsed_value = parse_u64(value, "counters." + name);
            if (!parsed_value) return std::unexpected(parsed_value.error());
            session.counters.insert_or_assign(name, *parsed_value);
        }
    }

    return session;
}

std::expected<ArchiveSession, std::string> merge_eta_prof_archives(
    const std::span<const ArchiveSession> sessions) {
    if (sessions.empty()) {
        return std::unexpected("eta-prof merge: expected at least one input session");
    }

    const auto mode = sessions.front().mode;
    for (std::size_t i = 0; i < sessions.size(); ++i) {
        if (sessions[i].mode != mode) {
            return std::unexpected("eta-prof merge: input modes do not match");
        }
    }

    FrameIdInterner interner;
    std::unordered_map<FrameId, FlatStats> flat_accumulator;
    std::unordered_map<EdgeKey, EdgeStats, EdgeKeyHash> tree_accumulator;

    ArchiveSession merged;
    merged.mode = mode;

    for (const auto& session : sessions) {
        std::vector<FrameId> remap;
        remap.reserve(session.frames.size());
        for (const auto& frame : session.frames) {
            auto id = interner.lookup(frame);
            if (!id.has_value()) {
                id = interner.intern(frame);
            }
            remap.push_back(*id);
        }

        for (const auto& [name, value] : session.counters) {
            merged.counters[name] += value;
        }

        for (const auto& row : session.flat_rows) {
            if (!valid_frame_id(remap.size(), row.frame_id)) continue;
            const auto mapped = remap[row.frame_id];
            auto& slot = flat_accumulator[mapped];
            slot.self_ns += row.stats.self_ns;
            slot.inclusive_ns += row.stats.inclusive_ns;
            slot.calls += row.stats.calls;
            slot.bytes_allocated += row.stats.bytes_allocated;
        }

        if (mode == ArchiveMode::Trace) {
            for (const auto& row : session.tree_rows) {
                if (!valid_frame_id(remap.size(), row.parent_frame_id)
                    || !valid_frame_id(remap.size(), row.child_frame_id)) {
                    continue;
                }
                const EdgeKey mapped_key{
                    .parent = remap[row.parent_frame_id],
                    .child = remap[row.child_frame_id],
                };
                auto& slot = tree_accumulator[mapped_key];
                slot.inclusive_ns += row.stats.inclusive_ns;
                slot.calls += row.stats.calls;
            }
        } else {
            for (const auto& profile : session.sample_profiles) {
                SpeedscopeThreadProfile merged_profile;
                merged_profile.name = profile.name;
                merged_profile.timestamps_ns = profile.timestamps_ns;
                merged_profile.samples = profile.samples;
                for (auto& stack : merged_profile.samples) {
                    for (auto& frame_id : stack) {
                        if (!valid_frame_id(remap.size(), frame_id)) {
                            frame_id = 0;
                            continue;
                        }
                        frame_id = remap[frame_id];
                    }
                }
                merged.sample_profiles.push_back(std::move(merged_profile));
            }
        }
    }

    merged.frames.reserve(interner.size());
    for (std::size_t i = 0; i < interner.size(); ++i) {
        const auto key = interner.key_for(static_cast<FrameId>(i));
        if (!key) {
            return std::unexpected("eta-prof merge: missing interned frame key");
        }
        merged.frames.push_back(*key);
    }

    merged.flat_rows.reserve(flat_accumulator.size());
    for (const auto& [frame_id, stats] : flat_accumulator) {
        merged.flat_rows.push_back(ArchiveFlatRow{
            .frame_id = frame_id,
            .stats = stats,
        });
    }

    if (mode == ArchiveMode::Trace) {
        merged.tree_rows.reserve(tree_accumulator.size());
        for (const auto& [edge, stats] : tree_accumulator) {
            merged.tree_rows.push_back(ArchiveTreeRow{
                .parent_frame_id = edge.parent,
                .child_frame_id = edge.child,
                .stats = stats,
            });
        }
    }

    sort_archive_rows(merged);
    return merged;
}

std::string render_pretty_archive_report(const ArchiveSession& session, const std::size_t top_n) {
    FrameIdInterner interner;
    build_interner_from_frames(session.frames, interner);

    Aggregator aggregator;
    for (const auto& row : session.flat_rows) {
        aggregator.record_flat(
            row.frame_id,
            row.stats.self_ns,
            row.stats.inclusive_ns,
            row.stats.calls,
            row.stats.bytes_allocated);
    }
    if (session.mode == ArchiveMode::Trace) {
        for (const auto& row : session.tree_rows) {
            aggregator.record_edge(
                row.parent_frame_id,
                row.child_frame_id,
                row.stats.inclusive_ns,
                row.stats.calls);
        }
    } else {
        aggregate_from_sample_profiles(session, aggregator);
    }
    return render_pretty_report(aggregator, interner, session.counters, top_n);
}

std::string render_json_archive_report(const ArchiveSession& session, const std::size_t top_n) {
    FrameIdInterner interner;
    build_interner_from_frames(session.frames, interner);

    Aggregator aggregator;
    for (const auto& row : session.flat_rows) {
        aggregator.record_flat(
            row.frame_id,
            row.stats.self_ns,
            row.stats.inclusive_ns,
            row.stats.calls,
            row.stats.bytes_allocated);
    }
    if (session.mode == ArchiveMode::Trace) {
        for (const auto& row : session.tree_rows) {
            aggregator.record_edge(
                row.parent_frame_id,
                row.child_frame_id,
                row.stats.inclusive_ns,
                row.stats.calls);
        }
    } else {
        aggregate_from_sample_profiles(session, aggregator);
    }
    return render_json_report(aggregator, interner, session.counters, top_n);
}

std::string render_speedscope_archive_report(const ArchiveSession& session) {
    FrameIdInterner interner;
    build_interner_from_frames(session.frames, interner);
    if (session.mode == ArchiveMode::Sample) {
        return write_speedscope_json(interner, session.sample_profiles);
    }
    return write_speedscope_json(interner, trace_summary_profiles(session));
}

std::string render_chrome_archive_report(const ArchiveSession& session) {
    FrameIdInterner interner;
    build_interner_from_frames(session.frames, interner);
    if (session.mode == ArchiveMode::Sample) {
        return write_chrome_trace_json(interner, session.sample_profiles);
    }
    return write_chrome_trace_json(interner, trace_summary_profiles(session));
}

} ///< namespace eta::runtime::prof
