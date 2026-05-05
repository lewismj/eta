#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

#include "eta/runtime/vm/bytecode.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/semantics/emitter.h"

namespace eta::runtime::vm {

/// Error type

enum class SerializerError : std::uint8_t {
    BadMagic,
    VersionMismatch,
    Truncated,
    CorruptConstant,
    HashMismatch,
    IOError,
    BuiltinCountMismatch,
    InvalidBytecode,    ///< post-deserialization structure check failed
};

constexpr const char* to_string(SerializerError e) noexcept {
    using enum SerializerError;
    switch (e) {
        case BadMagic:              return "bad magic bytes (expected ETAC)";
        case VersionMismatch:       return "unsupported .etac version";
        case Truncated:             return "truncated .etac file";
        case CorruptConstant:       return "corrupt constant in .etac file";
        case HashMismatch:          return "source hash mismatch";
        case IOError:               return "I/O error";
        case BuiltinCountMismatch:  return ".etac was compiled with a different builtin set "
                                           "(e.g. torch-enabled vs non-torch); please recompile with etac";
        case InvalidBytecode:       return "invalid bytecode (opcode or index out of range)";
    }
    return "unknown serializer error";
}

/// Per-module metadata stored in .etac

struct ModuleEntry {
    std::string name;
    std::uint32_t init_func_index{0};          ///< index into the function table
    std::uint32_t total_globals{0};
    std::optional<std::uint32_t> main_func_slot; ///< global slot of main, if any
    std::uint32_t first_func_index{0};         ///< first function index owned by this module
    std::uint32_t func_count{0};               ///< number of functions owned by this module
    std::vector<std::uint32_t> owned_global_slots; ///< global slots defined by this module

    struct ImportBinding {
        std::uint32_t local_slot{0};          ///< local/import binding slot in this module
        std::string from_module;              ///< provider module
        std::string remote_name;              ///< provider export name
    };

    struct ExportBinding {
        std::string name;                     ///< exported symbol name
        std::uint32_t slot{0};                ///< defining global slot in artifact layout
    };

    std::vector<ImportBinding> import_bindings; ///< import-binding relocation metadata
    std::vector<ExportBinding> export_bindings; ///< export-slot relocation metadata
};

/**
 * @brief Package metadata embedded in a `.etac` header.
 */
struct PackageMetadata {
    std::string name;
    std::string version;
    std::uint64_t manifest_hash{0};
};

/**
 * @brief Dependency hash entry embedded in a `.etac` header.
 */
struct DependencyHashEntry {
    std::string dependency;
    std::uint64_t etac_hash{0};
};

/// Result of deserialization

struct EtacFile {
    std::uint16_t format_version{0};
    std::uint16_t flags{0};
    std::uint64_t source_hash{0};
    std::uint32_t builtin_count{0};
    bool has_compiler_id{false};
    std::array<std::uint8_t, 16> compiler_id{};
    std::optional<PackageMetadata> package_metadata;
    std::vector<DependencyHashEntry> dependency_hashes;
    std::vector<std::string> imports;   ///< Non-prelude module dependencies
    std::vector<ModuleEntry> modules;
    semantics::BytecodeFunctionRegistry registry;
};

/**
 * @brief Freshness policy input for stale-artifact checks.
 */
struct FreshnessContext {
    std::optional<std::array<std::uint8_t, 16>> expected_compiler_id;
    std::optional<std::uint32_t> expected_builtin_count;
    std::optional<std::uint64_t> expected_source_hash;
    std::optional<std::uint64_t> expected_manifest_hash;
    std::vector<DependencyHashEntry> expected_dependency_hashes;
};

/**
 * @brief Freshness status for `.etac` artifacts.
 */
enum class FreshnessStatus : std::uint8_t {
    Fresh,
    UnsupportedFormat,
    MissingCompilerId,
    CompilerIdMismatch,
    BuiltinCountMismatch,
    SourceHashMismatch,
    ManifestHashMismatch,
    DependencyHashMismatch,
};

constexpr const char* to_string(FreshnessStatus status) noexcept {
    switch (status) {
        case FreshnessStatus::Fresh:              return "artifact is fresh";
        case FreshnessStatus::UnsupportedFormat:  return "unsupported bytecode format";
        case FreshnessStatus::MissingCompilerId:  return "artifact has no compiler fingerprint";
        case FreshnessStatus::CompilerIdMismatch: return "compiler fingerprint mismatch";
        case FreshnessStatus::BuiltinCountMismatch: return "builtin count mismatch";
        case FreshnessStatus::SourceHashMismatch: return "source hash mismatch";
        case FreshnessStatus::ManifestHashMismatch: return "manifest hash mismatch";
        case FreshnessStatus::DependencyHashMismatch: return "dependency hash mismatch";
    }
    return "unknown freshness status";
}

/**
 * @brief Result of stale-artifact checking.
 */
struct FreshnessResult {
    FreshnessStatus status{FreshnessStatus::Fresh};
    std::string detail;

    [[nodiscard]] bool fresh() const noexcept {
        return status == FreshnessStatus::Fresh;
    }
};

/// Serializer / Deserializer

class BytecodeSerializer {
public:
    BytecodeSerializer(memory::heap::Heap& heap,
                       memory::intern::InternTable& intern_table)
        : heap_(heap), intern_table_(intern_table) {}

    /**
     * Serialize a compiled file (set of modules + functions) to a binary stream.
     *
     * @param modules       Per-module metadata (name, init func index, etc.)
     * @param registry      All BytecodeFunctions produced from the source file.
     * @param source_hash   Hash of the original .eta source (for cache invalidation).
     * @param include_debug If true, source_map spans are included.
     * @param os            Output stream (binary mode).
     * @param imports       Non-prelude module dependencies to store in the .etac.
     * @param num_builtins  Number of builtin slots registered at compile time.
     * @param package_metadata Optional package metadata for stale-artifact checks.
     * @param dependency_hashes Optional dep-hash table for stale-artifact checks.
     * @param compiler_id   Optional compiler fingerprint (defaults to current build id).
     * @return true on success.
     */
    bool serialize(const std::vector<ModuleEntry>& modules,
                   const semantics::BytecodeFunctionRegistry& registry,
                   std::uint64_t source_hash,
                   bool include_debug,
                   std::ostream& os,
                   const std::vector<std::string>& imports = {},
                   std::uint32_t num_builtins = 0,
                   const std::optional<PackageMetadata>& package_metadata = std::nullopt,
                   const std::vector<DependencyHashEntry>& dependency_hashes = {},
                   const std::array<std::uint8_t, 16>* compiler_id = nullptr) const;

    /**
     * Deserialize a .etac binary into an EtacFile.
     * @param is             Input stream (binary mode).
     * @param expected_builtins  Number of builtin slots in the current runtime.
     *                           When non-zero, a mismatch triggers BuiltinCountMismatch.
     */
    std::expected<EtacFile, SerializerError>
    deserialize(std::istream& is, std::uint32_t expected_builtins = 0) const;

    /// Compute a source hash using boost::hash.
    static std::uint64_t hash_source(std::string_view source);

    /**
     * @brief Compute the compiler fingerprint for the current build.
     *
     * The value is deterministic for a given toolchain/build configuration and
     * is embedded in modern artifacts to detect stale `.etac` files.
     */
    static std::array<std::uint8_t, 16> default_compiler_id();

    /**
     * @brief Evaluate whether a deserialized artifact is fresh.
     */
    static FreshnessResult check_freshness(const EtacFile& file,
                                           const FreshnessContext& context);

    /// Format constants
    static constexpr char     MAGIC[4]       = {'E','T','A','C'};
    static constexpr uint16_t FORMAT_VERSION_V3 = 3;
    static constexpr uint16_t FORMAT_VERSION_V4 = 4;
    static constexpr uint16_t FORMAT_VERSION = 5;   ///< v5 module relocation metadata
    static constexpr uint16_t FLAG_HAS_DEBUG = 0x0001;
    static constexpr uint16_t FLAG_HAS_PACKAGE_META = 0x0002;
    static constexpr uint16_t FLAG_HAS_DEPHASH = 0x0004;

    /// Sanity limits applied during deserialization (prevent DoS from crafted files)
    static constexpr uint32_t MAX_STRING_LEN = 64u * 1024u;  ///< 64 KiB per string/symbol
    static constexpr uint32_t MAX_VEC_LEN    = 65535u;       ///< max inline constant-vector len
    static constexpr uint32_t MAX_DEP_HASH_ENTRIES = 65535u; ///< max dep-hash table entries

private:
    memory::heap::Heap& heap_;
    memory::intern::InternTable& intern_table_;

    /// Low-level binary I/O helpers
    static void write_u8 (std::ostream& os, std::uint8_t  v);
    static void write_u16(std::ostream& os, std::uint16_t v);
    static void write_u32(std::ostream& os, std::uint32_t v);
    static void write_u64(std::ostream& os, std::uint64_t v);
    static void write_i64(std::ostream& os, std::int64_t  v);
    static void write_f64(std::ostream& os, double v);
    static void write_str(std::ostream& os, std::string_view s);

    static bool read_u8 (std::istream& is, std::uint8_t&  v);
    static bool read_u16(std::istream& is, std::uint16_t& v);
    static bool read_u32(std::istream& is, std::uint32_t& v);
    static bool read_u64(std::istream& is, std::uint64_t& v);
    static bool read_i64(std::istream& is, std::int64_t&  v);
    static bool read_f64(std::istream& is, double& v);
    static bool read_str(std::istream& is, std::string& s);

    /**
     * Constant encoding
     * Tag byte preceding each constant in the pool.
     */
    enum ConstTag : std::uint8_t {
        CT_Nil       = 0,
        CT_True      = 1,
        CT_Fixnum    = 2,
        CT_Double    = 3,
        CT_Char      = 4,
        CT_String    = 5,
        CT_Symbol    = 6,
        CT_FuncIndex = 7,
        CT_HeapCons  = 8,   ///< recursive: car + cdr
        CT_HeapVec   = 9,   ///< recursive: len + elements
        CT_RawBits   = 10,  ///< fallback: raw u64
        CT_HeapNil   = 11,  ///< quoted empty list as heap object (if distinct from Nil)
        CT_False     = 12,
    };

    void write_constant(std::ostream& os, nanbox::LispVal v) const;
    std::expected<nanbox::LispVal, SerializerError> read_constant(std::istream& is) const;

    void write_heap_value(std::ostream& os, nanbox::LispVal v) const;

    /**
     * Lightweight structural verifier: checks that opcode bytes and index args
     * are in bounds.  Called after deserializing each BytecodeFunction.
     */
    static std::expected<void, SerializerError> verify_function(const BytecodeFunction& func);
};

} ///< namespace eta::runtime::vm

