#pragma once

/**
 * @file ui_primitives.h
 * @brief `%ui-*` runtime primitives backing `std.ui`.
 *
 * Registration order must match the `%ui-*` block in
 * eta/core/src/eta/runtime/builtin_specs_seed.h.
 *
 *   (%ui-start app-id)         -> url string; starts the server once per process
 *   (%ui-send value retain?)   -> number of clients the message was queued for
 *   (%ui-recv timeout-ms)      -> next client message (JSON string) or #f
 *   (%ui-open url)             -> #t if the browser launcher ran
 *   (%ui-stop)                 -> '()
 *   (%ui-client-count)         -> number of connected clients
 *   (%ui-window url title w h) -> #t if a native window opened (else #f)
 *
 * `%ui-send` serialises an Eta value to the protocol frame itself, so large
 * trees never become interned strings. Vectors of numbers with at least
 * kMinBufferLength elements are sent as binary Float64 buffers and appear in
 * the JSON as {"$buf": index}; the frame header lists each buffer's offset
 * and length.
 */

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "eta/runtime/builtin_env.h"
#include "eta/runtime/error.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/string_view.h"
#include "eta/runtime/types/hash_map.h"
#include "eta/runtime/types/types.h"
#include "eta/ui/ui_server.h"

namespace eta::ui {

using eta::runtime::BuiltinEnvironment;
using eta::runtime::error::RuntimeError;
using eta::runtime::error::RuntimeErrorCode;
using eta::runtime::error::VMError;
using eta::runtime::memory::heap::Heap;
using eta::runtime::memory::heap::ObjectKind;
using eta::runtime::memory::intern::InternTable;
using eta::runtime::nanbox::False;
using eta::runtime::nanbox::LispVal;
using eta::runtime::nanbox::Nil;
using eta::runtime::nanbox::Tag;
using eta::runtime::nanbox::True;
namespace ops = eta::runtime::nanbox::ops;
namespace types = eta::runtime::types;
using UiArgs = std::span<const LispVal>;

inline constexpr std::size_t kMinBufferLength = 16;
inline constexpr int kMaxDepth = 256;

namespace detail {

inline std::unexpected<RuntimeError> ui_type_error(const std::string& msg) {
    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, msg}});
}

inline std::unexpected<RuntimeError> ui_internal_error(const std::string& msg) {
    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InternalError, msg}});
}

inline void append_double(std::string& out, double d) {
    if (!std::isfinite(d)) {
        out += "null";
        return;
    }
    char buf[32];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof buf, d);
    if (ec != std::errc{}) {
        out += "null";
        return;
    }
    out.append(buf, ptr);
}

inline void append_int(std::string& out, std::int64_t v) {
    char buf[24];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof buf, v);
    (void)ec;
    out.append(buf, ptr);
}

/// Serialises Eta values to JSON, moving large numeric vectors into a
/// binary side buffer.
class FrameWriter {
public:
    FrameWriter(Heap& heap, InternTable& intern) : heap_(heap), intern_(intern) {}

    std::expected<void, RuntimeError> write(LispVal v, int depth = 0) {
        if (depth > kMaxDepth) return ui_type_error("%ui-send: value nested too deeply");

        if (v == Nil) { json_ += "[]"; return {}; }
        if (v == True) { json_ += "true"; return {}; }
        if (v == False) { json_ += "false"; return {}; }

        const auto num = eta::runtime::classify_numeric(v, heap_);
        if (num.is_valid()) {
            if (num.is_flonum()) append_double(json_, num.float_val);
            else append_int(json_, num.int_val);
            return {};
        }

        if (auto sv = eta::runtime::StringView::try_from(v, intern_)) {
            json_ += json_quote(sv->view());
            return {};
        }

        if (ops::is_boxed(v) && ops::tag(v) == Tag::Symbol) {
            auto text = intern_.get_string(ops::payload(v));
            if (!text) return ui_internal_error("%ui-send: unresolved symbol");
            json_ += json_quote(*text);
            return {};
        }

        if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) {
            return ui_type_error("%ui-send: unsupported value (characters and other immediates cannot be sent)");
        }
        const auto id = ops::payload(v);

        if (auto* vec = heap_.try_get_as<ObjectKind::Vector, types::Vector>(id)) {
            return write_vector(vec->elements, depth);
        }

        if (heap_.try_get_as<ObjectKind::Cons, types::Cons>(id) != nullptr) {
            // Proper list -> JSON array.
            std::vector<LispVal> items;
            LispVal cur = v;
            while (cur != Nil) {
                if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject) {
                    return ui_type_error("%ui-send: improper list");
                }
                auto* cell = heap_.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
                if (cell == nullptr) return ui_type_error("%ui-send: improper list");
                items.push_back(cell->car);
                cur = cell->cdr;
            }
            return write_vector(items, depth);
        }

        if (auto* map = heap_.try_get_as<ObjectKind::HashMap, types::HashMap>(id)) {
            json_.push_back('{');
            bool first = true;
            for (std::size_t i = 0; i < map->state.size(); ++i) {
                if (map->state[i] != static_cast<std::uint8_t>(types::HashSlotState::Occupied)) continue;
                if (!first) json_.push_back(',');
                first = false;
                auto key = key_string(map->keys[i]);
                if (!key) return std::unexpected(key.error());
                json_ += json_quote(*key);
                json_.push_back(':');
                auto r = write(map->values[i], depth + 1);
                if (!r) return r;
            }
            json_.push_back('}');
            return {};
        }

        return ui_type_error("%ui-send: unsupported value type (procedures must be replaced by handler ids before sending)");
    }

    [[nodiscard]] Frame finish() {
        std::string header = "{\"buffers\":[";
        for (std::size_t i = 0; i < buffer_index_.size(); ++i) {
            if (i) header.push_back(',');
            header += "{\"offset\":";
            append_int(header, static_cast<std::int64_t>(buffer_index_[i].first));
            header += ",\"length\":";
            append_int(header, static_cast<std::int64_t>(buffer_index_[i].second));
            header += ",\"dtype\":\"f64\"}";
        }
        header += "],\"body\":";
        header += json_;
        header.push_back('}');
        return make_frame(std::move(header), buffers_);
    }

private:
    std::expected<void, RuntimeError> write_vector(const std::vector<LispVal>& elems, int depth) {
        if (elems.size() >= kMinBufferLength) {
            std::vector<double> numbers;
            numbers.reserve(elems.size());
            bool all_numeric = true;
            for (auto e : elems) {
                const auto n = eta::runtime::classify_numeric(e, heap_);
                if (!n.is_valid()) { all_numeric = false; break; }
                numbers.push_back(n.is_flonum() ? n.float_val : static_cast<double>(n.int_val));
            }
            if (all_numeric) {
                const std::size_t index = buffer_index_.size();
                // Offsets are relative to the start of the buffer section,
                // which itself starts 8-byte aligned.
                while (buffers_.size() % 8 != 0) buffers_.push_back('\0');
                buffer_index_.emplace_back(buffers_.size(), numbers.size());
                buffers_.append(reinterpret_cast<const char*>(numbers.data()),
                                numbers.size() * sizeof(double));
                json_ += "{\"$buf\":";
                append_int(json_, static_cast<std::int64_t>(index));
                json_.push_back('}');
                return {};
            }
        }
        json_.push_back('[');
        for (std::size_t i = 0; i < elems.size(); ++i) {
            if (i) json_.push_back(',');
            auto r = write(elems[i], depth + 1);
            if (!r) return r;
        }
        json_.push_back(']');
        return {};
    }

    std::expected<std::string, RuntimeError> key_string(LispVal k) {
        if (auto sv = eta::runtime::StringView::try_from(k, intern_)) return std::string(sv->view());
        if (ops::is_boxed(k) && ops::tag(k) == Tag::Symbol) {
            auto text = intern_.get_string(ops::payload(k));
            if (!text) return ui_internal_error("%ui-send: unresolved symbol key");
            return std::string(*text);
        }
        const auto num = eta::runtime::classify_numeric(k, heap_);
        if (num.is_valid()) {
            std::string out;
            if (num.is_flonum()) append_double(out, num.float_val);
            else append_int(out, num.int_val);
            return out;
        }
        return ui_type_error("%ui-send: hash-map keys must be strings, symbols or numbers");
    }

    Heap& heap_;
    InternTable& intern_;
    std::string json_;
    std::string buffers_;
    std::vector<std::pair<std::size_t, std::size_t>> buffer_index_;
};

/// Locate the front-end bundle. Order: $ETA_UI_DEV_DIR, $ETA_UI_WEB_DIR,
/// then `packages/stdlib/native/ui/web` relative to the module search path
/// and the etai executable.
inline fs::path find_web_root(const std::string& module_search_path,
                              const std::string& etai_path) {
    std::error_code ec;
    const auto has_index = [&ec](const fs::path& p) {
        return !p.empty() && fs::is_regular_file(p / "index.html", ec);
    };
    for (const char* var : {"ETA_UI_DEV_DIR", "ETA_UI_WEB_DIR"}) {
        if (const char* v = std::getenv(var); v && *v && has_index(v)) return fs::path(v);
    }

    std::vector<fs::path> roots;
#ifdef _WIN32
    constexpr char kSep = ';';
#else
    constexpr char kSep = ':';
#endif
    std::size_t start = 0;
    while (start <= module_search_path.size()) {
        const auto end = module_search_path.find(kSep, start);
        const auto part = module_search_path.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!part.empty()) {
            fs::path p(part);
            roots.push_back(p);
            roots.push_back(p.parent_path());
            roots.push_back(p.parent_path().parent_path());
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (!etai_path.empty()) {
        const fs::path exe(etai_path);
        roots.push_back(exe.parent_path().parent_path());  // <prefix>/bin/etai
        roots.push_back(exe.parent_path());
    }
    for (const auto& root : roots) {
        const auto candidate = root / "packages" / "stdlib" / "native" / "ui" / "web";
        if (has_index(candidate)) return candidate;
    }
    return {};
}

} // namespace detail

/**
 * @brief Register the `%ui-*` primitives.
 *
 * @param module_search_path used to locate the front-end bundle
 * @param etai_path          path of the running etai (may be empty)
 */
inline void register_ui_primitives(BuiltinEnvironment& env,
                                   Heap& heap,
                                   InternTable& intern,
                                   std::string module_search_path = {},
                                   std::string etai_path = {}) {
    using detail::ui_type_error;

    env.register_builtin("%ui-start", 1, false,
        [&heap, &intern, module_search_path, etai_path](UiArgs args)
            -> std::expected<LispVal, RuntimeError> {
            auto sv = eta::runtime::StringView::try_from(args[0], intern);
            if (!sv) return ui_type_error("%ui-start: app id must be a string");
            const auto web_root = detail::find_web_root(module_search_path, etai_path);
            if (web_root.empty()) {
                return ui_type_error(
                    "%ui-start: front-end bundle not found. Build it (see "
                    "eta/tools/ui/frontend/README.md) or set ETA_UI_DEV_DIR to its output directory");
            }
            auto url = UiServer::instance().start(sv->view(), web_root);
            if (!url) return detail::ui_internal_error(url.error());
            return eta::runtime::memory::factory::make_string(heap, intern, *url);
        });

    env.register_builtin("%ui-send", 2, false,
        [&heap, &intern](UiArgs args) -> std::expected<LispVal, RuntimeError> {
            const bool retain = args[1] != False;
            detail::FrameWriter writer(heap, intern);
            auto ok = writer.write(args[0]);
            if (!ok) return std::unexpected(ok.error());
            const auto n = UiServer::instance().broadcast(writer.finish(), retain);
            return eta::runtime::memory::factory::make_fixnum(heap, static_cast<std::int64_t>(n));
        });

    env.register_builtin("%ui-recv", 1, false,
        [&heap, &intern](UiArgs args) -> std::expected<LispVal, RuntimeError> {
            const auto t = eta::runtime::classify_numeric(args[0], heap);
            if (!t.is_valid()) return ui_type_error("%ui-recv: timeout must be a number of milliseconds");
            const auto ms = t.is_flonum() ? static_cast<std::int64_t>(t.float_val) : t.int_val;
            auto msg = UiServer::instance().recv(ms);
            if (!msg) return False;
            return eta::runtime::memory::factory::make_string(heap, intern, *msg);
        });

    env.register_builtin("%ui-open", 1, false,
        [&intern](UiArgs args) -> std::expected<LispVal, RuntimeError> {
            auto sv = eta::runtime::StringView::try_from(args[0], intern);
            if (!sv) return ui_type_error("%ui-open: url must be a string");
            const std::string url(sv->view());
            if (!url.starts_with("http://127.0.0.1:")) {
                return ui_type_error("%ui-open: only local UI URLs can be opened");
            }
            return open_in_browser(url) ? True : False;
        });

    env.register_builtin("%ui-stop", 0, false,
        [](UiArgs) -> std::expected<LispVal, RuntimeError> {
            if (auto close = window_hooks().close) close();
            UiServer::instance().stop();
            return Nil;
        });

    env.register_builtin("%ui-client-count", 0, false,
        [&heap](UiArgs) -> std::expected<LispVal, RuntimeError> {
            return eta::runtime::memory::factory::make_fixnum(
                heap, static_cast<std::int64_t>(UiServer::instance().client_count()));
        });

    env.register_builtin("%ui-window", 4, false,
        [&heap, &intern](UiArgs args) -> std::expected<LispVal, RuntimeError> {
            auto url = eta::runtime::StringView::try_from(args[0], intern);
            auto title = eta::runtime::StringView::try_from(args[1], intern);
            if (!url || !title) return ui_type_error("%ui-window: url and title must be strings");
            const auto w = eta::runtime::classify_numeric(args[2], heap);
            const auto h = eta::runtime::classify_numeric(args[3], heap);
            if (!w.is_valid() || !h.is_valid()) return ui_type_error("%ui-window: width and height must be numbers");
            const std::string u(url->view());
            if (!u.starts_with("http://127.0.0.1:")) {
                return ui_type_error("%ui-window: only local UI URLs can be opened");
            }
            auto open = window_hooks().open;
            if (open == nullptr) return False;
            const int width = static_cast<int>(w.is_flonum() ? w.float_val : static_cast<double>(w.int_val));
            const int height = static_cast<int>(h.is_flonum() ? h.float_val : static_cast<double>(h.int_val));
            return open(u, std::string(title->view()), width, height) ? True : False;
        });
}

} // namespace eta::ui
