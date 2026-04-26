#include "eta/jupyter/display.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <torch/torch.h>

#include "eta/jupyter/comm/actors_comm.h"
#include "eta/jupyter/comm/dag_comm.h"
#include "eta/jupyter/comm/disasm_comm.h"
#include "eta/jupyter/comm/heap_comm.h"
#include "eta/jupyter/comm/tensor_comm.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/types/bytevector.h"
#include "eta/runtime/types/cons.h"
#include "eta/runtime/types/fact_table.h"
#include "eta/runtime/types/vector.h"
#include "eta/session/driver.h"
#include "eta/session/eval_display.h"
#include "eta/torch/tensor_ptr.h"

namespace eta::jupyter::display {

namespace {

namespace nl = nlohmann;
using eta::runtime::nanbox::LispVal;
using eta::runtime::nanbox::Tag;
using eta::runtime::nanbox::ops::decode;
using eta::runtime::nanbox::ops::is_boxed;
using eta::runtime::nanbox::ops::payload;
using eta::runtime::nanbox::ops::tag;

constexpr std::size_t kMaxJsonDepth = 24;
constexpr std::size_t kMaxListPreview = 2048;

struct WrapperPayload {
    std::string mime;
    LispVal payload{eta::runtime::nanbox::Nil};
};

[[nodiscard]] std::string html_escape(std::string_view text) {
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

[[nodiscard]] std::string base64_encode(const std::vector<std::uint8_t>& bytes) {
    static constexpr std::array<char, 64> alphabet = {
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
        'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
        'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
        'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
        'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
        'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
        'w', 'x', 'y', 'z', '0', '1', '2', '3',
        '4', '5', '6', '7', '8', '9', '+', '/',
    };

    std::string out;
    out.reserve(((bytes.size() + 2u) / 3u) * 4u);

    std::size_t i = 0;
    while (i + 2u < bytes.size()) {
        const std::uint32_t chunk =
            (static_cast<std::uint32_t>(bytes[i]) << 16u) |
            (static_cast<std::uint32_t>(bytes[i + 1u]) << 8u) |
            static_cast<std::uint32_t>(bytes[i + 2u]);
        out.push_back(alphabet[(chunk >> 18u) & 0x3Fu]);
        out.push_back(alphabet[(chunk >> 12u) & 0x3Fu]);
        out.push_back(alphabet[(chunk >> 6u) & 0x3Fu]);
        out.push_back(alphabet[chunk & 0x3Fu]);
        i += 3u;
    }

    if (i < bytes.size()) {
        const bool two = (i + 1u) < bytes.size();
        std::uint32_t chunk = static_cast<std::uint32_t>(bytes[i]) << 16u;
        if (two) chunk |= static_cast<std::uint32_t>(bytes[i + 1u]) << 8u;
        out.push_back(alphabet[(chunk >> 18u) & 0x3Fu]);
        out.push_back(alphabet[(chunk >> 12u) & 0x3Fu]);
        out.push_back(two ? alphabet[(chunk >> 6u) & 0x3Fu] : '=');
        out.push_back('=');
    }

    return out;
}

[[nodiscard]] bool decode_string(eta::session::Driver& driver, LispVal value, std::string* out) {
    if (!out || !is_boxed(value)) return false;
    const auto t = tag(value);
    if (t != Tag::String && t != Tag::Symbol) return false;

    auto sv = driver.intern_table().get_string(payload(value));
    if (!sv) return false;
    *out = std::string(*sv);
    return true;
}

[[nodiscard]] bool try_unpack_wrapper(eta::session::Driver& driver,
                                      LispVal value,
                                      WrapperPayload* out) {
    using eta::runtime::memory::heap::ObjectKind;
    using eta::runtime::types::Vector;

    if (!out || !is_boxed(value) || tag(value) != Tag::HeapObject) return false;
    auto* vec = driver.heap().try_get_as<ObjectKind::Vector, Vector>(payload(value));
    if (!vec || vec->elements.size() < 3u) return false;

    std::string marker;
    if (!decode_string(driver, vec->elements[0], &marker)) return false;
    if (marker != "jupyter-display") return false;

    std::string mime;
    if (!decode_string(driver, vec->elements[1], &mime)) return false;

    out->mime = std::move(mime);
    out->payload = vec->elements[2];
    return true;
}

[[nodiscard]] nl::json lisp_to_json(eta::session::Driver& driver, LispVal value, std::size_t depth);

[[nodiscard]] bool try_decode_pair(eta::session::Driver& driver,
                                   LispVal pair_value,
                                   std::string* key_out,
                                   LispVal* value_out) {
    using eta::runtime::memory::heap::ObjectKind;
    using eta::runtime::types::Cons;

    if (!key_out || !value_out) return false;
    if (!is_boxed(pair_value) || tag(pair_value) != Tag::HeapObject) return false;

    auto* pair = driver.heap().try_get_as<ObjectKind::Cons, Cons>(payload(pair_value));
    if (!pair) return false;

    std::string key;
    if (!decode_string(driver, pair->car, &key)) return false;

    LispVal value = eta::runtime::nanbox::Nil;
    if (pair->cdr == eta::runtime::nanbox::Nil) {
        return false;
    }

    if (is_boxed(pair->cdr) && tag(pair->cdr) == Tag::HeapObject) {
        if (auto* next = driver.heap().try_get_as<ObjectKind::Cons, Cons>(payload(pair->cdr))) {
            if (next->cdr != eta::runtime::nanbox::Nil) return false;
            value = next->car;
        } else {
            value = pair->cdr;
        }
    } else {
        value = pair->cdr;
    }

    *key_out = std::move(key);
    *value_out = value;
    return true;
}

[[nodiscard]] bool try_alist_to_object(eta::session::Driver& driver,
                                       LispVal value,
                                       std::size_t depth,
                                       nl::json* out) {
    using eta::runtime::memory::heap::ObjectKind;
    using eta::runtime::types::Cons;

    if (!out) return false;
    nl::json obj = nl::json::object();
    LispVal cur = value;
    std::size_t guard = 0;
    while (cur != eta::runtime::nanbox::Nil && guard < kMaxListPreview) {
        if (!is_boxed(cur) || tag(cur) != Tag::HeapObject) return false;
        auto* cons = driver.heap().try_get_as<ObjectKind::Cons, Cons>(payload(cur));
        if (!cons) return false;

        std::string key;
        LispVal mapped_value = eta::runtime::nanbox::Nil;
        if (!try_decode_pair(driver, cons->car, &key, &mapped_value)) return false;
        obj[key] = lisp_to_json(driver, mapped_value, depth + 1u);

        cur = cons->cdr;
        ++guard;
    }

    if (cur != eta::runtime::nanbox::Nil) return false;
    *out = std::move(obj);
    return true;
}

[[nodiscard]] nl::json list_to_json(eta::session::Driver& driver, LispVal value, std::size_t depth) {
    using eta::runtime::memory::heap::ObjectKind;
    using eta::runtime::types::Cons;

    nl::json maybe_object = nl::json::object();
    if (try_alist_to_object(driver, value, depth, &maybe_object)) {
        return maybe_object;
    }

    nl::json out = nl::json::array();
    LispVal cur = value;
    std::size_t guard = 0;
    while (cur != eta::runtime::nanbox::Nil && guard < kMaxListPreview) {
        if (!is_boxed(cur) || tag(cur) != Tag::HeapObject) {
            out.push_back(driver.format_value(cur, eta::runtime::FormatMode::Write));
            return out;
        }

        auto* cons = driver.heap().try_get_as<ObjectKind::Cons, Cons>(payload(cur));
        if (!cons) {
            out.push_back(driver.format_value(cur, eta::runtime::FormatMode::Write));
            return out;
        }

        out.push_back(lisp_to_json(driver, cons->car, depth + 1u));
        cur = cons->cdr;
        ++guard;
    }

    if (guard == kMaxListPreview) {
        out.push_back("<truncated>");
    }
    return out;
}

[[nodiscard]] nl::json lisp_to_json(eta::session::Driver& driver, LispVal value, std::size_t depth) {
    using eta::runtime::memory::heap::ObjectKind;
    using eta::runtime::types::ByteVector;
    using eta::runtime::types::Vector;

    if (depth > kMaxJsonDepth) {
        return driver.format_value(value, eta::runtime::FormatMode::Write);
    }

    if (value == eta::runtime::nanbox::Nil) return nullptr;
    if (value == eta::runtime::nanbox::True) return true;
    if (value == eta::runtime::nanbox::False) return false;

    if (!is_boxed(value)) {
        return std::bit_cast<double>(value);
    }

    const auto t = tag(value);
    if (t == Tag::Fixnum) {
        auto n = decode<std::int64_t>(value);
        return n ? nl::json(*n) : nl::json(nullptr);
    }
    if (t == Tag::Char) {
        auto cp = decode<char32_t>(value);
        if (!cp) return driver.format_value(value, eta::runtime::FormatMode::Write);
        if (*cp >= 0 && *cp <= 0x7F) {
            return std::string(1u, static_cast<char>(*cp));
        }
        std::ostringstream oss;
        oss << "U+" << std::uppercase << std::hex << static_cast<std::uint32_t>(*cp);
        return oss.str();
    }
    if (t == Tag::String || t == Tag::Symbol) {
        std::string s;
        if (decode_string(driver, value, &s)) return s;
        return driver.format_value(value, eta::runtime::FormatMode::Write);
    }
    if (t != Tag::HeapObject) {
        return driver.format_value(value, eta::runtime::FormatMode::Write);
    }

    const auto numeric = eta::runtime::classify_numeric(value, driver.heap());
    if (numeric.is_fixnum()) return numeric.int_val;
    if (numeric.is_flonum()) return numeric.float_val;

    const auto id = payload(value);
    if (driver.heap().try_get_as<ObjectKind::Cons, eta::runtime::types::Cons>(id)) {
        return list_to_json(driver, value, depth + 1u);
    }
    if (auto* vec = driver.heap().try_get_as<ObjectKind::Vector, Vector>(id)) {
        nl::json out = nl::json::array();
        for (const auto item : vec->elements) {
            out.push_back(lisp_to_json(driver, item, depth + 1u));
        }
        return out;
    }
    if (auto* bytes = driver.heap().try_get_as<ObjectKind::ByteVector, ByteVector>(id)) {
        nl::json out = nl::json::array();
        for (const auto b : bytes->data) out.push_back(b);
        return out;
    }

    return driver.format_value(value, eta::runtime::FormatMode::Write);
}

[[nodiscard]] bool try_get_fact_table(eta::session::Driver& driver,
                                      LispVal value,
                                      eta::runtime::types::FactTable** out) {
    using eta::runtime::memory::heap::ObjectKind;
    if (!out || !is_boxed(value) || tag(value) != Tag::HeapObject) return false;
    *out = driver.heap().try_get_as<ObjectKind::FactTable, eta::runtime::types::FactTable>(payload(value));
    return *out != nullptr;
}

[[nodiscard]] bool try_get_tensor(eta::session::Driver& driver,
                                  LispVal value,
                                  eta::torch_bindings::TensorPtr** out) {
    using eta::runtime::memory::heap::ObjectKind;
    if (!out || !is_boxed(value) || tag(value) != Tag::HeapObject) return false;
    *out = driver.heap().try_get_as<ObjectKind::Tensor, eta::torch_bindings::TensorPtr>(payload(value));
    return *out != nullptr;
}

[[nodiscard]] nl::json tensor_json(const ::torch::Tensor& tensor, std::size_t preview) {
    nl::json out = nl::json::object();
    nl::json shape = nl::json::array();
    for (const auto dim : tensor.sizes()) shape.push_back(dim);
    out["shape"] = std::move(shape);
    out["dtype"] = std::string(c10::toString(tensor.scalar_type()));
    out["device"] = tensor.device().str();
    out["numel"] = tensor.numel();

    try {
        auto flat = tensor.detach().cpu().flatten();
        if (flat.numel() > 0) {
            const auto flat64 = flat.to(::torch::kFloat64).contiguous();
            const auto count = static_cast<std::size_t>(flat64.numel());
            const auto* ptr = flat64.data_ptr<double>();

            nl::json head = nl::json::array();
            const auto head_n = (std::min)(preview, count);
            for (std::size_t i = 0; i < head_n; ++i) head.push_back(ptr[i]);
            out["preview_head"] = std::move(head);

            if (count > head_n) {
                nl::json tail = nl::json::array();
                const auto tail_n = (std::min)(preview, count - head_n);
                const std::size_t tail_start = count - tail_n;
                for (std::size_t i = tail_start; i < count; ++i) tail.push_back(ptr[i]);
                out["preview_tail"] = std::move(tail);
            }
        }
    } catch (...) {
        out["preview_error"] = "tensor preview unavailable";
    }

    return out;
}

[[nodiscard]] std::string tensor_html(const ::torch::Tensor& tensor, std::size_t preview) {
    std::ostringstream oss;
    const auto t = tensor.detach().cpu();
    const auto shape = t.sizes();

    oss << "<div class=\"eta-tensor\">";
    oss << "<div><strong>Tensor</strong> shape=[";
    for (std::size_t i = 0; i < static_cast<std::size_t>(shape.size()); ++i) {
        if (i) oss << ", ";
        oss << shape[i];
    }
    oss << "] dtype=" << html_escape(c10::toString(t.scalar_type())) << "</div>";

    if (t.dim() == 2) {
        try {
            const auto preview_limit = static_cast<decltype(t.size(0))>(preview);
            const auto rows = static_cast<std::size_t>((std::min)(t.size(0), preview_limit));
            const auto cols = static_cast<std::size_t>((std::min)(t.size(1), preview_limit));
            const auto matrix = t.to(::torch::kFloat64).contiguous();
            auto acc = matrix.accessor<double, 2>();
            oss << "<table border=\"1\" cellspacing=\"0\" cellpadding=\"3\">";
            for (std::size_t r = 0; r < rows; ++r) {
                oss << "<tr>";
                for (std::size_t c = 0; c < cols; ++c) {
                    oss << "<td>" << acc[static_cast<long long>(r)][static_cast<long long>(c)] << "</td>";
                }
                oss << "</tr>";
            }
            oss << "</table>";
            if (rows < static_cast<std::size_t>(t.size(0)) || cols < static_cast<std::size_t>(t.size(1))) {
                oss << "<div>Preview truncated to " << preview << "x" << preview << ".</div>";
            }
        } catch (...) {
            oss << "<pre>" << html_escape(tensor_json(t, preview).dump(2)) << "</pre>";
        }
    } else {
        oss << "<pre>" << html_escape(tensor_json(t, preview).dump(2)) << "</pre>";
    }

    oss << "</div>";
    return oss.str();
}

[[nodiscard]] nl::json fact_table_json(eta::session::Driver& driver,
                                       const eta::runtime::types::FactTable& ft,
                                       std::size_t row_limit) {
    nl::json out = nl::json::object();
    out["version"] = 1;
    out["columns"] = ft.col_names;
    out["row_count"] = ft.active_row_count();
    out["row_count_total"] = ft.row_count;

    nl::json rows = nl::json::array();
    std::size_t emitted = 0;
    for (std::size_t row = 0; row < ft.row_count; ++row) {
        if (row >= ft.live_mask.size() || ft.live_mask[row] == 0) continue;
        if (emitted >= row_limit) break;

        nl::json cells = nl::json::array();
        for (std::size_t col = 0; col < ft.col_names.size(); ++col) {
            cells.push_back(lisp_to_json(driver, ft.get_cell(row, col), 0u));
        }
        rows.push_back(std::move(cells));
        ++emitted;
    }

    out["rows"] = std::move(rows);
    out["truncated"] = ft.active_row_count() > emitted;
    return out;
}

[[nodiscard]] std::string fact_table_html(eta::session::Driver& driver,
                                          const eta::runtime::types::FactTable& ft,
                                          std::size_t row_limit) {
    std::ostringstream oss;
    oss << "<div class=\"eta-fact-table\">";
    oss << "<div><strong>FactTable</strong> rows=" << ft.active_row_count()
        << " cols=" << ft.col_names.size() << "</div>";
    oss << "<table border=\"1\" cellspacing=\"0\" cellpadding=\"3\"><thead><tr>";
    for (const auto& col : ft.col_names) {
        oss << "<th>" << html_escape(col) << "</th>";
    }
    oss << "</tr></thead><tbody>";

    std::size_t emitted = 0;
    for (std::size_t row = 0; row < ft.row_count; ++row) {
        if (row >= ft.live_mask.size() || ft.live_mask[row] == 0) continue;
        if (emitted >= row_limit) break;
        oss << "<tr>";
        for (std::size_t col = 0; col < ft.col_names.size(); ++col) {
            const auto cell = ft.get_cell(row, col);
            oss << "<td>" << html_escape(driver.format_value(cell, eta::runtime::FormatMode::Write)) << "</td>";
        }
        oss << "</tr>";
        ++emitted;
    }
    oss << "</tbody></table>";

    if (ft.active_row_count() > emitted) {
        oss << "<div>Preview truncated to " << row_limit << " rows.</div>";
    }
    oss << "</div>";
    return oss.str();
}

void ensure_plain_text(RenderResult& out, const eta::session::DisplayValue& value) {
    if (!value.text.empty() && !out.data.contains("text/plain")) {
        out.data["text/plain"] = value.text;
    }
}

void render_wrapper(RenderResult& out,
                    eta::session::Driver& driver,
                    const WrapperPayload& wrapper,
                    const RenderOptions& options) {
    if (wrapper.mime == "text/html" ||
        wrapper.mime == "text/markdown" ||
        wrapper.mime == "text/latex" ||
        wrapper.mime == "image/svg+xml") {
        std::string text;
        if (decode_string(driver, wrapper.payload, &text)) {
            out.data[wrapper.mime] = text;
        } else {
            out.data[wrapper.mime] = driver.format_value(wrapper.payload, eta::runtime::FormatMode::Write);
        }
        return;
    }

    if (wrapper.mime == "image/png") {
        using eta::runtime::memory::heap::ObjectKind;
        using eta::runtime::types::ByteVector;
        if (is_boxed(wrapper.payload) && tag(wrapper.payload) == Tag::HeapObject) {
            if (auto* bv = driver.heap().try_get_as<ObjectKind::ByteVector, ByteVector>(payload(wrapper.payload))) {
                out.data["image/png"] = base64_encode(bv->data);
                return;
            }
        }

        std::string maybe_b64;
        if (decode_string(driver, wrapper.payload, &maybe_b64)) {
            out.data["image/png"] = maybe_b64;
            return;
        }
        out.data["text/plain"] = "invalid image/png payload";
        return;
    }

    if (wrapper.mime == "application/vnd.vegalite.v5+json") {
        auto j = lisp_to_json(driver, wrapper.payload, 0u);
        if (j.is_string()) {
            try {
                j = nl::json::parse(j.get<std::string>());
            } catch (...) {
                j = nl::json::object({{"value", j}});
            }
        }
        out.data["application/vnd.vegalite.v5+json"] = std::move(j);
        out.metadata["eta"] = nl::json::object({{"plot_theme", options.plot_theme}});
        return;
    }

    if (wrapper.mime == "application/vnd.eta.tensor+json") {
        eta::torch_bindings::TensorPtr* tensor_ptr = nullptr;
        if (try_get_tensor(driver, wrapper.payload, &tensor_ptr)) {
            out.data["application/vnd.eta.tensor+json"] = tensor_json(tensor_ptr->tensor, options.tensor_preview);
            out.data["text/html"] = tensor_html(tensor_ptr->tensor, options.tensor_preview);
        } else {
            out.data["text/plain"] = "invalid tensor payload";
        }
        return;
    }

    if (wrapper.mime == "application/vnd.eta.facttable+json") {
        eta::runtime::types::FactTable* ft = nullptr;
        if (try_get_fact_table(driver, wrapper.payload, &ft)) {
            out.data["application/vnd.eta.facttable+json"] =
                fact_table_json(driver, *ft, options.table_max_rows);
            out.data["text/html"] = fact_table_html(driver, *ft, options.table_max_rows);
        } else {
            out.data["text/plain"] = "invalid fact-table payload";
        }
        return;
    }

    if (wrapper.mime == "application/vnd.eta.heap+json") {
        auto snapshot = eta::jupyter::comm::build_heap_snapshot(driver);
        out.data["application/vnd.eta.heap+json"] = snapshot;
        out.data["text/html"] = eta::jupyter::comm::heap_snapshot_html(snapshot);
        return;
    }

    if (wrapper.mime == "application/vnd.eta.disasm+json") {
        std::string scope = "current";
        std::string function;

        auto query = lisp_to_json(driver, wrapper.payload, 0u);
        if (query.is_string()) {
            function = query.get<std::string>();
        } else if (query.is_object()) {
            scope = query.value("scope", std::string("current"));
            function = query.value("function", std::string());
        }

        auto payload = eta::jupyter::comm::build_disassembly(driver, scope, function);
        out.data["application/vnd.eta.disasm+json"] = payload;
        out.data["text/html"] = eta::jupyter::comm::disassembly_html(payload);
        return;
    }

    if (wrapper.mime == "application/vnd.eta.actors+json") {
        auto payload = eta::jupyter::comm::build_actor_snapshot(driver);
        out.data["application/vnd.eta.actors+json"] = payload;
        out.data["text/html"] = eta::jupyter::comm::actors_snapshot_html(payload);
        return;
    }

    if (wrapper.mime == "application/vnd.eta.dag+json") {
        auto graph = lisp_to_json(driver, wrapper.payload, 0u);
        auto payload = eta::jupyter::comm::build_dag_payload(std::move(graph));
        out.data["application/vnd.eta.dag+json"] = payload;
        out.data["text/html"] = eta::jupyter::comm::dag_payload_html(payload);
        return;
    }

    if (wrapper.mime == "application/vnd.eta.tensor-explorer+json") {
        eta::torch_bindings::TensorPtr* tensor_ptr = nullptr;
        nlohmann::json tensor = nlohmann::json::object();
        if (try_get_tensor(driver, wrapper.payload, &tensor_ptr)) {
            tensor = tensor_json(tensor_ptr->tensor, options.tensor_preview);
        } else {
            tensor = lisp_to_json(driver, wrapper.payload, 0u);
        }
        auto payload = eta::jupyter::comm::build_tensor_payload(std::move(tensor));
        out.data["application/vnd.eta.tensor-explorer+json"] = payload;
        out.data["text/html"] = eta::jupyter::comm::tensor_payload_html(payload);
        return;
    }

    out.data[wrapper.mime] = lisp_to_json(driver, wrapper.payload, 0u);
}

} // namespace

RenderResult render_display_value(eta::session::Driver& driver,
                                  const eta::session::DisplayValue& value,
                                  const RenderOptions& options) {
    RenderResult out;
    if (value.tag == eta::session::DisplayTag::Error) {
        ensure_plain_text(out, value);
        return out;
    }

    WrapperPayload wrapper;
    if (try_unpack_wrapper(driver, value.value, &wrapper)) {
        render_wrapper(out, driver, wrapper, options);
        ensure_plain_text(out, value);
        return out;
    }

    if (value.tag == eta::session::DisplayTag::Tensor) {
        eta::torch_bindings::TensorPtr* tensor_ptr = nullptr;
        if (try_get_tensor(driver, value.value, &tensor_ptr)) {
            out.data["application/vnd.eta.tensor+json"] = tensor_json(tensor_ptr->tensor, options.tensor_preview);
            out.data["text/html"] = tensor_html(tensor_ptr->tensor, options.tensor_preview);
        }
    } else if (value.tag == eta::session::DisplayTag::FactTable) {
        eta::runtime::types::FactTable* ft = nullptr;
        if (try_get_fact_table(driver, value.value, &ft)) {
            out.data["application/vnd.eta.facttable+json"] =
                fact_table_json(driver, *ft, options.table_max_rows);
            out.data["text/html"] = fact_table_html(driver, *ft, options.table_max_rows);
        }
    }

    ensure_plain_text(out, value);
    return out;
}

} // namespace eta::jupyter::display
