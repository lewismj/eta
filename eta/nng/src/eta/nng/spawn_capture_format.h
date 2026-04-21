#pragma once

/**
 * @file spawn_capture_format.h
 * @brief Serialization format for spawn-thread closure/module captures.
 */

#include <bit>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "eta/runtime/error.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/types/types.h"
#include "eta/runtime/vm/bytecode.h"

namespace eta::nng {

using namespace eta::runtime;
using namespace eta::runtime::nanbox;
using namespace eta::runtime::error;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::memory::factory;

/**
 * Magic byte that marks spawn-thread capture payloads.
 * Distinct from message wire formats (0xEA/0xEB).
 */
constexpr uint8_t SPAWN_CAPTURE_VERSION_BYTE = 0xEC;

/**
 * Value tags used in spawn-thread capture payloads.
 * Base scalar/container tags align with binary wire-format tags for consistency.
 */
enum SpawnCaptureTag : uint8_t {
    SCT_Nil        =  0,
    SCT_True       =  1,
    SCT_Fixnum     =  2,
    SCT_Double     =  3,
    SCT_Char       =  4,
    SCT_String     =  5,
    SCT_Symbol     =  6,
    SCT_HeapCons   =  8,
    SCT_HeapVec    =  9,
    SCT_False      = 12,
    SCT_ByteVec    = 13,
    SCT_ClosureRef = 14,
    SCT_GlobalRef  = 15,
};

struct SpawnCapturedGlobal {
    uint32_t slot{0};
    LispVal value{nanbox::Nil};
};

struct SpawnCapturePayload {
    std::vector<LispVal> upvals;
    std::vector<SpawnCapturedGlobal> globals;
};

using SpawnClosureFuncIndexFn = std::function<std::expected<uint32_t, std::string>(ObjectId)>;
using SpawnGlobalRefSlotFn = std::function<std::optional<uint32_t>(LispVal)>;
using SpawnResolveFuncFn = std::function<const runtime::vm::BytecodeFunction*(uint32_t)>;
using SpawnResolveGlobalFn = std::function<std::optional<LispVal>(uint32_t)>;

namespace detail {

struct SpawnCaptureWriter {
    struct ClosureNode {
        uint32_t func_idx{0};
        std::vector<LispVal> upvals;
        std::vector<std::vector<uint8_t>> encoded_upvals;
    };

    const Heap& heap;
    const InternTable& intern;
    SpawnClosureFuncIndexFn closure_func_index;
    SpawnGlobalRefSlotFn global_ref_slot;

    std::unordered_map<ObjectId, uint32_t> closure_ids;
    std::vector<ClosureNode> closures;
    std::unordered_set<ObjectId> active_containers;
    std::string error_msg;

    static void write_u8(std::vector<uint8_t>& out, uint8_t v) {
        out.push_back(v);
    }

    static void write_u32(std::vector<uint8_t>& out, uint32_t v) {
        out.push_back(static_cast<uint8_t>(v));
        out.push_back(static_cast<uint8_t>(v >> 8));
        out.push_back(static_cast<uint8_t>(v >> 16));
        out.push_back(static_cast<uint8_t>(v >> 24));
    }

    static void write_i64(std::vector<uint8_t>& out, int64_t v) {
        uint64_t u = static_cast<uint64_t>(v);
        for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(u >> (8 * i)));
    }

    static void write_f64(std::vector<uint8_t>& out, double v) {
        uint64_t u = 0;
        std::memcpy(&u, &v, sizeof(double));
        for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(u >> (8 * i)));
    }

    static void write_str(std::vector<uint8_t>& out, std::string_view sv) {
        write_u32(out, static_cast<uint32_t>(sv.size()));
        out.insert(out.end(), sv.begin(), sv.end());
    }

    bool fail(std::string_view root_label, std::string_view msg) {
        if (error_msg.empty()) {
            if (!root_label.empty()) {
                error_msg = std::string(root_label) + ": " + std::string(msg);
            } else {
                error_msg = std::string(msg);
            }
        }
        return false;
    }

    bool write_heap_value(LispVal v, std::vector<uint8_t>& out,
                          std::string_view root_label) {
        auto id = ops::payload(v);

        if (auto* big = heap.try_get_as<ObjectKind::Fixnum, int64_t>(id)) {
            write_u8(out, SCT_Fixnum);
            write_i64(out, *big);
            return true;
        }

        if (auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(id)) {
            if (!active_containers.insert(id).second) {
                return fail(root_label, "cyclic pair/list structure is not serializable");
            }
            write_u8(out, SCT_HeapCons);
            const bool ok_car = write_value(cons->car, out, root_label);
            const bool ok_cdr = ok_car ? write_value(cons->cdr, out, root_label) : false;
            active_containers.erase(id);
            return ok_car && ok_cdr;
        }

        if (auto* vec = heap.try_get_as<ObjectKind::Vector, types::Vector>(id)) {
            if (!active_containers.insert(id).second) {
                return fail(root_label, "cyclic vector structure is not serializable");
            }
            write_u8(out, SCT_HeapVec);
            write_u32(out, static_cast<uint32_t>(vec->elements.size()));
            for (auto elem : vec->elements) {
                if (!write_value(elem, out, root_label)) {
                    active_containers.erase(id);
                    return false;
                }
            }
            active_containers.erase(id);
            return true;
        }

        if (auto* bv = heap.try_get_as<ObjectKind::ByteVector, types::ByteVector>(id)) {
            write_u8(out, SCT_ByteVec);
            write_u32(out, static_cast<uint32_t>(bv->data.size()));
            out.insert(out.end(), bv->data.begin(), bv->data.end());
            return true;
        }

        if (auto* cl = heap.try_get_as<ObjectKind::Closure, types::Closure>(id)) {
            uint32_t closure_id = 0;
            if (auto it = closure_ids.find(id); it != closure_ids.end()) {
                closure_id = it->second;
            } else {
                if (!closure_func_index) {
                    return fail(root_label, "closure serializer is not configured");
                }
                auto idx = closure_func_index(id);
                if (!idx) {
                    return fail(root_label, idx.error());
                }
                closure_id = static_cast<uint32_t>(closures.size());
                closure_ids.emplace(id, closure_id);
                closures.push_back(ClosureNode{*idx, cl->upvals, {}});
            }
            write_u8(out, SCT_ClosureRef);
            write_u32(out, closure_id);
            return true;
        }

        HeapEntry entry{};
        const auto kind = heap.try_get(id, entry) ? entry.header.kind : ObjectKind::Unknown;
        return fail(root_label,
            "unsupported heap object kind " + std::string(to_string(kind)));
    }

    bool write_value(LispVal v, std::vector<uint8_t>& out,
                     std::string_view root_label) {
        if (global_ref_slot) {
            if (auto slot = global_ref_slot(v)) {
                write_u8(out, SCT_GlobalRef);
                write_u32(out, *slot);
                return true;
            }
        }

        if (v == nanbox::Nil)   { write_u8(out, SCT_Nil);   return true; }
        if (v == nanbox::True)  { write_u8(out, SCT_True);  return true; }
        if (v == nanbox::False) { write_u8(out, SCT_False); return true; }

        if (!ops::is_boxed(v)) {
            write_u8(out, SCT_Double);
            write_f64(out, std::bit_cast<double>(v));
            return true;
        }

        switch (ops::tag(v)) {
            case Tag::Fixnum: {
                write_u8(out, SCT_Fixnum);
                write_i64(out, ops::decode<int64_t>(v).value_or(0));
                return true;
            }
            case Tag::Char: {
                write_u8(out, SCT_Char);
                write_u32(out, static_cast<uint32_t>(ops::decode<char32_t>(v).value_or(0)));
                return true;
            }
            case Tag::String: {
                write_u8(out, SCT_String);
                auto sv = intern.get_string(ops::payload(v));
                write_str(out, sv.value_or(""));
                return true;
            }
            case Tag::Symbol: {
                write_u8(out, SCT_Symbol);
                auto sv = intern.get_string(ops::payload(v));
                write_str(out, sv.value_or(""));
                return true;
            }
            case Tag::HeapObject:
                return write_heap_value(v, out, root_label);
            case Tag::Nan: {
                write_u8(out, SCT_Double);
                write_f64(out, std::numeric_limits<double>::quiet_NaN());
                return true;
            }
            default:
                return fail(root_label, "unsupported boxed tag");
        }
    }
};

struct SpawnCaptureReader {
    std::span<const uint8_t> data;
    size_t pos{0};

    Heap& heap;
    InternTable& intern;
    SpawnResolveFuncFn resolve_func;
    SpawnResolveGlobalFn resolve_global;

    std::vector<LispVal> closures;
    std::vector<uint32_t> closure_upval_counts;

    static std::unexpected<RuntimeError> err(std::string msg) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::InternalError,
            "spawn-thread capture: " + std::move(msg)}});
    }

    bool read_u8(uint8_t& v) {
        if (pos >= data.size()) return false;
        v = data[pos++];
        return true;
    }

    bool read_u32(uint32_t& v) {
        if (pos + 4 > data.size()) return false;
        v = static_cast<uint32_t>(data[pos])
          | (static_cast<uint32_t>(data[pos + 1]) << 8)
          | (static_cast<uint32_t>(data[pos + 2]) << 16)
          | (static_cast<uint32_t>(data[pos + 3]) << 24);
        pos += 4;
        return true;
    }

    bool read_i64(int64_t& v) {
        if (pos + 8 > data.size()) return false;
        uint64_t u = 0;
        for (int i = 0; i < 8; ++i) u |= (static_cast<uint64_t>(data[pos + i]) << (8 * i));
        pos += 8;
        v = static_cast<int64_t>(u);
        return true;
    }

    bool read_f64(double& v) {
        if (pos + 8 > data.size()) return false;
        uint64_t u = 0;
        for (int i = 0; i < 8; ++i) u |= (static_cast<uint64_t>(data[pos + i]) << (8 * i));
        pos += 8;
        std::memcpy(&v, &u, sizeof(double));
        return true;
    }

    bool read_str(std::string& s) {
        uint32_t len = 0;
        if (!read_u32(len)) return false;
        if (pos + len > data.size()) return false;
        s.assign(reinterpret_cast<const char*>(data.data() + pos), len);
        pos += len;
        return true;
    }

    std::expected<LispVal, RuntimeError> read_value(Heap::ExternalRootFrame& roots) {
        uint8_t tag = 0;
        if (!read_u8(tag)) return err("truncated value tag");

        switch (static_cast<SpawnCaptureTag>(tag)) {
            case SCT_Nil:   return nanbox::Nil;
            case SCT_True:  return nanbox::True;
            case SCT_False: return nanbox::False;

            case SCT_Fixnum: {
                int64_t val = 0;
                if (!read_i64(val)) return err("truncated fixnum");
                auto r = make_fixnum(heap, val);
                if (!r) return std::unexpected(r.error());
                return *r;
            }
            case SCT_Double: {
                double val = 0.0;
                if (!read_f64(val)) return err("truncated flonum");
                auto r = make_flonum(val);
                if (!r) return std::unexpected(r.error());
                return *r;
            }
            case SCT_Char: {
                uint32_t cp = 0;
                if (!read_u32(cp)) return err("truncated char");
                auto r = ops::encode(static_cast<char32_t>(cp));
                if (!r) return std::unexpected(RuntimeError{r.error()});
                return *r;
            }
            case SCT_String: {
                std::string s;
                if (!read_str(s)) return err("truncated string");
                auto r = make_string(heap, intern, s);
                if (!r) return std::unexpected(r.error());
                return *r;
            }
            case SCT_Symbol: {
                std::string s;
                if (!read_str(s)) return err("truncated symbol");
                auto r = make_symbol(intern, s);
                if (!r) return std::unexpected(r.error());
                return *r;
            }
            case SCT_HeapCons: {
                auto car = read_value(roots);
                if (!car) return car;
                roots.push(*car);
                auto cdr = read_value(roots);
                if (!cdr) return cdr;
                roots.push(*cdr);
                auto r = make_cons(heap, *car, *cdr);
                if (!r) return std::unexpected(r.error());
                roots.push(*r);
                return *r;
            }
            case SCT_HeapVec: {
                uint32_t len = 0;
                if (!read_u32(len)) return err("truncated vector length");
                std::vector<LispVal> elems;
                elems.reserve(len);
                for (uint32_t i = 0; i < len; ++i) {
                    auto elem = read_value(roots);
                    if (!elem) return elem;
                    elems.push_back(*elem);
                    roots.push(*elem);
                }
                auto r = make_vector(heap, std::move(elems));
                if (!r) return std::unexpected(r.error());
                roots.push(*r);
                return *r;
            }
            case SCT_ByteVec: {
                uint32_t len = 0;
                if (!read_u32(len)) return err("truncated bytevector length");
                if (pos + len > data.size()) return err("truncated bytevector data");
                std::vector<uint8_t> bytes(data.begin() + pos, data.begin() + pos + len);
                pos += len;
                auto r = make_bytevector(heap, std::move(bytes));
                if (!r) return std::unexpected(r.error());
                roots.push(*r);
                return *r;
            }
            case SCT_ClosureRef: {
                uint32_t id = 0;
                if (!read_u32(id)) return err("truncated closure ref");
                if (id >= closures.size()) return err("closure ref out of range");
                return closures[id];
            }
            case SCT_GlobalRef: {
                uint32_t slot = 0;
                if (!read_u32(slot)) return err("truncated global ref");
                if (!resolve_global) return err("global resolver is not configured");
                auto v = resolve_global(slot);
                if (!v) {
                    return err("global ref slot " + std::to_string(slot) + " is not available");
                }
                return *v;
            }
            default:
                return err("unknown value tag");
        }
    }
};

} ///< namespace detail

/**
 * Serialize spawn-thread captures (upvalues + captured globals), including
 * closure graphs with identity-preserving closure references.
 */
inline std::expected<std::vector<uint8_t>, std::string>
serialize_spawn_capture(
    std::span<const LispVal> upvals,
    std::span<const SpawnCapturedGlobal> globals,
    const Heap& heap,
    const InternTable& intern,
    SpawnClosureFuncIndexFn closure_func_index,
    SpawnGlobalRefSlotFn global_ref_slot = {})
{
    detail::SpawnCaptureWriter writer{heap, intern, std::move(closure_func_index),
                                      std::move(global_ref_slot)};

    std::vector<std::vector<uint8_t>> upval_chunks(upvals.size());
    for (std::size_t i = 0; i < upvals.size(); ++i) {
        const auto root = std::string("upvalue[") + std::to_string(i) + "]";
        if (!writer.write_value(upvals[i], upval_chunks[i], root)) {
            return std::unexpected(writer.error_msg.empty()
                ? std::string("failed to encode upvalue") : writer.error_msg);
        }
    }

    std::vector<std::vector<uint8_t>> global_chunks(globals.size());
    for (std::size_t i = 0; i < globals.size(); ++i) {
        const auto root = std::string("global[") + std::to_string(globals[i].slot) + "]";
        if (!writer.write_value(globals[i].value, global_chunks[i], root)) {
            return std::unexpected(writer.error_msg.empty()
                ? std::string("failed to encode captured global") : writer.error_msg);
        }
    }

    for (std::size_t i = 0; i < writer.closures.size(); ++i) {
        auto& node = writer.closures[i];
        node.encoded_upvals.resize(node.upvals.size());
        for (std::size_t j = 0; j < node.upvals.size(); ++j) {
            const auto root = std::string("closure-upvalue[") + std::to_string(i)
                            + "][" + std::to_string(j) + "]";
            if (!writer.write_value(node.upvals[j], node.encoded_upvals[j], root)) {
                return std::unexpected(writer.error_msg.empty()
                    ? std::string("failed to encode closure upvalue") : writer.error_msg);
            }
        }
    }

    std::vector<uint8_t> out;
    out.reserve(256);

    detail::SpawnCaptureWriter::write_u8(out, SPAWN_CAPTURE_VERSION_BYTE);
    detail::SpawnCaptureWriter::write_u32(out, static_cast<uint32_t>(writer.closures.size()));

    for (const auto& node : writer.closures) {
        detail::SpawnCaptureWriter::write_u32(out, node.func_idx);
        detail::SpawnCaptureWriter::write_u32(out, static_cast<uint32_t>(node.encoded_upvals.size()));
        for (const auto& up_chunk : node.encoded_upvals) {
            out.insert(out.end(), up_chunk.begin(), up_chunk.end());
        }
    }

    detail::SpawnCaptureWriter::write_u32(out, static_cast<uint32_t>(upval_chunks.size()));
    for (const auto& chunk : upval_chunks) {
        out.insert(out.end(), chunk.begin(), chunk.end());
    }

    detail::SpawnCaptureWriter::write_u32(out, static_cast<uint32_t>(globals.size()));
    for (std::size_t i = 0; i < globals.size(); ++i) {
        detail::SpawnCaptureWriter::write_u32(out, globals[i].slot);
        out.insert(out.end(), global_chunks[i].begin(), global_chunks[i].end());
    }

    return out;
}

/**
 * Deserialize a spawn-thread capture payload.
 */
inline std::expected<SpawnCapturePayload, RuntimeError>
deserialize_spawn_capture(
    std::span<const uint8_t> data,
    Heap& heap,
    InternTable& intern,
    SpawnResolveFuncFn resolve_func,
    SpawnResolveGlobalFn resolve_global)
{
    if (data.empty() || data[0] != SPAWN_CAPTURE_VERSION_BYTE) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::InternalError,
            "spawn-thread capture: missing version byte 0xEC"}});
    }

    detail::SpawnCaptureReader reader{data.subspan(1), 0, heap, intern,
                                      std::move(resolve_func), std::move(resolve_global)};

    auto roots = heap.make_external_root_frame();

    uint32_t closure_count = 0;
    if (!reader.read_u32(closure_count)) {
        return detail::SpawnCaptureReader::err("truncated closure count");
    }

    reader.closures.resize(closure_count, nanbox::Nil);
    reader.closure_upval_counts.resize(closure_count, 0);

    for (uint32_t i = 0; i < closure_count; ++i) {
        uint32_t func_idx = 0;
        uint32_t upval_count = 0;
        if (!reader.read_u32(func_idx) || !reader.read_u32(upval_count)) {
            return detail::SpawnCaptureReader::err("truncated closure header");
        }
        if (!reader.resolve_func) {
            return detail::SpawnCaptureReader::err("function resolver is not configured");
        }

        const auto* func = reader.resolve_func(func_idx);
        if (!func) {
            return detail::SpawnCaptureReader::err(
                "closure function index " + std::to_string(func_idx) + " is not available");
        }

        std::vector<LispVal> upvals(upval_count, nanbox::Nil);
        auto cl = make_closure(heap, func, std::move(upvals));
        if (!cl) return std::unexpected(cl.error());

        reader.closures[i] = *cl;
        reader.closure_upval_counts[i] = upval_count;
        roots.push(*cl);
    }

    for (uint32_t i = 0; i < closure_count; ++i) {
        auto* cl = heap.try_get_as<ObjectKind::Closure, types::Closure>(ops::payload(reader.closures[i]));
        if (!cl) {
            return detail::SpawnCaptureReader::err("internal closure placeholder type mismatch");
        }
        for (uint32_t j = 0; j < reader.closure_upval_counts[i]; ++j) {
            auto uv = reader.read_value(roots);
            if (!uv) return std::unexpected(uv.error());
            cl->upvals[j] = *uv;
            roots.push(*uv);
        }
    }

    SpawnCapturePayload payload;

    uint32_t upval_count = 0;
    if (!reader.read_u32(upval_count)) {
        return detail::SpawnCaptureReader::err("truncated upvalue count");
    }
    payload.upvals.reserve(upval_count);
    for (uint32_t i = 0; i < upval_count; ++i) {
        auto uv = reader.read_value(roots);
        if (!uv) return std::unexpected(uv.error());
        payload.upvals.push_back(*uv);
        roots.push(*uv);
    }

    uint32_t global_count = 0;
    if (!reader.read_u32(global_count)) {
        return detail::SpawnCaptureReader::err("truncated captured-global count");
    }
    payload.globals.reserve(global_count);
    for (uint32_t i = 0; i < global_count; ++i) {
        uint32_t slot = 0;
        if (!reader.read_u32(slot)) {
            return detail::SpawnCaptureReader::err("truncated captured-global slot");
        }
        auto gv = reader.read_value(roots);
        if (!gv) return std::unexpected(gv.error());
        payload.globals.push_back(SpawnCapturedGlobal{slot, *gv});
        roots.push(*gv);
    }

    if (reader.pos != reader.data.size()) {
        return detail::SpawnCaptureReader::err("trailing bytes");
    }

    return payload;
}

} ///< namespace eta::nng
