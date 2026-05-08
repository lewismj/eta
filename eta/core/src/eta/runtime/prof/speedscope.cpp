#include "eta/runtime/prof/speedscope.h"

#include <sstream>

#include "eta/util/json.h"

namespace eta::runtime::prof {

std::string write_speedscope_json(
    const FrameIdInterner& interner,
    const std::vector<SpeedscopeThreadProfile>& profiles) {
    std::ostringstream out;
    out << "{";
    out << "\"$schema\":\"https://www.speedscope.app/file-format-schema.json\",";
    out << "\"shared\":{\"frames\":[";
    for (std::size_t i = 0; i < interner.size(); ++i) {
        if (i != 0) out << ",";
        const auto key = interner.key_for(static_cast<FrameId>(i));
        const std::string name = key ? key->qualified_name : "<unknown>";
        out << "{\"name\":";
        eta::json::escape_string(out, name);
        out << "}";
    }
    out << "]},";
    out << "\"profiles\":[";
    for (std::size_t i = 0; i < profiles.size(); ++i) {
        const auto& p = profiles[i];
        if (i != 0) out << ",";
        out << "{";
        out << "\"type\":\"sampled\",";
        out << "\"name\":";
        eta::json::escape_string(out, p.name);
        out << ",";
        out << "\"unit\":\"nanoseconds\",";
        out << "\"startValue\":0,";
        out << "\"endValue\":" << (p.timestamps_ns.empty() ? 0 : p.timestamps_ns.back()) << ",";
        out << "\"samples\":[";
        for (std::size_t s = 0; s < p.samples.size(); ++s) {
            if (s != 0) out << ",";
            out << "[";
            for (std::size_t j = 0; j < p.samples[s].size(); ++j) {
                if (j != 0) out << ",";
                out << p.samples[s][j];
            }
            out << "]";
        }
        out << "],";
        out << "\"weights\":[";
        for (std::size_t w = 0; w < p.timestamps_ns.size(); ++w) {
            if (w != 0) out << ",";
            out << 1;
        }
        out << "]";
        out << "}";
    }
    out << "]";
    out << "}";
    return out.str();
}

} ///< namespace eta::runtime::prof
