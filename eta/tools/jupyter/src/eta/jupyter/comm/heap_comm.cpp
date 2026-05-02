#include "eta/jupyter/comm/heap_comm.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/cons_pool.h"
#include "eta/runtime/nanbox.h"
#include "eta/session/driver.h"

namespace eta::jupyter::comm {

namespace {

namespace nl = nlohmann;

[[nodiscard]] std::string escape_html(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        switch (ch) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

} // namespace

std::string_view heap_target()
{
    return "eta.heap";
}

nl::json build_heap_snapshot(eta::session::Driver& driver)
{
    using eta::runtime::memory::heap::ObjectKind;
    using eta::runtime::memory::heap::ObjectId;

    struct KindStat {
        std::int64_t count{0};
        std::int64_t bytes{0};
    };

    std::unordered_map<std::uint8_t, KindStat> kind_stats;
    std::int64_t scanned_objects = 0;
    driver.heap().for_each_entry([&](ObjectId /*id*/, eta::runtime::memory::heap::HeapEntry& entry) {
        auto& stat = kind_stats[static_cast<std::uint8_t>(entry.header.kind)];
        ++stat.count;
        stat.bytes += static_cast<std::int64_t>(entry.size);
        ++scanned_objects;
    });

    std::vector<std::pair<std::uint8_t, KindStat>> kind_rows;
    kind_rows.reserve(kind_stats.size());
    for (const auto& [kind, stat] : kind_stats) {
        kind_rows.emplace_back(kind, stat);
    }
    std::sort(kind_rows.begin(), kind_rows.end(),
              [](const auto& lhs, const auto& rhs) {
                  if (lhs.second.bytes != rhs.second.bytes) {
                      return lhs.second.bytes > rhs.second.bytes;
                  }
                  if (lhs.second.count != rhs.second.count) {
                      return lhs.second.count > rhs.second.count;
                  }
                  return lhs.first < rhs.first;
              });

    nl::json kinds = nl::json::array();
    for (const auto& [kind_u8, stat] : kind_rows) {
        kinds.push_back(nl::json::object({
            {"kind", std::string(eta::runtime::memory::heap::to_string(
                         static_cast<ObjectKind>(kind_u8)))},
            {"count", stat.count},
            {"bytes", stat.bytes},
        }));
    }

    nl::json roots = nl::json::array();
    const auto root_infos = driver.vm().enumerate_gc_roots();
    const auto& globals = driver.vm().globals();
    const auto& global_names = driver.global_names();

    std::unordered_map<ObjectId, std::uint32_t> slot_by_object;
    slot_by_object.reserve(globals.size());
    for (std::size_t slot = 0; slot < globals.size(); ++slot) {
        const auto value = globals[slot];
        if (!eta::runtime::nanbox::ops::is_boxed(value) ||
            eta::runtime::nanbox::ops::tag(value) != eta::runtime::nanbox::Tag::HeapObject) {
            continue;
        }
        auto id = static_cast<ObjectId>(eta::runtime::nanbox::ops::payload(value));
        slot_by_object.emplace(id, static_cast<std::uint32_t>(slot));
    }

    for (const auto& root : root_infos) {
        nl::json ids = nl::json::array();
        nl::json labels = nl::json::array();

        for (const auto object_id : root.object_ids) {
            ids.push_back(static_cast<std::int64_t>(object_id));
            if (root.name == "Globals") {
                std::string label = "Object #" + std::to_string(object_id);
                if (auto it = slot_by_object.find(object_id); it != slot_by_object.end()) {
                    if (auto named = global_names.find(it->second); named != global_names.end()) {
                        label = named->second;
                    } else {
                        label = "global[" + std::to_string(it->second) + "]";
                    }
                }
                labels.push_back(std::move(label));
            }
        }

        nl::json root_json = nl::json::object({
            {"name", root.name},
            {"objectIds", std::move(ids)},
        });
        if (!labels.empty()) {
            root_json["labels"] = std::move(labels);
        }
        roots.push_back(std::move(root_json));
    }

    const auto pool = driver.heap().cons_pool().stats();
    nl::json cons_pool = nl::json::object({
        {"capacity", static_cast<std::int64_t>(pool.capacity)},
        {"live", static_cast<std::int64_t>(pool.live_count)},
        {"free", static_cast<std::int64_t>(pool.free_count)},
        {"bytes", static_cast<std::int64_t>(pool.bytes)},
    });

    return nl::json::object({
        {"version", 1},
        {"totalBytes", static_cast<std::int64_t>(driver.heap().total_bytes())},
        {"softLimit", static_cast<std::int64_t>(driver.heap().soft_limit())},
        {"kinds", std::move(kinds)},
        {"roots", std::move(roots)},
        {"consPool", std::move(cons_pool)},
        {"scannedObjects", scanned_objects},
    });
}

std::string heap_snapshot_html(const nlohmann::json& snapshot)
{
    std::string html;
    html += "<div class=\"eta-heap-snapshot\">";
    html += "<div><strong>Heap Snapshot</strong></div>";
    html += "<div>Total bytes: "
         + escape_html(std::to_string(snapshot.value("totalBytes", static_cast<std::int64_t>(0))))
         + "</div>";
    html += "<div>Soft limit: "
         + escape_html(std::to_string(snapshot.value("softLimit", static_cast<std::int64_t>(0))))
         + "</div>";

    html += "<table border=\"1\" cellspacing=\"0\" cellpadding=\"3\">";
    html += "<thead><tr><th>Kind</th><th>Count</th><th>Bytes</th></tr></thead><tbody>";
    if (snapshot.contains("kinds") && snapshot["kinds"].is_array()) {
        for (const auto& row : snapshot["kinds"]) {
            html += "<tr>";
            html += "<td>" + escape_html(row.value("kind", "")) + "</td>";
            html += "<td>"
                 + escape_html(std::to_string(row.value("count", static_cast<std::int64_t>(0))))
                 + "</td>";
            html += "<td>"
                 + escape_html(std::to_string(row.value("bytes", static_cast<std::int64_t>(0))))
                 + "</td>";
            html += "</tr>";
        }
    }
    html += "</tbody></table>";
    html += "</div>";
    return html;
}

} // namespace eta::jupyter::comm
