#include "bytecode_serializer.h"

#include <bit>
#include <cstring>
#include <ostream>
#include <istream>

#include <boost/container_hash/hash.hpp>

#include "eta/runtime/types/types.h"
#include "eta/runtime/factory.h"

namespace eta::runtime::vm {

using namespace nanbox;

// ============================================================================
// Binary I/O helpers (little-endian)
// ============================================================================

void BytecodeSerializer::write_u8 (std::ostream& os, uint8_t  v) { os.write(reinterpret_cast<const char*>(&v), 1); }
void BytecodeSerializer::write_u16(std::ostream& os, uint16_t v) { os.write(reinterpret_cast<const char*>(&v), 2); }
void BytecodeSerializer::write_u32(std::ostream& os, uint32_t v) { os.write(reinterpret_cast<const char*>(&v), 4); }
void BytecodeSerializer::write_u64(std::ostream& os, uint64_t v) { os.write(reinterpret_cast<const char*>(&v), 8); }
void BytecodeSerializer::write_i64(std::ostream& os, int64_t  v) { os.write(reinterpret_cast<const char*>(&v), 8); }
void BytecodeSerializer::write_f64(std::ostream& os, double   v) { os.write(reinterpret_cast<const char*>(&v), 8); }

void BytecodeSerializer::write_str(std::ostream& os, std::string_view s) {
    write_u32(os, static_cast<uint32_t>(s.size()));
    os.write(s.data(), static_cast<std::streamsize>(s.size()));
}

bool BytecodeSerializer::read_u8 (std::istream& is, uint8_t&  v) { is.read(reinterpret_cast<char*>(&v), 1); return is.good(); }
bool BytecodeSerializer::read_u16(std::istream& is, uint16_t& v) { is.read(reinterpret_cast<char*>(&v), 2); return is.good(); }
bool BytecodeSerializer::read_u32(std::istream& is, uint32_t& v) { is.read(reinterpret_cast<char*>(&v), 4); return is.good(); }
bool BytecodeSerializer::read_u64(std::istream& is, uint64_t& v) { is.read(reinterpret_cast<char*>(&v), 8); return is.good(); }
bool BytecodeSerializer::read_i64(std::istream& is, int64_t&  v) { is.read(reinterpret_cast<char*>(&v), 8); return is.good(); }
bool BytecodeSerializer::read_f64(std::istream& is, double&   v) { is.read(reinterpret_cast<char*>(&v), 8); return is.good(); }

bool BytecodeSerializer::read_str(std::istream& is, std::string& s) {
    uint32_t len;
    if (!read_u32(is, len)) return false;
    s.resize(len);
    is.read(s.data(), len);
    return is.good();
}

// ============================================================================
// Source hash (boost::hash)
// ============================================================================

uint64_t BytecodeSerializer::hash_source(std::string_view source) {
    boost::hash<std::string_view> hasher;
    return static_cast<uint64_t>(hasher(source));
}

// ============================================================================
// Constant serialization
// ============================================================================

void BytecodeSerializer::write_constant(std::ostream& os, LispVal v) const {
    // Check special sentinels first
    if (v == Nil) { write_u8(os, CT_Nil); return; }
    if (v == True) { write_u8(os, CT_True); return; }
    if (v == False) { write_u8(os, CT_False); return; }

    // Boxed values: dispatch on tag below
    if (ops::is_boxed(v)) {
        // fall through to the switch below
    }
    // Unboxed: either a func_index sentinel or a raw double.
    // IMPORTANT: is_func_index must be checked carefully because bit 63
    // (FUNC_INDEX_TAG) is also the IEEE-754 sign bit of negative doubles.
    // is_func_index additionally verifies bits 62-32 are zero, which is
    // true for func_index values (uint32 payload) but not for neg doubles.
    else if (is_func_index(v)) {
        write_u8(os, CT_FuncIndex);
        write_u32(os, decode_func_index(v));
        return;
    }
    else {
        // Raw double (including negative doubles)
        write_u8(os, CT_Double);
        write_f64(os, std::bit_cast<double>(v));
        return;
    }

    // Boxed values — dispatch on tag
    auto tag = ops::tag(v);
    switch (tag) {
        case Tag::Fixnum: {
            write_u8(os, CT_Fixnum);
            auto val = ops::decode<int64_t>(v);
            write_i64(os, val.value_or(0));
            return;
        }
        case Tag::Char: {
            write_u8(os, CT_Char);
            auto val = ops::decode<char32_t>(v);
            write_u32(os, static_cast<uint32_t>(val.value_or(0)));
            return;
        }
        case Tag::String: {
            write_u8(os, CT_String);
            auto sv = intern_table_.get_string(ops::payload(v));
            write_str(os, sv.value_or(""));
            return;
        }
        case Tag::Symbol: {
            write_u8(os, CT_Symbol);
            auto sv = intern_table_.get_string(ops::payload(v));
            write_str(os, sv.value_or(""));
            return;
        }
        case Tag::HeapObject: {
            write_heap_value(os, v);
            return;
        }
        case Tag::Nan: {
            write_u8(os, CT_Double);
            write_f64(os, std::numeric_limits<double>::quiet_NaN());
            return;
        }
        case Tag::Nil:
            // Nil is handled above (sentinel check), but must be listed
            // explicitly to satisfy -Wswitch-enum.
            write_u8(os, CT_Nil);
            return;
    }
}

void BytecodeSerializer::write_heap_value(std::ostream& os, LispVal v) const {
    using namespace memory::heap;
    auto id = ops::payload(v);

    // Cons cell → recursive
    if (auto* cons = heap_.try_get_as<ObjectKind::Cons, types::Cons>(id)) {
        write_u8(os, CT_HeapCons);
        write_constant(os, cons->car);
        write_constant(os, cons->cdr);
        return;
    }

    // Vector → length + recursive elements
    if (auto* vec = heap_.try_get_as<ObjectKind::Vector, types::Vector>(id)) {
        write_u8(os, CT_HeapVec);
        write_u32(os, static_cast<uint32_t>(vec->elements.size()));
        for (auto elem : vec->elements) {
            write_constant(os, elem);
        }
        return;
    }

    // Fallback for other heap objects: raw bits
    write_u8(os, CT_RawBits);
    write_u64(os, v);
}

std::expected<LispVal, SerializerError>
BytecodeSerializer::read_constant(std::istream& is) const {
    uint8_t tag;
    if (!read_u8(is, tag)) return std::unexpected(SerializerError::Truncated);

    switch (static_cast<ConstTag>(tag)) {
        case CT_Nil:  return Nil;
        case CT_True: return True;
        case CT_False: return False;

        case CT_Fixnum: {
            int64_t val;
            if (!read_i64(is, val)) return std::unexpected(SerializerError::Truncated);
            auto enc = ops::encode(val);
            if (!enc) return std::unexpected(SerializerError::CorruptConstant);
            return *enc;
        }
        case CT_Double: {
            double val;
            if (!read_f64(is, val)) return std::unexpected(SerializerError::Truncated);
            auto enc = ops::encode(val);
            if (!enc) return std::unexpected(SerializerError::CorruptConstant);
            return *enc;
        }
        case CT_Char: {
            uint32_t cp;
            if (!read_u32(is, cp)) return std::unexpected(SerializerError::Truncated);
            auto enc = ops::encode(static_cast<char32_t>(cp));
            if (!enc) return std::unexpected(SerializerError::CorruptConstant);
            return *enc;
        }
        case CT_String: {
            std::string text;
            if (!read_str(is, text)) return std::unexpected(SerializerError::Truncated);
            auto id_res = intern_table_.intern(text);
            if (!id_res) return std::unexpected(SerializerError::CorruptConstant);
            return ops::box(Tag::String, *id_res);
        }
        case CT_Symbol: {
            std::string text;
            if (!read_str(is, text)) return std::unexpected(SerializerError::Truncated);
            auto id_res = intern_table_.intern(text);
            if (!id_res) return std::unexpected(SerializerError::CorruptConstant);
            return ops::box(Tag::Symbol, *id_res);
        }
        case CT_FuncIndex: {
            uint32_t idx;
            if (!read_u32(is, idx)) return std::unexpected(SerializerError::Truncated);
            return encode_func_index(idx);
        }
        case CT_HeapCons: {
            auto car = read_constant(is);
            if (!car) return car;
            auto cdr = read_constant(is);
            if (!cdr) return cdr;
            auto res = memory::factory::make_cons(heap_, *car, *cdr);
            if (!res) return std::unexpected(SerializerError::CorruptConstant);
            return *res;
        }
        case CT_HeapVec: {
            uint32_t len;
            if (!read_u32(is, len)) return std::unexpected(SerializerError::Truncated);
            std::vector<LispVal> elems;
            elems.reserve(len);
            for (uint32_t i = 0; i < len; ++i) {
                auto elem = read_constant(is);
                if (!elem) return elem;
                elems.push_back(*elem);
            }
            auto res = memory::factory::make_vector(heap_, std::move(elems));
            if (!res) return std::unexpected(SerializerError::CorruptConstant);
            return *res;
        }
        case CT_HeapNil: return Nil;
        case CT_RawBits: {
            uint64_t bits;
            if (!read_u64(is, bits)) return std::unexpected(SerializerError::Truncated);
            return bits;
        }
        default:
            return std::unexpected(SerializerError::CorruptConstant);
    }
}

// ============================================================================
// Serialize
// ============================================================================

bool BytecodeSerializer::serialize(
        const std::vector<ModuleEntry>& modules,
        const semantics::BytecodeFunctionRegistry& registry,
        uint64_t source_hash,
        bool include_debug,
        std::ostream& os,
        const std::vector<std::string>& imports,
        uint32_t num_builtins) const
{
    // ── Header ──────────────────────────────────────────────────
    os.write(MAGIC, 4);
    write_u16(os, FORMAT_VERSION);

    uint16_t flags = 0;
    if (include_debug) flags |= FLAG_HAS_DEBUG;
    write_u16(os, flags);

    write_u64(os, source_hash);
    write_u32(os, num_builtins);

    write_u32(os, static_cast<uint32_t>(modules.size()));
    write_u32(os, static_cast<uint32_t>(registry.size()));

    // ── Imports table ────────────────────────────────────────────
    write_u32(os, static_cast<uint32_t>(imports.size()));
    for (const auto& imp : imports) {
        write_str(os, imp);
    }

    // ── Module table ────────────────────────────────────────────
    for (const auto& mod : modules) {
        write_str(os, mod.name);
        write_u32(os, mod.init_func_index);
        write_u32(os, mod.total_globals);
        uint8_t has_main = mod.main_func_slot.has_value() ? 1 : 0;
        write_u8(os, has_main);
        write_u32(os, mod.main_func_slot.value_or(0xFFFFFFFFu));
    }

    // ── Function table ──────────────────────────────────────────
    const auto& funcs = registry.all();
    for (const auto& func : funcs) {
        write_str(os, func.name);
        write_u32(os, func.arity);
        write_u8(os, func.has_rest ? 1 : 0);
        write_u32(os, func.stack_size);

        // Constants
        write_u32(os, static_cast<uint32_t>(func.constants.size()));
        for (auto c : func.constants) {
            write_constant(os, c);
        }

        // Instructions
        write_u32(os, static_cast<uint32_t>(func.code.size()));
        for (const auto& instr : func.code) {
            write_u8(os, static_cast<uint8_t>(instr.opcode));
            write_u32(os, instr.arg);
        }

        // Source map (optional)
        if (include_debug) {
            for (std::size_t i = 0; i < func.code.size(); ++i) {
                auto span = func.span_at(static_cast<uint32_t>(i));
                write_u32(os, span.file_id);
                write_u32(os, span.start.line);
                write_u32(os, span.start.column);
                write_u32(os, span.end.line);
                write_u32(os, span.end.column);
            }
        }

        // Local names
        write_u32(os, static_cast<uint32_t>(func.local_names.size()));
        for (const auto& n : func.local_names) write_str(os, n);

        // Upval names
        write_u32(os, static_cast<uint32_t>(func.upval_names.size()));
        for (const auto& n : func.upval_names) write_str(os, n);
    }

    return os.good();
}

// ============================================================================
// Deserialize
// ============================================================================

std::expected<EtacFile, SerializerError>
BytecodeSerializer::deserialize(std::istream& is, uint32_t expected_builtins) const
{
    EtacFile result;

    // ── Header ──────────────────────────────────────────────────
    char magic[4];
    is.read(magic, 4);
    if (!is.good() || std::memcmp(magic, MAGIC, 4) != 0)
        return std::unexpected(SerializerError::BadMagic);

    uint16_t version;
    if (!read_u16(is, version)) return std::unexpected(SerializerError::Truncated);
    if (version != FORMAT_VERSION) return std::unexpected(SerializerError::VersionMismatch);

    uint16_t flags;
    if (!read_u16(is, flags)) return std::unexpected(SerializerError::Truncated);
    bool has_debug = (flags & FLAG_HAS_DEBUG) != 0;

    if (!read_u64(is, result.source_hash)) return std::unexpected(SerializerError::Truncated);

    uint32_t file_builtins;
    if (!read_u32(is, file_builtins)) return std::unexpected(SerializerError::Truncated);
    if (expected_builtins != 0 && file_builtins != expected_builtins)
        return std::unexpected(SerializerError::BuiltinCountMismatch);

    uint32_t num_modules, num_functions;
    if (!read_u32(is, num_modules)) return std::unexpected(SerializerError::Truncated);
    if (!read_u32(is, num_functions)) return std::unexpected(SerializerError::Truncated);

    // ── Imports table ────────────────────────────────────────────
    uint32_t num_imports;
    if (!read_u32(is, num_imports)) return std::unexpected(SerializerError::Truncated);
    result.imports.resize(num_imports);
    for (uint32_t i = 0; i < num_imports; ++i) {
        if (!read_str(is, result.imports[i])) return std::unexpected(SerializerError::Truncated);
    }

    // ── Module table ────────────────────────────────────────────
    result.modules.resize(num_modules);
    for (uint32_t m = 0; m < num_modules; ++m) {
        auto& mod = result.modules[m];
        if (!read_str(is, mod.name)) return std::unexpected(SerializerError::Truncated);
        if (!read_u32(is, mod.init_func_index)) return std::unexpected(SerializerError::Truncated);
        if (!read_u32(is, mod.total_globals)) return std::unexpected(SerializerError::Truncated);
        uint8_t has_main;
        if (!read_u8(is, has_main)) return std::unexpected(SerializerError::Truncated);
        uint32_t main_slot;
        if (!read_u32(is, main_slot)) return std::unexpected(SerializerError::Truncated);
        if (has_main) mod.main_func_slot = main_slot;
    }

    // ── Function table ──────────────────────────────────────────
    for (uint32_t f = 0; f < num_functions; ++f) {
        BytecodeFunction func;

        if (!read_str(is, func.name)) return std::unexpected(SerializerError::Truncated);
        if (!read_u32(is, func.arity)) return std::unexpected(SerializerError::Truncated);
        uint8_t has_rest;
        if (!read_u8(is, has_rest)) return std::unexpected(SerializerError::Truncated);
        func.has_rest = (has_rest != 0);
        if (!read_u32(is, func.stack_size)) return std::unexpected(SerializerError::Truncated);

        // Constants
        uint32_t num_consts;
        if (!read_u32(is, num_consts)) return std::unexpected(SerializerError::Truncated);
        func.constants.reserve(num_consts);
        for (uint32_t i = 0; i < num_consts; ++i) {
            auto c = read_constant(is);
            if (!c) return std::unexpected(c.error());
            func.constants.push_back(*c);
        }

        // Instructions
        uint32_t num_instrs;
        if (!read_u32(is, num_instrs)) return std::unexpected(SerializerError::Truncated);
        func.code.reserve(num_instrs);
        for (uint32_t i = 0; i < num_instrs; ++i) {
            uint8_t op;
            uint32_t arg;
            if (!read_u8(is, op)) return std::unexpected(SerializerError::Truncated);
            if (!read_u32(is, arg)) return std::unexpected(SerializerError::Truncated);
            func.code.push_back({static_cast<OpCode>(op), arg});
        }

        // Source map
        if (has_debug) {
            func.source_map.reserve(num_instrs);
            for (uint32_t i = 0; i < num_instrs; ++i) {
                reader::lexer::Span sp{};
                if (!read_u32(is, sp.file_id)) return std::unexpected(SerializerError::Truncated);
                if (!read_u32(is, sp.start.line)) return std::unexpected(SerializerError::Truncated);
                if (!read_u32(is, sp.start.column)) return std::unexpected(SerializerError::Truncated);
                if (!read_u32(is, sp.end.line)) return std::unexpected(SerializerError::Truncated);
                if (!read_u32(is, sp.end.column)) return std::unexpected(SerializerError::Truncated);
                func.source_map.push_back(sp);
            }
        }

        // Local names
        uint32_t num_locals;
        if (!read_u32(is, num_locals)) return std::unexpected(SerializerError::Truncated);
        func.local_names.resize(num_locals);
        for (uint32_t i = 0; i < num_locals; ++i) {
            if (!read_str(is, func.local_names[i])) return std::unexpected(SerializerError::Truncated);
        }

        // Upval names
        uint32_t num_upvals;
        if (!read_u32(is, num_upvals)) return std::unexpected(SerializerError::Truncated);
        func.upval_names.resize(num_upvals);
        for (uint32_t i = 0; i < num_upvals; ++i) {
            if (!read_str(is, func.upval_names[i])) return std::unexpected(SerializerError::Truncated);
        }

        result.registry.add(std::move(func));
    }

    return result;
}

} // namespace eta::runtime::vm

