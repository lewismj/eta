#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "eta/runtime/datum_reader.h"
#include "eta/runtime/value_formatter.h"
#include "eta/runtime/types/types.h"

namespace eta::nng {

using namespace eta::runtime;
using namespace eta::runtime::nanbox;
using namespace eta::runtime::error;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;

/**
 * @brief Serialize a LispVal to its s-expression (Write-mode) string.
 *
 * Uses the existing format_value() in Write mode, which produces
 * machine-readable output (quoted strings, #\ char syntax, etc.)
 * suitable for round-trip through deserialize_value().
 *
 * Non-serializable values (closures, continuations, ports, tensors)
 * will produce opaque strings like "#<closure>" that cannot be
 * deserialized.
 */
inline std::string serialize_value(LispVal v, Heap& heap, InternTable& intern) {
    return format_value(v, FormatMode::Write, heap, intern);
}

/**
 * @brief Deserialize an s-expression string back to a LispVal.
 *
 * Parses the string using the Eta reader (Lexer + Parser) and
 * converts the resulting AST into runtime heap objects.
 *
 * @param data   The s-expression text to parse.
 * @param heap   The runtime heap for allocating objects.
 * @param intern The intern table for strings and symbols.
 * @return The deserialized LispVal, or an error on malformed input.
 */
inline std::expected<LispVal, RuntimeError>
deserialize_value(std::string_view data, Heap& heap, InternTable& intern) {
    return parse_datum_string(data, heap, intern);
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 8 — Heartbeat wire format
// ─────────────────────────────────────────────────────────────────────────────

/// Magic byte that marks a heartbeat control message (ping/pong).
/// Distinct from the binary format version byte (0xEA) and all printable ASCII.
constexpr uint8_t HEARTBEAT_VERSION_BYTE = 0xEB;
constexpr uint8_t HB_PING = 0x00;  ///< second byte for ping
constexpr uint8_t HB_PONG = 0x01;  ///< second byte for pong

/// Return true if the buffer contains a heartbeat ping message.
inline bool is_heartbeat_ping(const uint8_t* d, size_t sz) noexcept {
    return sz == 2 && d[0] == HEARTBEAT_VERSION_BYTE && d[1] == HB_PING;
}
/// Return true if the buffer contains a heartbeat pong message.
inline bool is_heartbeat_pong(const uint8_t* d, size_t sz) noexcept {
    return sz == 2 && d[0] == HEARTBEAT_VERSION_BYTE && d[1] == HB_PONG;
}
/// Build a heartbeat ping message.
inline std::vector<uint8_t> make_heartbeat_ping() {
    return {HEARTBEAT_VERSION_BYTE, HB_PING};
}
/// Build a heartbeat pong message.
inline std::vector<uint8_t> make_heartbeat_pong() {
    return {HEARTBEAT_VERSION_BYTE, HB_PONG};
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 6 — Binary wire format
// ─────────────────────────────────────────────────────────────────────────────

/// Magic version byte that marks a binary-format nng message.
/// S-expression messages always start with a printable ASCII character,
/// so 0xEA (outside ASCII printable range) is unambiguous.
constexpr uint8_t BINARY_VERSION_BYTE = 0xEA;

/// Tag bytes used in the binary format.
/// Values 0-12 intentionally match BytecodeSerializer::ConstTag for
/// symmetry; CT_ByteVec (13) is an extension for bytevectors.
enum BinaryTag : uint8_t {
    BT_Nil      =  0,
    BT_True     =  1,
    BT_Fixnum   =  2,   ///< followed by i64 LE
    BT_Double   =  3,   ///< followed by f64 LE (raw bits)
    BT_Char     =  4,   ///< followed by u32 LE codepoint
    BT_String   =  5,   ///< followed by u32 len + UTF-8 bytes
    BT_Symbol   =  6,   ///< followed by u32 len + UTF-8 bytes
    // 7 = FuncIndex — not used in wire format
    BT_HeapCons =  8,   ///< car (recursive) + cdr (recursive)
    BT_HeapVec  =  9,   ///< u32 len + elements (recursive)
    // 10 = RawBits — not used in wire format
    // 11 = HeapNil — not used in wire format
    BT_False    = 12,
    BT_ByteVec  = 13,   ///< u32 len + raw bytes
};

// ── Internal: buffered binary writer ─────────────────────────────────────────

namespace detail {

struct BinaryWriter {
    std::vector<uint8_t>& buf;
    const Heap& heap;
    const InternTable& intern;

    void write_u8(uint8_t v) {
        buf.push_back(v);
    }

    void write_u32(uint32_t v) {
        buf.push_back(static_cast<uint8_t>(v));
        buf.push_back(static_cast<uint8_t>(v >> 8));
        buf.push_back(static_cast<uint8_t>(v >> 16));
        buf.push_back(static_cast<uint8_t>(v >> 24));
    }

    void write_i64(int64_t v) {
        uint64_t u = static_cast<uint64_t>(v);
        for (int i = 0; i < 8; ++i)
            buf.push_back(static_cast<uint8_t>(u >> (8 * i)));
    }

    void write_f64(double v) {
        uint64_t u;
        std::memcpy(&u, &v, 8);
        for (int i = 0; i < 8; ++i)
            buf.push_back(static_cast<uint8_t>(u >> (8 * i)));
    }

    void write_str(std::string_view sv) {
        write_u32(static_cast<uint32_t>(sv.size()));
        buf.insert(buf.end(), sv.begin(), sv.end());
    }

    void write_heap_value(LispVal v) {
        using namespace eta::runtime::memory::heap;
        auto id = ops::payload(v);

        // Heap-allocated big fixnum (int64_t that didn't fit in 47-bit nanbox)
        if (auto* big = heap.try_get_as<ObjectKind::Fixnum, int64_t>(id)) {
            write_u8(BT_Fixnum);
            write_i64(*big);
            return;
        }
        // Cons cell
        if (auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(id)) {
            write_u8(BT_HeapCons);
            write_value(cons->car);
            write_value(cons->cdr);
            return;
        }
        // Vector
        if (auto* vec = heap.try_get_as<ObjectKind::Vector, types::Vector>(id)) {
            write_u8(BT_HeapVec);
            write_u32(static_cast<uint32_t>(vec->elements.size()));
            for (auto elem : vec->elements)
                write_value(elem);
            return;
        }
        // ByteVector
        if (auto* bv = heap.try_get_as<ObjectKind::ByteVector, types::ByteVector>(id)) {
            write_u8(BT_ByteVec);
            write_u32(static_cast<uint32_t>(bv->data.size()));
            buf.insert(buf.end(), bv->data.begin(), bv->data.end());
            return;
        }
        // Non-serializable heap object (closure, continuation, port, tensor, …)
        // → encode as Nil so the message is well-formed rather than corrupt.
        write_u8(BT_Nil);
    }

    void write_value(LispVal v) {
        // Sentinel values
        if (v == Nil)   { write_u8(BT_Nil);   return; }
        if (v == True)  { write_u8(BT_True);  return; }
        if (v == False) { write_u8(BT_False); return; }

        if (!ops::is_boxed(v)) {
            // Raw double (including negative doubles)
            write_u8(BT_Double);
            write_f64(std::bit_cast<double>(v));
            return;
        }

        switch (ops::tag(v)) {
            case Tag::Fixnum: {
                write_u8(BT_Fixnum);
                write_i64(ops::decode<int64_t>(v).value_or(0));
                return;
            }
            case Tag::Char: {
                write_u8(BT_Char);
                write_u32(static_cast<uint32_t>(ops::decode<char32_t>(v).value_or(0)));
                return;
            }
            case Tag::String: {
                write_u8(BT_String);
                auto sv = intern.get_string(ops::payload(v));
                write_str(sv.value_or(""));
                return;
            }
            case Tag::Symbol: {
                write_u8(BT_Symbol);
                auto sv = intern.get_string(ops::payload(v));
                write_str(sv.value_or(""));
                return;
            }
            case Tag::HeapObject: {
                write_heap_value(v);
                return;
            }
            case Tag::Nan: {
                write_u8(BT_Double);
                write_f64(std::numeric_limits<double>::quiet_NaN());
                return;
            }
            default:
                write_u8(BT_Nil);
                return;
        }
    }
};

// ── Internal: buffered binary reader ─────────────────────────────────────────

struct BinaryReader {
    std::span<const uint8_t> data;
    size_t pos = 0;
    Heap& heap;
    InternTable& intern;

    static std::unexpected<RuntimeError> err(const char* msg) {
        return std::unexpected(RuntimeError{
            VMError{RuntimeErrorCode::InternalError,
                    std::string("binary wire format: ") + msg}});
    }

    bool read_u8(uint8_t& v) {
        if (pos >= data.size()) return false;
        v = data[pos++];
        return true;
    }
    bool read_u32(uint32_t& v) {
        if (pos + 4 > data.size()) return false;
        v = static_cast<uint32_t>(data[pos])
          | (static_cast<uint32_t>(data[pos+1]) << 8)
          | (static_cast<uint32_t>(data[pos+2]) << 16)
          | (static_cast<uint32_t>(data[pos+3]) << 24);
        pos += 4;
        return true;
    }
    bool read_i64(int64_t& v) {
        if (pos + 8 > data.size()) return false;
        uint64_t u = 0;
        for (int i = 0; i < 8; ++i)
            u |= (static_cast<uint64_t>(data[pos + i]) << (8 * i));
        v = static_cast<int64_t>(u);
        pos += 8;
        return true;
    }
    bool read_f64(double& v) {
        if (pos + 8 > data.size()) return false;
        uint64_t u = 0;
        for (int i = 0; i < 8; ++i)
            u |= (static_cast<uint64_t>(data[pos + i]) << (8 * i));
        std::memcpy(&v, &u, 8);
        pos += 8;
        return true;
    }
    bool read_str(std::string& s) {
        uint32_t len;
        if (!read_u32(len)) return false;
        if (pos + len > data.size()) return false;
        s.assign(reinterpret_cast<const char*>(data.data() + pos), len);
        pos += len;
        return true;
    }

    std::expected<LispVal, RuntimeError> read_value() {
        uint8_t tag_byte;
        if (!read_u8(tag_byte)) return err("truncated (missing tag byte)");

        using namespace memory::factory;

        switch (static_cast<BinaryTag>(tag_byte)) {
            case BT_Nil:   return Nil;
            case BT_True:  return True;
            case BT_False: return False;

            case BT_Fixnum: {
                int64_t val;
                if (!read_i64(val)) return err("truncated fixnum");
                auto res = make_fixnum(heap, val);
                if (!res) return std::unexpected(res.error());
                return *res;
            }
            case BT_Double: {
                double val;
                if (!read_f64(val)) return err("truncated double");
                auto enc = make_flonum(val);
                if (!enc) return std::unexpected(enc.error());
                return *enc;
            }
            case BT_Char: {
                uint32_t cp;
                if (!read_u32(cp)) return err("truncated char");
                auto enc = ops::encode(static_cast<char32_t>(cp));
                if (!enc) return std::unexpected(RuntimeError{enc.error()});
                return *enc;
            }
            case BT_String: {
                std::string s;
                if (!read_str(s)) return err("truncated string");
                auto res = make_string(heap, intern, s);
                if (!res) return std::unexpected(res.error());
                return *res;
            }
            case BT_Symbol: {
                std::string s;
                if (!read_str(s)) return err("truncated symbol");
                auto res = make_symbol(intern, s);
                if (!res) return std::unexpected(res.error());
                return *res;
            }
            case BT_HeapCons: {
                auto car = read_value();
                if (!car) return car;
                auto cdr = read_value();
                if (!cdr) return cdr;
                auto res = make_cons(heap, *car, *cdr);
                if (!res) return std::unexpected(res.error());
                return *res;
            }
            case BT_HeapVec: {
                uint32_t len;
                if (!read_u32(len)) return err("truncated vector length");
                std::vector<LispVal> elems;
                elems.reserve(len);
                for (uint32_t i = 0; i < len; ++i) {
                    auto elem = read_value();
                    if (!elem) return elem;
                    elems.push_back(*elem);
                }
                auto res = make_vector(heap, std::move(elems));
                if (!res) return std::unexpected(res.error());
                return *res;
            }
            case BT_ByteVec: {
                uint32_t len;
                if (!read_u32(len)) return err("truncated bytevector length");
                if (pos + len > data.size()) return err("truncated bytevector data");
                std::vector<uint8_t> bytes(data.begin() + pos, data.begin() + pos + len);
                pos += len;
                auto res = make_bytevector(heap, std::move(bytes));
                if (!res) return std::unexpected(res.error());
                return *res;
            }
            default:
                return err("unknown binary tag");
        }
    }
};

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Check whether a raw byte buffer starts with the binary wire-format
 *        version byte (0xEA).  Used by recv! to auto-detect the format.
 */
inline bool is_binary_format(const uint8_t* data, size_t size) {
    return size > 0 && data[0] == BINARY_VERSION_BYTE;
}

inline bool is_binary_format(std::span<const uint8_t> data) {
    return !data.empty() && data[0] == BINARY_VERSION_BYTE;
}

/**
 * @brief Serialize a LispVal to the compact binary wire format.
 *
 * The output starts with BINARY_VERSION_BYTE (0xEA) so that recv! can
 * auto-detect and choose the correct deserializer.
 *
 * Supported types: nil, bool, fixnum (any size), flonum, char, string,
 * symbol, cons/list, vector, bytevector (recursive).
 *
 * Non-serializable values (closures, continuations, ports, tensors) are
 * silently encoded as nil.
 *
 * @param v      The value to serialize.
 * @param heap   The heap (needed to dereference heap objects).
 * @param intern The intern table (needed to resolve string/symbol IDs).
 * @return A byte vector starting with 0xEA followed by the encoded value.
 */
inline std::vector<uint8_t>
serialize_binary(LispVal v, const Heap& heap, const InternTable& intern) {
    std::vector<uint8_t> buf;
    buf.reserve(64);
    buf.push_back(BINARY_VERSION_BYTE);
    detail::BinaryWriter w{buf, heap, intern};
    w.write_value(v);
    return buf;
}

/**
 * @brief Deserialize a binary wire-format buffer back to a LispVal.
 *
 * @param data   Buffer starting with 0xEA (the version byte).
 * @param heap   The heap for allocating new objects.
 * @param intern The intern table for interning strings and symbols.
 * @return The deserialized LispVal, or an error if the data is malformed.
 */
inline std::expected<LispVal, RuntimeError>
deserialize_binary(std::span<const uint8_t> data, Heap& heap, InternTable& intern) {
    if (data.empty() || data[0] != BINARY_VERSION_BYTE) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::InternalError,
            "binary wire format: missing version byte 0xEA"}});
    }
    detail::BinaryReader r{data.subspan(1), 0, heap, intern};
    return r.read_value();
}

} // namespace eta::nng

