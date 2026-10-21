#include "bytecode_serializer.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <istream>
#include <ostream>
#include <sstream>

#include <boost/container_hash/hash.hpp>

#include "eta/runtime/types/types.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/numeric_ops.h"

namespace eta::runtime::vm {

using namespace nanbox;

/**
 * Little-endian conversion helpers
 * All multi-byte values are stored in little-endian order on disk.
 * On little-endian hosts these are no-ops; on big-endian hosts they swap bytes.
 */

namespace {

template<std::integral T>
T host_to_le(T v) noexcept {
    if constexpr (std::endian::native == std::endian::little)
        return v;
    return std::byteswap(v);
}

inline double host_to_le_f64(double v) noexcept {
    if constexpr (std::endian::native == std::endian::little)
        return v;
    return std::bit_cast<double>(std::byteswap(std::bit_cast<uint64_t>(v)));
}

inline int64_t host_to_le_i64(int64_t v) noexcept {
    if constexpr (std::endian::native == std::endian::little)
        return v;
    return static_cast<int64_t>(std::byteswap(static_cast<uint64_t>(v)));
}

constexpr uint64_t kFNVOffsetBasis = 14695981039346656037ull;
constexpr uint64_t kFNVPrime = 1099511628211ull;

uint64_t fnv1a64(std::string_view text, uint64_t seed = kFNVOffsetBasis) noexcept {
    uint64_t hash = seed;
    for (const unsigned char c : text) {
        hash ^= static_cast<uint64_t>(c);
        hash *= kFNVPrime;
    }
    return hash;
}

void store_u64_le(uint64_t value, std::array<uint8_t, 16>& out, std::size_t offset) noexcept {
    for (std::size_t i = 0; i < 8; ++i) {
        out[offset + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFFu);
    }
}

std::array<uint8_t, 16> make_fingerprint128(std::string_view input) noexcept {
    const uint64_t lo = fnv1a64(input, kFNVOffsetBasis);
    const uint64_t hi = fnv1a64(input, kFNVOffsetBasis ^ 0x9e3779b97f4a7c15ull);

    std::array<uint8_t, 16> out{};
    store_u64_le(lo, out, 0);
    store_u64_le(hi, out, 8);
    return out;
}

std::string build_compiler_fingerprint_input() {
    std::ostringstream oss;
    oss << "eta-bytecode-v6";

#if defined(_MSC_FULL_VER)
    oss << "|msvc:" << _MSC_FULL_VER;
#elif defined(__clang__)
    oss << "|clang:" << __clang_major__ << "." << __clang_minor__ << "." << __clang_patchlevel__;
#elif defined(__GNUC__)
    oss << "|gcc:" << __GNUC__ << "." << __GNUC_MINOR__ << "." << __GNUC_PATCHLEVEL__;
#else
    oss << "|compiler:unknown";
#endif

    oss << "|cplusplus:" << __cplusplus;

#if defined(NDEBUG)
    oss << "|ndebug:1";
#else
    oss << "|ndebug:0";
#endif

#if defined(USE_PEXT)
    oss << "|pext:1";
#else
    oss << "|pext:0";
#endif

#if defined(ETA_CLP_QP_BACKEND)
    oss << "|clp_qp:1";
#else
    oss << "|clp_qp:0";
#endif

    oss << "|ptr:" << (sizeof(void*) * 8u);

#if defined(_WIN32)
    oss << "|os:windows";
#elif defined(__APPLE__)
    oss << "|os:macos";
#elif defined(__linux__)
    oss << "|os:linux";
#else
    oss << "|os:unknown";
#endif

    return oss.str();
}

/**
 * Bytes left between the read position and end of @p is.
 * Non-seekable streams report no limit, so count checks pass through.
 */
uint64_t remaining_bytes(std::istream& is) {
    const auto pos = is.tellg();
    if (pos == std::streampos(-1)) return std::numeric_limits<uint64_t>::max();
    is.seekg(0, std::ios::end);
    const auto end = is.tellg();
    is.seekg(pos);
    if (end == std::streampos(-1) || end < pos) return std::numeric_limits<uint64_t>::max();
    return static_cast<uint64_t>(end - pos);
}

/**
 * True if @p count elements of at least @p min_elem_size bytes each can still
 * be present in @p is.  Rejects crafted counts before anything is allocated.
 */
bool count_fits(std::istream& is, uint32_t count, uint64_t min_elem_size) {
    return static_cast<uint64_t>(count) * min_elem_size <= remaining_bytes(is);
}

/// Minimum on-disk sizes, used to bound counts against the remaining stream.
constexpr uint64_t kMinStr          = 4;              ///< u32 length prefix
constexpr uint64_t kMinConst        = 1;              ///< tag byte
constexpr uint64_t kMinInstr        = 1 + 4;          ///< opcode + arg
constexpr uint64_t kMinSpan         = 5 * 4;          ///< file_id + 2x(line, column)
constexpr uint64_t kMinDepHash      = kMinStr + 8;
constexpr uint64_t kMinImportBind   = 4 + 2 * kMinStr;
constexpr uint64_t kMinExportBind   = kMinStr + 4;
constexpr uint64_t kMinModuleLegacy = kMinStr + 4 + 4 + 1 + 4;
constexpr uint64_t kMinModuleV5     = kMinModuleLegacy + 5 * 4;
constexpr uint64_t kMinFunction     = kMinStr + 4 + 1 + 4 + 4 * 4; ///< header + 4 empty tables

} ///< anonymous namespace

/**
 * Binary I/O helpers (little-endian)
 */

void BytecodeSerializer::write_u8 (std::ostream& os, uint8_t  v) { os.write(reinterpret_cast<const char*>(&v), 1); }
void BytecodeSerializer::write_u16(std::ostream& os, uint16_t v) { v = host_to_le(v); os.write(reinterpret_cast<const char*>(&v), 2); }
void BytecodeSerializer::write_u32(std::ostream& os, uint32_t v) { v = host_to_le(v); os.write(reinterpret_cast<const char*>(&v), 4); }
void BytecodeSerializer::write_u64(std::ostream& os, uint64_t v) { v = host_to_le(v); os.write(reinterpret_cast<const char*>(&v), 8); }
void BytecodeSerializer::write_i64(std::ostream& os, int64_t  v) { v = host_to_le_i64(v); os.write(reinterpret_cast<const char*>(&v), 8); }
void BytecodeSerializer::write_f64(std::ostream& os, double   v) { v = host_to_le_f64(v); os.write(reinterpret_cast<const char*>(&v), 8); }

void BytecodeSerializer::write_str(std::ostream& os, std::string_view s) {
    /// Mirror read_str's limit so we never emit an artifact we cannot load.
    if (s.size() > MAX_STRING_LEN) {
        os.setstate(std::ios::failbit);
        return;
    }
    write_u32(os, static_cast<uint32_t>(s.size()));
    os.write(s.data(), static_cast<std::streamsize>(s.size()));
}

bool BytecodeSerializer::read_u8 (std::istream& is, uint8_t&  v) { is.read(reinterpret_cast<char*>(&v), 1); return is.good(); }
bool BytecodeSerializer::read_u16(std::istream& is, uint16_t& v) { is.read(reinterpret_cast<char*>(&v), 2); if (!is.good()) return false; v = host_to_le(v); return true; }
bool BytecodeSerializer::read_u32(std::istream& is, uint32_t& v) { is.read(reinterpret_cast<char*>(&v), 4); if (!is.good()) return false; v = host_to_le(v); return true; }
bool BytecodeSerializer::read_u64(std::istream& is, uint64_t& v) { is.read(reinterpret_cast<char*>(&v), 8); if (!is.good()) return false; v = host_to_le(v); return true; }
bool BytecodeSerializer::read_i64(std::istream& is, int64_t&  v) { is.read(reinterpret_cast<char*>(&v), 8); if (!is.good()) return false; v = host_to_le_i64(v); return true; }
bool BytecodeSerializer::read_f64(std::istream& is, double&   v) { is.read(reinterpret_cast<char*>(&v), 8); if (!is.good()) return false; v = host_to_le_f64(v); return true; }

bool BytecodeSerializer::read_str(std::istream& is, std::string& s) {
    uint32_t len;
    if (!read_u32(is, len)) return false;
    /// Reject strings that would cause unbounded allocation (DoS guard).
    if (len > MAX_STRING_LEN || !count_fits(is, len, 1)) return false;
    s.resize(len);
    is.read(s.data(), len);
    return is.good();
}

/**
 * Source hash (boost::hash)
 */

uint64_t BytecodeSerializer::hash_source(std::string_view source) {
    boost::hash<std::string_view> hasher;
    return static_cast<uint64_t>(hasher(source));
}

std::array<uint8_t, 16> BytecodeSerializer::default_compiler_id() {
    static const auto compiler_id = make_fingerprint128(build_compiler_fingerprint_input());
    return compiler_id;
}

FreshnessResult
BytecodeSerializer::check_freshness(const EtacFile& file, const FreshnessContext& context) {
    if (file.format_version != FORMAT_VERSION_V3
        && file.format_version != FORMAT_VERSION_V4
        && file.format_version != FORMAT_VERSION_V5
        && file.format_version != FORMAT_VERSION_V6
        && file.format_version != FORMAT_VERSION) {
        return FreshnessResult{
            .status = FreshnessStatus::UnsupportedFormat,
            .detail = "format version " + std::to_string(file.format_version),
        };
    }

    if (context.expected_compiler_id.has_value()) {
        if (!file.has_compiler_id) {
            return FreshnessResult{
                .status = FreshnessStatus::MissingCompilerId,
                .detail = "legacy artifact has no compiler fingerprint",
            };
        }
        if (file.compiler_id != *context.expected_compiler_id) {
            return FreshnessResult{
                .status = FreshnessStatus::CompilerIdMismatch,
                .detail = "artifact compiler fingerprint does not match runtime",
            };
        }
    }

    if (context.expected_builtin_count.has_value()
        && file.builtin_count != *context.expected_builtin_count) {
        return FreshnessResult{
            .status = FreshnessStatus::BuiltinCountMismatch,
            .detail = "artifact builtins=" + std::to_string(file.builtin_count)
                + ", runtime builtins=" + std::to_string(*context.expected_builtin_count),
        };
    }

    if (context.expected_extension_env_hash.has_value()) {
        if (!file.has_extension_env_hash) {
            if (*context.expected_extension_env_hash != 0) {
                return FreshnessResult{
                    .status = FreshnessStatus::MissingExtensionEnvHash,
                    .detail = "legacy artifact has no extension environment hash",
                };
            }
        } else if (file.extension_env_hash != *context.expected_extension_env_hash) {
            return FreshnessResult{
                .status = FreshnessStatus::ExtensionEnvHashMismatch,
                .detail = "artifact extension hash=" + std::to_string(file.extension_env_hash)
                    + ", runtime extension hash="
                    + std::to_string(*context.expected_extension_env_hash),
            };
        }
    }

    if (context.expected_source_hash.has_value()
        && file.source_hash != *context.expected_source_hash) {
        return FreshnessResult{
            .status = FreshnessStatus::SourceHashMismatch,
            .detail = "artifact source hash mismatch",
        };
    }

    if (context.expected_manifest_hash.has_value()) {
        if (!file.package_metadata.has_value()) {
            return FreshnessResult{
                .status = FreshnessStatus::ManifestHashMismatch,
                .detail = "artifact has no package metadata",
            };
        }
        if (file.package_metadata->manifest_hash != *context.expected_manifest_hash) {
            return FreshnessResult{
                .status = FreshnessStatus::ManifestHashMismatch,
                .detail = "artifact manifest hash mismatch",
            };
        }
    }

    if (!context.expected_dependency_hashes.empty()) {
        for (const auto& expected : context.expected_dependency_hashes) {
            auto it = std::find_if(
                file.dependency_hashes.begin(),
                file.dependency_hashes.end(),
                [&expected](const DependencyHashEntry& actual) {
                    return actual.dependency == expected.dependency;
                });
            if (it == file.dependency_hashes.end()) {
                return FreshnessResult{
                    .status = FreshnessStatus::DependencyHashMismatch,
                    .detail = "missing dependency hash for '" + expected.dependency + "'",
                };
            }
            if (it->etac_hash != expected.etac_hash) {
                return FreshnessResult{
                    .status = FreshnessStatus::DependencyHashMismatch,
                    .detail = "dependency hash mismatch for '" + expected.dependency + "'",
                };
            }
        }
    }

    return FreshnessResult{};
}

/**
 * Constant serialization
 */

void BytecodeSerializer::write_constant(std::ostream& os, LispVal v, uint32_t depth) const {
    /// Check special sentinels first
    if (v == Nil) { write_u8(os, CT_Nil); return; }
    if (v == True) { write_u8(os, CT_True); return; }
    if (v == False) { write_u8(os, CT_False); return; }

    /// Boxed values: dispatch on tag below
    if (ops::is_boxed(v)) {
        /// fall through to the switch below
    }
    /**
     * Unboxed: either a func_index sentinel or a raw double.
     * IMPORTANT: is_func_index must be checked carefully because bit 63
     * (FUNC_INDEX_TAG) is also the IEEE-754 sign bit of negative doubles.
     * is_func_index additionally verifies bits 62-32 are zero, which is
     * true for func_index values (uint32 payload) but not for neg doubles.
     */
    else if (is_func_index(v)) {
        write_u8(os, CT_FuncIndex);
        write_u32(os, decode_func_index(v));
        return;
    }
    else {
        /// Raw double (including negative doubles)
        write_u8(os, CT_Double);
        write_f64(os, std::bit_cast<double>(v));
        return;
    }

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
            write_heap_value(os, v, depth);
            return;
        }
        case Tag::Nan: {
            write_u8(os, CT_Double);
            write_f64(os, std::numeric_limits<double>::quiet_NaN());
            return;
        }
        case Tag::Nil:
            /**
             * Nil is handled above (sentinel check), but must be listed
             * explicitly to satisfy -Wswitch-enum.
             */
            write_u8(os, CT_Nil);
            return;
        case Tag::TapeRef:
            /**
             * TapeRef values are runtime-only and should never appear in compiled
             * bytecode.  Emit CT_Nil as a safe placeholder so the constant-count
             * alignment in the file is preserved if one somehow slips through.
             */
            write_u8(os, CT_Nil);
            return;
    }
}

void BytecodeSerializer::write_heap_value(std::ostream& os, LispVal v, uint32_t depth) const {
    using namespace memory::heap;
    auto id = ops::payload(v);

    if (heap_.try_get_as<ObjectKind::Cons, types::Cons>(id)) {
        /**
         * Refuse nesting read_constant would reject, so a compiled artifact
         * always loads (same contract as write_str's MAX_STRING_LEN).
         */
        if (depth >= MAX_CONST_DEPTH) { os.setstate(std::ios::failbit); return; }
        /**
         * The encoding nests each cdr inside its cons, but walk the spine
         * iteratively so a long quoted list doesn't recurse once per element.
         */
        LispVal cur = v;
        while (ops::is_boxed(cur) && ops::tag(cur) == Tag::HeapObject) {
            auto* cons = heap_.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
            if (!cons) break;
            write_u8(os, CT_HeapCons);
            write_constant(os, cons->car, depth + 1);
            cur = cons->cdr;
        }
        write_constant(os, cur, depth + 1);
        return;
    }

    if (auto* vec = heap_.try_get_as<ObjectKind::Vector, types::Vector>(id)) {
        if (depth >= MAX_CONST_DEPTH) { os.setstate(std::ios::failbit); return; }
        write_u8(os, CT_HeapVec);
        write_u32(os, static_cast<uint32_t>(vec->elements.size()));
        for (auto elem : vec->elements) {
            write_constant(os, elem, depth + 1);
        }
        return;
    }

    /// Fallback for other heap objects: raw bits
    write_u8(os, CT_RawBits);
    write_u64(os, v);
}

std::expected<LispVal, SerializerError>
BytecodeSerializer::read_constant(std::istream& is, uint32_t depth) const {
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
            /// Mirrors write_heap_value: cars nest, the cdr spine is a loop.
            if (depth >= MAX_CONST_DEPTH) return std::unexpected(SerializerError::CorruptConstant);
            auto roots = heap_.make_external_root_frame();
            std::vector<LispVal> cars;
            for (;;) {
                auto car = read_constant(is, depth + 1);
                if (!car) return car;
                cars.push_back(*car);
                roots.push(*car);
                if (is.peek() != CT_HeapCons) break;
                is.get(); ///< consume the next spine cell's tag
            }
            auto tail = read_constant(is, depth + 1);
            if (!tail) return tail;
            LispVal list = *tail;
            roots.push(list);
            for (auto it = cars.rbegin(); it != cars.rend(); ++it) {
                auto res = memory::factory::make_cons(heap_, *it, list);
                if (!res) return std::unexpected(SerializerError::CorruptConstant);
                list = *res;
                roots.push(list);
            }
            return list;
        }
        case CT_HeapVec: {
            if (depth >= MAX_CONST_DEPTH) return std::unexpected(SerializerError::CorruptConstant);
            uint32_t len;
            if (!read_u32(is, len)) return std::unexpected(SerializerError::Truncated);
            /// Guard against unbounded vector allocation from crafted files.
            if (len > MAX_VEC_LEN) return std::unexpected(SerializerError::CorruptConstant);
            if (!count_fits(is, len, kMinConst)) return std::unexpected(SerializerError::Truncated);
            std::vector<LispVal> elems;
            elems.reserve(len);
            auto roots = heap_.make_external_root_frame();
            for (uint32_t i = 0; i < len; ++i) {
                auto elem = read_constant(is, depth + 1);
                if (!elem) return elem;
                elems.push_back(*elem);
                roots.push(*elem);
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

/**
 * Serialize
 */

bool BytecodeSerializer::serialize(
        const std::vector<ModuleEntry>& modules,
        const semantics::BytecodeFunctionRegistry& registry,
        uint64_t source_hash,
        bool include_debug,
        std::ostream& os,
        const std::vector<std::string>& imports,
        uint32_t num_builtins,
        const std::optional<PackageMetadata>& package_metadata,
        const std::vector<DependencyHashEntry>& dependency_hashes,
        const std::array<uint8_t, 16>* compiler_id,
        uint64_t extension_env_hash) const
{
    /// Header
    os.write(MAGIC, 4);
    write_u16(os, FORMAT_VERSION);

    uint16_t flags = 0;
    if (include_debug) flags |= FLAG_HAS_DEBUG;
    if (package_metadata.has_value()) flags |= FLAG_HAS_PACKAGE_META;
    if (!dependency_hashes.empty()) flags |= FLAG_HAS_DEPHASH;
    write_u16(os, flags);

    write_u64(os, source_hash);
    write_u32(os, num_builtins);
    write_u64(os, extension_env_hash);
    const auto effective_compiler_id =
        compiler_id != nullptr ? *compiler_id : default_compiler_id();
    os.write(reinterpret_cast<const char*>(effective_compiler_id.data()),
             static_cast<std::streamsize>(effective_compiler_id.size()));

    if (package_metadata.has_value()) {
        write_str(os, package_metadata->name);
        write_str(os, package_metadata->version);
        write_u64(os, package_metadata->manifest_hash);
    }

    if (!dependency_hashes.empty()) {
        write_u32(os, static_cast<uint32_t>(dependency_hashes.size()));
        for (const auto& dep : dependency_hashes) {
            write_str(os, dep.dependency);
            write_u64(os, dep.etac_hash);
        }
    }

    write_u32(os, static_cast<uint32_t>(modules.size()));
    write_u32(os, static_cast<uint32_t>(registry.size()));

    /// Imports table
    write_u32(os, static_cast<uint32_t>(imports.size()));
    for (const auto& imp : imports) {
        write_str(os, imp);
    }

    /// Module table
    for (const auto& mod : modules) {
        write_str(os, mod.name);
        write_u32(os, mod.init_func_index);
        write_u32(os, mod.total_globals);
        uint8_t has_main = mod.main_func_slot.has_value() ? 1 : 0;
        write_u8(os, has_main);
        write_u32(os, mod.main_func_slot.value_or(0xFFFFFFFFu));
        write_u32(os, mod.first_func_index);
        write_u32(os, mod.func_count);

        write_u32(os, static_cast<uint32_t>(mod.owned_global_slots.size()));
        for (const auto slot : mod.owned_global_slots) {
            write_u32(os, slot);
        }

        write_u32(os, static_cast<uint32_t>(mod.import_bindings.size()));
        for (const auto& imp : mod.import_bindings) {
            write_u32(os, imp.local_slot);
            write_str(os, imp.from_module);
            write_str(os, imp.remote_name);
        }

        write_u32(os, static_cast<uint32_t>(mod.export_bindings.size()));
        for (const auto& ex : mod.export_bindings) {
            write_str(os, ex.name);
            write_u32(os, ex.slot);
        }
    }

    /// Function table
    const auto& funcs = registry.all();
    for (const auto& func : funcs) {
        write_str(os, func.name);
        write_u32(os, func.arity);
        write_u8(os, func.has_rest ? 1 : 0);
        write_u32(os, func.stack_size);

        /// Constants
        write_u32(os, static_cast<uint32_t>(func.constants.size()));
        for (auto c : func.constants) {
            write_constant(os, c);
        }

        /// Instructions
        write_u32(os, static_cast<uint32_t>(func.code.size()));
        for (const auto& instr : func.code) {
            write_u8(os, static_cast<uint8_t>(instr.opcode));
            write_u32(os, instr.arg);
        }

        /// Source map (optional)
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

        /// Local names
        write_u32(os, static_cast<uint32_t>(func.local_names.size()));
        for (const auto& n : func.local_names) write_str(os, n);

        /// Upval names
        write_u32(os, static_cast<uint32_t>(func.upval_names.size()));
        for (const auto& n : func.upval_names) write_str(os, n);
    }

    return os.good();
}

/**
 * Deserialize
 */

std::expected<EtacFile, SerializerError>
BytecodeSerializer::deserialize(std::istream& is, uint32_t expected_builtins) const
{
    EtacFile result;

    /**
     * Decoded constants are held only in `result` until the caller registers
     * the functions, so keep each one rooted while later constants allocate.
     * Callers must root `result.registry` themselves once this returns.
     */
    auto constant_roots = heap_.make_external_root_frame();

    /// Header
    char magic[4];
    is.read(magic, 4);
    if (!is.good() || std::memcmp(magic, MAGIC, 4) != 0)
        return std::unexpected(SerializerError::BadMagic);

    uint16_t version;
    if (!read_u16(is, version)) return std::unexpected(SerializerError::Truncated);
    if (version != FORMAT_VERSION_V3
        && version != FORMAT_VERSION_V4
        && version != FORMAT_VERSION_V5
        && version != FORMAT_VERSION_V6
        && version != FORMAT_VERSION) {
        return std::unexpected(SerializerError::VersionMismatch);
    }
    result.format_version = version;

    uint16_t flags;
    if (!read_u16(is, flags)) return std::unexpected(SerializerError::Truncated);
    result.flags = flags;
    bool has_debug = (flags & FLAG_HAS_DEBUG) != 0;

    if (!read_u64(is, result.source_hash)) return std::unexpected(SerializerError::Truncated);

    uint32_t file_builtins;
    if (!read_u32(is, file_builtins)) return std::unexpected(SerializerError::Truncated);
    result.builtin_count = file_builtins;
    if (expected_builtins != 0 && file_builtins != expected_builtins)
        return std::unexpected(SerializerError::BuiltinCountMismatch);

    if (version >= FORMAT_VERSION_V6) {
        if (!read_u64(is, result.extension_env_hash)) {
            return std::unexpected(SerializerError::Truncated);
        }
        result.has_extension_env_hash = true;
    }

    if (version >= FORMAT_VERSION_V4) {
        if (!is.read(reinterpret_cast<char*>(result.compiler_id.data()),
                     static_cast<std::streamsize>(result.compiler_id.size()))
                 .good()) {
            return std::unexpected(SerializerError::Truncated);
        }
        result.has_compiler_id = true;

        if ((flags & FLAG_HAS_PACKAGE_META) != 0) {
            PackageMetadata meta;
            if (!read_str(is, meta.name)) return std::unexpected(SerializerError::Truncated);
            if (!read_str(is, meta.version)) return std::unexpected(SerializerError::Truncated);
            if (!read_u64(is, meta.manifest_hash)) return std::unexpected(SerializerError::Truncated);
            result.package_metadata = std::move(meta);
        }

        if ((flags & FLAG_HAS_DEPHASH) != 0) {
            uint32_t dep_count = 0;
            if (!read_u32(is, dep_count)) return std::unexpected(SerializerError::Truncated);
            if (dep_count > MAX_DEP_HASH_ENTRIES) {
                return std::unexpected(SerializerError::CorruptConstant);
            }
            if (!count_fits(is, dep_count, kMinDepHash)) return std::unexpected(SerializerError::Truncated);
            result.dependency_hashes.reserve(dep_count);
            for (uint32_t i = 0; i < dep_count; ++i) {
                DependencyHashEntry entry;
                if (!read_str(is, entry.dependency)) return std::unexpected(SerializerError::Truncated);
                if (!read_u64(is, entry.etac_hash)) return std::unexpected(SerializerError::Truncated);
                result.dependency_hashes.push_back(std::move(entry));
            }
        }
    }

    uint32_t num_modules, num_functions;
    if (!read_u32(is, num_modules)) return std::unexpected(SerializerError::Truncated);
    if (!read_u32(is, num_functions)) return std::unexpected(SerializerError::Truncated);

    /// Imports table
    uint32_t num_imports;
    if (!read_u32(is, num_imports)) return std::unexpected(SerializerError::Truncated);
    if (!count_fits(is, num_imports, kMinStr)) return std::unexpected(SerializerError::Truncated);
    result.imports.resize(num_imports);
    for (uint32_t i = 0; i < num_imports; ++i) {
        if (!read_str(is, result.imports[i])) return std::unexpected(SerializerError::Truncated);
    }

    /// Module table
    const uint64_t min_module = version >= FORMAT_VERSION_V5 ? kMinModuleV5 : kMinModuleLegacy;
    if (!count_fits(is, num_modules, min_module)) return std::unexpected(SerializerError::Truncated);
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

        if (mod.total_globals > MAX_GLOBALS
            || mod.init_func_index >= num_functions
            || (has_main && main_slot >= mod.total_globals)) {
            return std::unexpected(SerializerError::InvalidBytecode);
        }

        if (version >= FORMAT_VERSION_V5) {
            if (!read_u32(is, mod.first_func_index)) return std::unexpected(SerializerError::Truncated);
            if (!read_u32(is, mod.func_count)) return std::unexpected(SerializerError::Truncated);
            if (static_cast<uint64_t>(mod.first_func_index) + mod.func_count > num_functions) {
                return std::unexpected(SerializerError::InvalidBytecode);
            }

            uint32_t owned_count = 0;
            if (!read_u32(is, owned_count)) return std::unexpected(SerializerError::Truncated);
            if (!count_fits(is, owned_count, 4)) return std::unexpected(SerializerError::Truncated);
            mod.owned_global_slots.resize(owned_count);
            for (uint32_t i = 0; i < owned_count; ++i) {
                if (!read_u32(is, mod.owned_global_slots[i])) {
                    return std::unexpected(SerializerError::Truncated);
                }
                if (mod.owned_global_slots[i] >= mod.total_globals) {
                    return std::unexpected(SerializerError::InvalidBytecode);
                }
            }

            uint32_t import_binding_count = 0;
            if (!read_u32(is, import_binding_count)) return std::unexpected(SerializerError::Truncated);
            if (!count_fits(is, import_binding_count, kMinImportBind)) return std::unexpected(SerializerError::Truncated);
            mod.import_bindings.resize(import_binding_count);
            for (uint32_t i = 0; i < import_binding_count; ++i) {
                auto& imp = mod.import_bindings[i];
                if (!read_u32(is, imp.local_slot)) return std::unexpected(SerializerError::Truncated);
                if (!read_str(is, imp.from_module)) return std::unexpected(SerializerError::Truncated);
                if (!read_str(is, imp.remote_name)) return std::unexpected(SerializerError::Truncated);
                if (imp.local_slot >= mod.total_globals) return std::unexpected(SerializerError::InvalidBytecode);
            }

            uint32_t export_binding_count = 0;
            if (!read_u32(is, export_binding_count)) return std::unexpected(SerializerError::Truncated);
            if (!count_fits(is, export_binding_count, kMinExportBind)) return std::unexpected(SerializerError::Truncated);
            mod.export_bindings.resize(export_binding_count);
            for (uint32_t i = 0; i < export_binding_count; ++i) {
                auto& ex = mod.export_bindings[i];
                if (!read_str(is, ex.name)) return std::unexpected(SerializerError::Truncated);
                if (!read_u32(is, ex.slot)) return std::unexpected(SerializerError::Truncated);
                if (ex.slot >= mod.total_globals) return std::unexpected(SerializerError::InvalidBytecode);
            }
        }
    }

    if (version < FORMAT_VERSION_V5) {
        /**
         * Legacy artifacts do not store per-module function ranges.
         * Infer ranges from init-function indices, where each module init is
         * emitted last for that module and module order is preserved.
         */
        uint32_t start = 0;
        for (auto& mod : result.modules) {
            mod.first_func_index = start;
            if (mod.init_func_index >= start) {
                mod.func_count = (mod.init_func_index - start) + 1;
            } else {
                mod.func_count = 0;
            }
            start = mod.init_func_index + 1;
        }
    }

    /// Function table
    if (!count_fits(is, num_functions, kMinFunction)) return std::unexpected(SerializerError::Truncated);
    for (uint32_t f = 0; f < num_functions; ++f) {
        BytecodeFunction func;

        if (!read_str(is, func.name)) return std::unexpected(SerializerError::Truncated);
        if (!read_u32(is, func.arity)) return std::unexpected(SerializerError::Truncated);
        uint8_t has_rest;
        if (!read_u8(is, has_rest)) return std::unexpected(SerializerError::Truncated);
        func.has_rest = (has_rest != 0);
        if (!read_u32(is, func.stack_size)) return std::unexpected(SerializerError::Truncated);
        /// setup_frame sizes the VM stack from these, so bound them here.
        if (func.stack_size > MAX_FRAME_SLOTS || func.arity > MAX_FRAME_SLOTS) {
            return std::unexpected(SerializerError::InvalidBytecode);
        }

        /// Constants
        uint32_t num_consts;
        if (!read_u32(is, num_consts)) return std::unexpected(SerializerError::Truncated);
        if (!count_fits(is, num_consts, kMinConst)) return std::unexpected(SerializerError::Truncated);
        func.constants.reserve(num_consts);
        for (uint32_t i = 0; i < num_consts; ++i) {
            auto c = read_constant(is);
            if (!c) return std::unexpected(c.error());
            constant_roots.push(*c);
            func.constants.push_back(*c);
        }

        /// Instructions
        uint32_t num_instrs;
        if (!read_u32(is, num_instrs)) return std::unexpected(SerializerError::Truncated);
        const uint64_t min_instr = kMinInstr + (has_debug ? kMinSpan : 0);
        if (!count_fits(is, num_instrs, min_instr)) return std::unexpected(SerializerError::Truncated);
        func.code.reserve(num_instrs);
        for (uint32_t i = 0; i < num_instrs; ++i) {
            uint8_t op;
            uint32_t arg;
            if (!read_u8(is, op)) return std::unexpected(SerializerError::Truncated);
            if (!read_u32(is, arg)) return std::unexpected(SerializerError::Truncated);
            func.code.push_back({static_cast<OpCode>(op), arg});
        }

        /// Source map
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

        /// Local names
        uint32_t num_locals;
        if (!read_u32(is, num_locals)) return std::unexpected(SerializerError::Truncated);
        if (!count_fits(is, num_locals, kMinStr)) return std::unexpected(SerializerError::Truncated);
        func.local_names.resize(num_locals);
        for (uint32_t i = 0; i < num_locals; ++i) {
            if (!read_str(is, func.local_names[i])) return std::unexpected(SerializerError::Truncated);
        }

        /// Upval names
        uint32_t num_upvals;
        if (!read_u32(is, num_upvals)) return std::unexpected(SerializerError::Truncated);
        if (!count_fits(is, num_upvals, kMinStr)) return std::unexpected(SerializerError::Truncated);
        func.upval_names.resize(num_upvals);
        for (uint32_t i = 0; i < num_upvals; ++i) {
            if (!read_str(is, func.upval_names[i])) return std::unexpected(SerializerError::Truncated);
        }

        result.registry.add(std::move(func));
    }

    /// Lightweight structural verification of every deserialized function.
    uint32_t global_limit = 0;
    for (const auto& mod : result.modules) {
        global_limit = std::max(global_limit, mod.total_globals);
    }
    for (uint32_t f = 0; f < result.registry.size(); ++f) {
        const auto* fn = result.registry.get(f);
        if (!fn) continue;
        auto vr = verify_function(*fn, result.registry, global_limit);
        if (!vr) return std::unexpected(vr.error());
    }

    return result;
}

} ///< namespace eta::runtime::vm

/**
 * Bytecode verifier (lightweight structural check)
 */

namespace eta::runtime::vm {

std::expected<void, SerializerError>
BytecodeSerializer::verify_function(const BytecodeFunction& func,
                                    const semantics::BytecodeFunctionRegistry& registry,
                                    uint32_t global_limit) {
    const auto nconsts    = static_cast<uint32_t>(func.constants.size());
    const auto stack_size = func.stack_size;
    const auto nupvals    = static_cast<uint32_t>(func.upval_names.size());
    const auto ncode      = static_cast<int64_t>(func.code.size());

    /// A branch target must land on an instruction of this function.
    auto target_ok = [ncode](int64_t target) { return target >= 0 && target < ncode; };

    for (std::size_t pc = 0; pc < func.code.size(); ++pc) {
        const auto& instr = func.code[pc];
        const auto next_pc = static_cast<int64_t>(pc) + 1;

        /**
         * Reject completely unknown opcode bytes.
         *
         * WAM opcodes reserve a block at 0x80-0xBF, which means the enum
         * is intentionally non-contiguous.  String-dispatch via to_string() is
         * the canonical "known opcode" check.
         */
        if (std::strcmp(to_string(instr.opcode), "Unknown") == 0)
            return std::unexpected(SerializerError::InvalidBytecode);

        /// Constant-index instructions: arg is an index into constants[].
        if (instr.opcode == OpCode::LoadConst) {
            if (nconsts == 0 || instr.arg >= nconsts)
                return std::unexpected(SerializerError::InvalidBytecode);
        }
        /// Local-slot instructions: arg is a slot index in [0, stack_size).
        else if (instr.opcode == OpCode::LoadLocal ||
                 instr.opcode == OpCode::StoreLocal) {
            if (stack_size > 0 && instr.arg >= stack_size)
                return std::unexpected(SerializerError::InvalidBytecode);
        }
        /**
         * MakeClosure packs (const_idx << 16 | num_upvals) into arg.  The
         * constant must name a function in this file, and the closure must
         * capture exactly the upvalues that function's LoadUpval/StoreUpval
         * were verified against.
         */
        else if (instr.opcode == OpCode::MakeClosure) {
            const uint32_t const_idx  = instr.arg >> 16;
            const uint32_t num_upvals = instr.arg & 0xFFFFu;
            if (nconsts == 0 || const_idx >= nconsts)
                return std::unexpected(SerializerError::InvalidBytecode);
            const auto fval = func.constants[const_idx];
            if (!is_func_index(fval))
                return std::unexpected(SerializerError::InvalidBytecode);
            const auto* target = registry.get(decode_func_index(fval));
            if (!target || target->upval_names.size() != num_upvals)
                return std::unexpected(SerializerError::InvalidBytecode);
        }
        /// Upvalue instructions: arg indexes this closure's captured upvalues.
        else if (instr.opcode == OpCode::LoadUpval ||
                 instr.opcode == OpCode::StoreUpval) {
            if (instr.arg >= nupvals)
                return std::unexpected(SerializerError::InvalidBytecode);
        }
        /// Global instructions: arg is an artifact-layout global slot.
        else if (instr.opcode == OpCode::LoadGlobal ||
                 instr.opcode == OpCode::StoreGlobal) {
            if (instr.arg >= global_limit)
                return std::unexpected(SerializerError::InvalidBytecode);
        }
        /// Jump: arg is a signed offset from the next instruction.
        else if (instr.opcode == OpCode::Jump) {
            if (!target_ok(next_pc + static_cast<int32_t>(instr.arg)))
                return std::unexpected(SerializerError::InvalidBytecode);
        }
        /// JumpIfFalse: arg is an unsigned forward offset from the next instruction.
        else if (instr.opcode == OpCode::JumpIfFalse) {
            if (!target_ok(next_pc + static_cast<int64_t>(instr.arg)))
                return std::unexpected(SerializerError::InvalidBytecode);
        }
        /// SetupCatch packs (tag_const_idx << 16 | handler_offset) into arg.
        else if (instr.opcode == OpCode::SetupCatch) {
            if ((instr.arg >> 16) >= nconsts ||
                !target_ok(next_pc + static_cast<int64_t>(instr.arg & 0xFFFFu)))
                return std::unexpected(SerializerError::InvalidBytecode);
        }
        /**
         * Call/Values counts are stack operands; nothing can need more than
         * a whole frame's worth.  Apply also needs the list operand.
         */
        else if (instr.opcode == OpCode::Call ||
                 instr.opcode == OpCode::TailCall ||
                 instr.opcode == OpCode::Values) {
            if (instr.arg > MAX_FRAME_SLOTS)
                return std::unexpected(SerializerError::InvalidBytecode);
        }
        else if (instr.opcode == OpCode::Apply ||
                 instr.opcode == OpCode::TailApply) {
            if (instr.arg == 0 || instr.arg > MAX_FRAME_SLOTS)
                return std::unexpected(SerializerError::InvalidBytecode);
        }
        else if (instr.opcode == OpCode::NumCmp) {
            if (instr.arg > numeric_ops::kMaxCmpOp)
                return std::unexpected(SerializerError::InvalidBytecode);
        }
        /// All other opcodes: no index-based args requiring static validation.
    }

    return {};
}

} ///< namespace eta::runtime::vm

