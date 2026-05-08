#include "eta/runtime/prof/chrome.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>

#include "eta/util/json.h"

namespace eta::runtime::prof {

namespace {

[[nodiscard]] std::uint64_t sample_weight_ns(const SpeedscopeThreadProfile& profile,
                                             const std::size_t index) noexcept {
    if (index + 1u < profile.timestamps_ns.size()) {
        const auto begin = profile.timestamps_ns[index];
        const auto end = profile.timestamps_ns[index + 1u];
        if (end > begin) return end - begin;
    }
    if (index > 0u && index < profile.timestamps_ns.size()) {
        const auto prev = profile.timestamps_ns[index - 1u];
        const auto now = profile.timestamps_ns[index];
        if (now > prev) return now - prev;
    }
    return 1u;
}

[[nodiscard]] std::uint64_t to_us(const std::uint64_t ns) noexcept {
    return ns / 1000u;
}

} ///< namespace

std::string write_chrome_trace_json(const FrameIdInterner& interner,
                                    const std::vector<SpeedscopeThreadProfile>& profiles) {
    std::ostringstream out;
    out << "{";
    out << "\"traceEvents\":[";

    bool wrote_event = false;
    auto write_comma = [&]() {
        if (wrote_event) out << ",";
        wrote_event = true;
    };

    write_comma();
    out << "{"
        << "\"name\":\"process_name\","
        << "\"ph\":\"M\","
        << "\"pid\":1,"
        << "\"tid\":0,"
        << "\"args\":{\"name\":\"Eta Runtime\"}"
        << "}";

    for (std::size_t profile_idx = 0; profile_idx < profiles.size(); ++profile_idx) {
        const auto& profile = profiles[profile_idx];
        const auto tid = static_cast<std::uint64_t>(profile_idx + 1u);

        write_comma();
        out << "{"
            << "\"name\":\"thread_name\","
            << "\"ph\":\"M\","
            << "\"pid\":1,"
            << "\"tid\":" << tid << ","
            << "\"args\":{\"name\":";
        eta::json::escape_string(out, profile.name.empty()
            ? ("thread-" + std::to_string(tid))
            : profile.name);
        out << "}"
            << "}";

        const auto sample_count = (std::min)(profile.samples.size(), profile.timestamps_ns.size());
        for (std::size_t sample_idx = 0; sample_idx < sample_count; ++sample_idx) {
            const auto ts_us = to_us(profile.timestamps_ns[sample_idx]);
            const auto dur_us = (std::max)(std::uint64_t{1}, to_us(sample_weight_ns(profile, sample_idx)));
            const auto& stack = profile.samples[sample_idx];
            for (std::size_t depth = 0; depth < stack.size(); ++depth) {
                const auto key = interner.key_for(stack[depth]);
                const std::string frame_name = key ? key->qualified_name : "<unknown>";

                write_comma();
                out << "{";
                out << "\"name\":";
                eta::json::escape_string(out, frame_name);
                out << ",\"cat\":\"eta\"";
                out << ",\"ph\":\"X\"";
                out << ",\"pid\":1";
                out << ",\"tid\":" << tid;
                out << ",\"ts\":" << ts_us;
                out << ",\"dur\":" << dur_us;
                out << ",\"args\":{\"depth\":" << depth << "}";
                out << "}";
            }
        }
    }

    out << "]}";
    return out.str();
}

} ///< namespace eta::runtime::prof

