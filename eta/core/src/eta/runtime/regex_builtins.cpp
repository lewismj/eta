#include "eta/runtime/regex_builtins.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <limits>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "eta/runtime/error.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/string_view.h"
#include "eta/runtime/types/types.h"

namespace eta::runtime {
using namespace eta::runtime::nanbox;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::memory::factory;
using namespace eta::runtime::error;

namespace {
using Args = const std::vector<LispVal>&;

inline constexpr const char* kRegexErrorTag = "runtime.regex-error";

struct FlagParseResult {
    std::regex::flag_type flags{std::regex_constants::ECMAScript};
    std::vector<std::string> flag_names{"ecmascript"};
};

std::unexpected<RuntimeError> type_error(std::string message) {
    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, std::move(message)}});
}

std::unexpected<RuntimeError> regex_compile_error(std::string who,
                                                  const std::string& pattern,
                                                  std::string detail) {
    VMError err{
        RuntimeErrorCode::UserError,
        std::move(who) + ": invalid regex pattern: " + detail + " (pattern: \"" + pattern + "\")"
    };
    err.tag_override = kRegexErrorTag;
    err.fields.push_back(VMErrorField{"pattern", pattern});
    err.fields.push_back(VMErrorField{"detail", std::move(detail)});
    return std::unexpected(RuntimeError{std::move(err)});
}

std::expected<const types::Regex*, RuntimeError> require_regex(
    Heap& heap,
    LispVal value,
    const char* who) {
    if (!ops::is_boxed(value) || ops::tag(value) != Tag::HeapObject) {
        return type_error(std::string(who) + ": first argument must be a regex");
    }
    auto* regex_obj = heap.try_get_as<ObjectKind::Regex, types::Regex>(ops::payload(value));
    if (!regex_obj) {
        return type_error(std::string(who) + ": first argument must be a regex");
    }
    if (!regex_obj->compiled) {
        return std::unexpected(RuntimeError{
            VMError{RuntimeErrorCode::InternalError, std::string(who) + ": regex object has no compiled engine"}});
    }
    return regex_obj;
}

std::expected<std::string, RuntimeError> require_string(
    InternTable& intern_table,
    LispVal value,
    const char* who,
    const char* position_label) {
    auto sv = StringView::try_from(value, intern_table);
    if (!sv) {
        return type_error(std::string(who) + ": " + position_label + " must be a string");
    }
    return std::string(sv->view());
}

std::expected<std::string, RuntimeError> require_symbol_name(
    InternTable& intern_table,
    LispVal value,
    const char* who) {
    if (!ops::is_boxed(value) || ops::tag(value) != Tag::Symbol) {
        return type_error(std::string(who) + ": expected symbol in flags list");
    }
    auto sym = intern_table.get_string(ops::payload(value));
    if (!sym) {
        return std::unexpected(RuntimeError{
            VMError{RuntimeErrorCode::InternalError, std::string(who) + ": unresolved symbol id"}});
    }
    return std::string(*sym);
}

std::expected<void, RuntimeError> for_each_list(
    Heap& heap,
    LispVal list,
    const char* who,
    const std::function<std::expected<void, RuntimeError>(LispVal)>& fn) {
    LispVal cur = list;
    while (cur != Nil) {
        if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject) {
            return type_error(std::string(who) + ": expected a proper list");
        }
        auto* cell = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
        if (!cell) {
            return type_error(std::string(who) + ": expected a proper list");
        }
        auto step = fn(cell->car);
        if (!step) return std::unexpected(step.error());
        cur = cell->cdr;
    }
    return {};
}

std::expected<FlagParseResult, RuntimeError> parse_flag_list(
    Heap& heap,
    InternTable& intern_table,
    LispVal flags_list,
    const char* who) {
    FlagParseResult out;
    bool has_icase = false;
    bool has_multiline = false;
    bool has_nosubs = false;
    bool has_optimize = false;
    bool has_collate = false;

    std::string grammar = "ecmascript";
    auto apply_grammar = [&](std::regex::flag_type f, std::string g) {
        out.flags &= ~(std::regex_constants::ECMAScript
                     | std::regex_constants::basic
                     | std::regex_constants::extended
                     | std::regex_constants::awk
                     | std::regex_constants::grep
                     | std::regex_constants::egrep);
        out.flags |= f;
        grammar = std::move(g);
    };

    auto walk = for_each_list(heap, flags_list, who, [&](LispVal item) -> std::expected<void, RuntimeError> {
        auto sym_res = require_symbol_name(intern_table, item, who);
        if (!sym_res) return std::unexpected(sym_res.error());
        std::string name = *sym_res;
        if (!name.empty() && name.front() == ':') {
            name.erase(name.begin());
        }

        if (name == "icase") {
            if (!has_icase) {
                out.flags |= std::regex_constants::icase;
                has_icase = true;
            }
            return {};
        }
        if (name == "multiline") {
#if defined(_MSC_VER)
            return type_error(std::string(who) + ": 'multiline is not supported by this C++ standard library");
#else
            if (!has_multiline) {
                out.flags |= std::regex_constants::multiline;
                has_multiline = true;
            }
            return {};
#endif
        }
        if (name == "nosubs") {
            if (!has_nosubs) {
                out.flags |= std::regex_constants::nosubs;
                has_nosubs = true;
            }
            return {};
        }
        if (name == "optimize") {
            if (!has_optimize) {
                out.flags |= std::regex_constants::optimize;
                has_optimize = true;
            }
            return {};
        }
        if (name == "collate") {
            if (!has_collate) {
                out.flags |= std::regex_constants::collate;
                has_collate = true;
            }
            return {};
        }

        if (name == "ecmascript") { apply_grammar(std::regex_constants::ECMAScript, "ecmascript"); return {}; }
        if (name == "basic") { apply_grammar(std::regex_constants::basic, "basic"); return {}; }
        if (name == "extended") { apply_grammar(std::regex_constants::extended, "extended"); return {}; }
        if (name == "awk") { apply_grammar(std::regex_constants::awk, "awk"); return {}; }
        if (name == "grep") { apply_grammar(std::regex_constants::grep, "grep"); return {}; }
        if (name == "egrep") { apply_grammar(std::regex_constants::egrep, "egrep"); return {}; }

        return type_error(std::string(who) + ": unknown regex flag '" + name + "'");
    });
    if (!walk) return std::unexpected(walk.error());

    out.flag_names.clear();
    out.flag_names.push_back(grammar);
    if (has_icase) out.flag_names.emplace_back("icase");
    if (has_multiline) out.flag_names.emplace_back("multiline");
    if (has_nosubs) out.flag_names.emplace_back("nosubs");
    if (has_optimize) out.flag_names.emplace_back("optimize");
    if (has_collate) out.flag_names.emplace_back("collate");

    return out;
}

std::vector<std::pair<std::string, std::size_t>> parse_named_group_indices(
    const std::string& pattern) {
    std::vector<std::pair<std::string, std::size_t>> named;
    std::unordered_set<std::string> seen;
    std::size_t capture_index = 0;
    bool escaped = false;
    bool in_char_class = false;

    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char ch = pattern[i];

        if (escaped) {
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (in_char_class) {
            if (ch == ']') in_char_class = false;
            continue;
        }
        if (ch == '[') {
            in_char_class = true;
            continue;
        }
        if (ch != '(') continue;

        if (i + 1 < pattern.size() && pattern[i + 1] == '?') {
            if (i + 2 >= pattern.size()) continue;
            const char marker = pattern[i + 2];
            if (marker == ':' || marker == '=' || marker == '!') {
                continue;  // non-capturing / lookahead
            }
            if (marker == '<') {
                if (i + 3 < pattern.size() && (pattern[i + 3] == '=' || pattern[i + 3] == '!')) {
                    continue;  // lookbehind
                }
                const std::size_t name_start = i + 3;
                const std::size_t close = pattern.find('>', name_start);
                if (close == std::string::npos || close == name_start) {
                    continue;
                }
                ++capture_index;
                std::string name = pattern.substr(name_start, close - name_start);
                if (seen.insert(name).second) {
                    named.emplace_back(std::move(name), capture_index);
                }
                i = close;
                continue;
            }
            continue;
        }

        ++capture_index;
    }

    return named;
}

std::string rewrite_named_capture_groups(const std::string& pattern) {
    std::string out;
    out.reserve(pattern.size());

    bool escaped = false;
    bool in_char_class = false;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char ch = pattern[i];

        if (escaped) {
            out.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            out.push_back(ch);
            escaped = true;
            continue;
        }
        if (in_char_class) {
            out.push_back(ch);
            if (ch == ']') in_char_class = false;
            continue;
        }
        if (ch == '[') {
            out.push_back(ch);
            in_char_class = true;
            continue;
        }

        if (ch == '('
            && i + 2 < pattern.size()
            && pattern[i + 1] == '?'
            && pattern[i + 2] == '<') {
            if (i + 3 < pattern.size() && (pattern[i + 3] == '=' || pattern[i + 3] == '!')) {
                out.push_back(ch);  // lookbehind, keep unchanged
                continue;
            }
            const std::size_t close = pattern.find('>', i + 3);
            if (close != std::string::npos && close > i + 3) {
                out.push_back('(');
                i = close;
                continue;
            }
        }

        out.push_back(ch);
    }

    return out;
}

std::optional<std::size_t> named_group_index(const types::Regex& regex_obj, std::string_view name) {
    for (const auto& [candidate, index] : regex_obj.named_group_indices) {
        if (candidate == name) return index;
    }
    return std::nullopt;
}

std::string rewrite_named_replacement(const std::string& replacement, const types::Regex& regex_obj) {
    if (regex_obj.named_group_indices.empty()) return replacement;

    std::string out;
    out.reserve(replacement.size());
    for (std::size_t i = 0; i < replacement.size(); ++i) {
        if (replacement[i] == '$' && i + 2 < replacement.size() && replacement[i + 1] == '<') {
            const std::size_t close = replacement.find('>', i + 2);
            if (close != std::string::npos) {
                const std::string_view name{
                    replacement.data() + i + 2,
                    close - (i + 2)
                };
                if (auto idx = named_group_index(regex_obj, name)) {
                    out.push_back('$');
                    out += std::to_string(*idx);
                    i = close;
                    continue;
                }
            }
        }
        out.push_back(replacement[i]);
    }
    return out;
}

std::expected<std::shared_ptr<const std::regex>, RuntimeError> compile_regex(
    const std::string& pattern,
    std::regex::flag_type flags,
    const char* who) {
    try {
        return std::make_shared<const std::regex>(pattern, flags);
    } catch (const std::regex_error& e) {
        return regex_compile_error(who, pattern, e.what());
    }
}

struct RegexCacheKey {
    std::string pattern;
    std::regex::flag_type flags{};

    bool operator==(const RegexCacheKey& other) const noexcept {
        return flags == other.flags && pattern == other.pattern;
    }
};

struct RegexCacheKeyHash {
    std::size_t operator()(const RegexCacheKey& key) const noexcept {
        std::size_t h1 = std::hash<std::string>{}(key.pattern);
        std::size_t h2 = static_cast<std::size_t>(key.flags);
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

struct RegexCacheStats {
    std::uint64_t hits{};
    std::uint64_t misses{};
    std::uint64_t compiles{};
    std::uint64_t evictions{};
    std::size_t entries{};
};

struct RegexCacheResult {
    std::string pattern;
    std::regex::flag_type flags{};
    std::vector<std::string> flag_names;
    std::vector<std::pair<std::string, std::size_t>> named_group_indices;
    std::shared_ptr<const std::regex> compiled;
};

class RegexCache {
public:
    static constexpr std::size_t kCapacity = 128;

    std::expected<RegexCacheResult, RuntimeError> get_or_compile(
        const std::string& pattern,
        std::regex::flag_type flags,
        std::vector<std::string> flag_names,
        const char* who) {
        std::lock_guard lock(mu_);
        RegexCacheKey key{pattern, flags};
        if (auto hit = entries_.find(key); hit != entries_.end()) {
            ++hits_;
            touch_(hit->second);
            return make_result_(hit->first, hit->second);
        }

        ++misses_;
        auto named_groups = parse_named_group_indices(pattern);
        const std::string engine_pattern = rewrite_named_capture_groups(pattern);
        auto compiled = compile_regex(engine_pattern, flags, who);
        if (!compiled) return std::unexpected(compiled.error());
        ++compiles_;

        lru_.push_front(key);
        RegexCacheEntry entry{
            .compiled = std::move(*compiled),
            .flag_names = std::move(flag_names),
            .named_group_indices = std::move(named_groups),
            .lru_it = lru_.begin()
        };
        auto [it, inserted] = entries_.emplace(std::move(key), std::move(entry));
        if (!inserted) {
            lru_.pop_front();
            ++hits_;
            touch_(it->second);
            return make_result_(it->first, it->second);
        }

        if (entries_.size() > kCapacity) {
            const auto& stale = lru_.back();
            entries_.erase(stale);
            lru_.pop_back();
            ++evictions_;
        }

        return make_result_(it->first, it->second);
    }

    RegexCacheStats stats() const {
        std::lock_guard lock(mu_);
        return RegexCacheStats{
            .hits = hits_,
            .misses = misses_,
            .compiles = compiles_,
            .evictions = evictions_,
            .entries = entries_.size()
        };
    }

    void reset() {
        std::lock_guard lock(mu_);
        entries_.clear();
        lru_.clear();
        hits_ = 0;
        misses_ = 0;
        compiles_ = 0;
        evictions_ = 0;
    }

private:
    struct RegexCacheEntry {
        std::shared_ptr<const std::regex> compiled;
        std::vector<std::string> flag_names;
        std::vector<std::pair<std::string, std::size_t>> named_group_indices;
        std::list<RegexCacheKey>::iterator lru_it;
    };

    RegexCacheResult make_result_(const RegexCacheKey& key, const RegexCacheEntry& entry) const {
        return RegexCacheResult{
            .pattern = key.pattern,
            .flags = key.flags,
            .flag_names = entry.flag_names,
            .named_group_indices = entry.named_group_indices,
            .compiled = entry.compiled
        };
    }

    void touch_(RegexCacheEntry& entry) {
        lru_.splice(lru_.begin(), lru_, entry.lru_it);
        entry.lru_it = lru_.begin();
    }

    mutable std::mutex mu_;
    std::unordered_map<RegexCacheKey, RegexCacheEntry, RegexCacheKeyHash> entries_;
    std::list<RegexCacheKey> lru_;
    std::uint64_t hits_{0};
    std::uint64_t misses_{0};
    std::uint64_t compiles_{0};
    std::uint64_t evictions_{0};
};

RegexCache g_regex_cache;

std::expected<LispVal, RuntimeError> index_to_lisp(Heap& heap, std::size_t v, const char* who) {
    if (v > static_cast<std::size_t>(std::numeric_limits<int64_t>::max())) {
        return std::unexpected(RuntimeError{
            VMError{RuntimeErrorCode::TypeError, std::string(who) + ": index overflow"}});
    }
    const auto narrowed = static_cast<int64_t>(v);
    auto enc = ops::encode<int64_t>(narrowed);
    if (enc) return *enc;
    return make_fixnum(heap, narrowed);
}

std::expected<std::size_t, RuntimeError> decode_non_negative_index(
    Heap& heap,
    LispVal value,
    const char* who,
    const char* label) {
    auto n = classify_numeric(value, heap);
    if (!n.is_valid() || n.is_flonum() || n.int_val < 0) {
        return type_error(std::string(who) + ": " + label + " must be a non-negative integer");
    }
    return static_cast<std::size_t>(n.int_val);
}

std::expected<LispVal, RuntimeError> vector_to_list(Heap& heap, const std::vector<LispVal>& items) {
    LispVal out = Nil;
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        auto cell = make_cons(heap, *it, out);
        if (!cell) return std::unexpected(cell.error());
        out = *cell;
    }
    return out;
}

std::expected<LispVal, RuntimeError> make_span(Heap& heap, std::size_t start, std::size_t end, const char* who) {
    auto start_val = index_to_lisp(heap, start, who);
    if (!start_val) return std::unexpected(start_val.error());
    auto end_val = index_to_lisp(heap, end, who);
    if (!end_val) return std::unexpected(end_val.error());
    return make_cons(heap, *start_val, *end_val);
}

std::expected<LispVal, RuntimeError> make_match_value(
    Heap& heap,
    InternTable& intern_table,
    const types::Regex& regex_obj,
    const std::match_results<std::string::const_iterator>& match,
    std::size_t absolute_base,
    LispVal input_value,
    const char* who) {
    const std::size_t start = absolute_base + static_cast<std::size_t>(match.position(0));
    const std::size_t end = start + static_cast<std::size_t>(match.length(0));

    auto start_val = index_to_lisp(heap, start, who);
    if (!start_val) return std::unexpected(start_val.error());
    auto end_val = index_to_lisp(heap, end, who);
    if (!end_val) return std::unexpected(end_val.error());

    std::vector<LispVal> groups;
    groups.reserve(match.size());
    for (std::size_t i = 0; i < match.size(); ++i) {
        if (!match[i].matched) {
            groups.push_back(False);
            continue;
        }
        const std::size_t g_start = absolute_base + static_cast<std::size_t>(match.position(i));
        const std::size_t g_end = g_start + static_cast<std::size_t>(match.length(i));
        auto span = make_span(heap, g_start, g_end, who);
        if (!span) return std::unexpected(span.error());
        groups.push_back(*span);
    }
    auto groups_vec = make_vector(heap, std::move(groups));
    if (!groups_vec) return std::unexpected(groups_vec.error());

    std::vector<LispVal> named_rows;
    named_rows.reserve(regex_obj.named_group_indices.size());
    for (const auto& [name, index] : regex_obj.named_group_indices) {
        auto sym = make_symbol(intern_table, name);
        if (!sym) return std::unexpected(sym.error());

        LispVal span_or_false = False;
        if (index < match.size() && match[index].matched) {
            const std::size_t g_start = absolute_base + static_cast<std::size_t>(match.position(index));
            const std::size_t g_end = g_start + static_cast<std::size_t>(match.length(index));
            auto span = make_span(heap, g_start, g_end, who);
            if (!span) return std::unexpected(span.error());
            span_or_false = *span;
        }

        auto row = make_cons(heap, *sym, span_or_false);
        if (!row) return std::unexpected(row.error());
        named_rows.push_back(*row);
    }
    auto named_list = vector_to_list(heap, named_rows);
    if (!named_list) return std::unexpected(named_list.error());

    std::vector<LispVal> payload{
        *start_val, *end_val, *groups_vec, *named_list, input_value
    };
    return make_vector(heap, std::move(payload));
}

std::expected<std::string, RuntimeError> call_replace_callback(
    vm::VM* vm,
    Heap& heap,
    InternTable& intern_table,
    LispVal fn,
    LispVal match_val,
    const char* who) {
    std::expected<LispVal, RuntimeError> callback_result;
    if (vm) {
        callback_result = vm->call_value(fn, {match_val});
    } else {
        if (!ops::is_boxed(fn) || ops::tag(fn) != Tag::HeapObject) {
            return type_error(std::string(who) + ": callback must be a procedure");
        }
        auto* prim = heap.try_get_as<ObjectKind::Primitive, types::Primitive>(ops::payload(fn));
        if (!prim) {
            return type_error(std::string(who) + ": callback closures require a VM context");
        }
        callback_result = prim->func({match_val});
    }
    if (!callback_result) return std::unexpected(callback_result.error());

    auto replacement = StringView::try_from(*callback_result, intern_table);
    if (!replacement) {
        return type_error(std::string(who) + ": callback must return a string");
    }
    return std::string(replacement->view());
}

std::expected<LispVal, RuntimeError> flag_names_to_symbol_list(
    Heap& heap,
    InternTable& intern_table,
    const std::vector<std::string>& names) {
    std::vector<LispVal> items;
    items.reserve(names.size());
    for (const auto& name : names) {
        auto sym = make_symbol(intern_table, name);
        if (!sym) return std::unexpected(sym.error());
        items.push_back(*sym);
    }
    return vector_to_list(heap, items);
}

bool is_regex_value(Heap& heap, LispVal value) {
    if (!ops::is_boxed(value) || ops::tag(value) != Tag::HeapObject) return false;
    return heap.try_get_as<ObjectKind::Regex, types::Regex>(ops::payload(value)) != nullptr;
}

std::expected<types::Regex, RuntimeError> cache_default_regex(
    const std::string& pattern,
    const char* who) {
    std::vector<std::string> flag_names{"ecmascript"};
    auto cached = g_regex_cache.get_or_compile(
        pattern,
        std::regex_constants::ECMAScript,
        std::move(flag_names),
        who);
    if (!cached) return std::unexpected(cached.error());
    return types::Regex{
        .pattern = std::move(cached->pattern),
        .flags = cached->flags,
        .flag_names = std::move(cached->flag_names),
        .named_group_indices = std::move(cached->named_group_indices),
        .compiled = std::move(cached->compiled)
    };
}

std::expected<LispVal, RuntimeError> cache_stat_to_lisp(Heap& heap, std::uint64_t value, const char* who) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<int64_t>::max())) {
        return type_error(std::string(who) + ": cache statistic overflow");
    }
    const auto narrowed = static_cast<int64_t>(value);
    auto enc = ops::encode<int64_t>(narrowed);
    if (enc) return *enc;
    return make_fixnum(heap, narrowed);
}

std::expected<LispVal, RuntimeError> cache_entry_count_to_lisp(
    Heap& heap,
    std::size_t entries,
    const char* who) {
    if (entries > static_cast<std::size_t>(std::numeric_limits<int64_t>::max())) {
        return type_error(std::string(who) + ": cache entry count overflow");
    }
    const auto narrowed = static_cast<int64_t>(entries);
    auto enc = ops::encode<int64_t>(narrowed);
    if (enc) return *enc;
    return make_fixnum(heap, narrowed);
}

std::expected<LispVal, RuntimeError> make_cache_stats_vector(Heap& heap, const char* who) {
    const auto stats = g_regex_cache.stats();
    auto hits = cache_stat_to_lisp(heap, stats.hits, who);
    if (!hits) return std::unexpected(hits.error());
    auto misses = cache_stat_to_lisp(heap, stats.misses, who);
    if (!misses) return std::unexpected(misses.error());
    auto compiles = cache_stat_to_lisp(heap, stats.compiles, who);
    if (!compiles) return std::unexpected(compiles.error());
    auto evictions = cache_stat_to_lisp(heap, stats.evictions, who);
    if (!evictions) return std::unexpected(evictions.error());
    auto entries = cache_entry_count_to_lisp(heap, stats.entries, who);
    if (!entries) return std::unexpected(entries.error());

    std::vector<LispVal> payload{
        *hits, *misses, *compiles, *evictions, *entries
    };
    return make_vector(heap, std::move(payload));
}

} // namespace

void register_regex_builtins(BuiltinEnvironment& env,
                             Heap& heap,
                             InternTable& intern_table,
                             vm::VM* vm) {
    env.register_builtin("%regex-compile", 2, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto pattern = require_string(intern_table, args[0], "%regex-compile", "pattern");
            if (!pattern) return std::unexpected(pattern.error());

            auto flags = parse_flag_list(heap, intern_table, args[1], "%regex-compile");
            if (!flags) return std::unexpected(flags.error());

            auto named_groups = parse_named_group_indices(*pattern);
            const std::string engine_pattern = rewrite_named_capture_groups(*pattern);
            auto compiled = compile_regex(engine_pattern, flags->flags, "%regex-compile");
            if (!compiled) return std::unexpected(compiled.error());

            return make_regex(
                heap,
                *pattern,
                flags->flags,
                std::move(flags->flag_names),
                std::move(named_groups),
                std::move(*compiled));
        });

    env.register_builtin("%regex?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        return is_regex_value(heap, args[0]) ? True : False;
    });

    env.register_builtin("%regex-pattern", 1, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto regex_obj = require_regex(heap, args[0], "%regex-pattern");
            if (!regex_obj) return std::unexpected(regex_obj.error());
            return make_string(heap, intern_table, (*regex_obj)->pattern);
        });

    env.register_builtin("%regex-flags", 1, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto regex_obj = require_regex(heap, args[0], "%regex-flags");
            if (!regex_obj) return std::unexpected(regex_obj.error());
            return flag_names_to_symbol_list(heap, intern_table, (*regex_obj)->flag_names);
        });

    env.register_builtin("%regex-match?", 2, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto regex_obj = require_regex(heap, args[0], "%regex-match?");
            if (!regex_obj) return std::unexpected(regex_obj.error());
            auto input = require_string(intern_table, args[1], "%regex-match?", "second argument");
            if (!input) return std::unexpected(input.error());
            const bool matched = std::regex_match(*input, *(*regex_obj)->compiled);
            return matched ? True : False;
        });

    env.register_builtin("%regex-search", 3, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto regex_obj = require_regex(heap, args[0], "%regex-search");
            if (!regex_obj) return std::unexpected(regex_obj.error());
            auto input = require_string(intern_table, args[1], "%regex-search", "second argument");
            if (!input) return std::unexpected(input.error());
            auto start = decode_non_negative_index(heap, args[2], "%regex-search", "start");
            if (!start) return std::unexpected(start.error());
            if (*start > input->size()) return False;

            std::match_results<std::string::const_iterator> match;
            const auto begin = input->cbegin() + static_cast<std::ptrdiff_t>(*start);
            const bool found = std::regex_search(begin, input->cend(), match, *(*regex_obj)->compiled);
            if (!found) return False;
            return make_match_value(heap, intern_table, *(*regex_obj), match, *start, args[1], "%regex-search");
        });

    env.register_builtin("%regex-find-all", 2, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto regex_obj = require_regex(heap, args[0], "%regex-find-all");
            if (!regex_obj) return std::unexpected(regex_obj.error());
            auto input = require_string(intern_table, args[1], "%regex-find-all", "second argument");
            if (!input) return std::unexpected(input.error());

            std::vector<LispVal> matches;
            auto search_begin = input->cbegin();
            while (search_begin <= input->cend()) {
                std::match_results<std::string::const_iterator> match;
                if (!std::regex_search(search_begin, input->cend(), match, *(*regex_obj)->compiled)) {
                    break;
                }
                const std::size_t base =
                    static_cast<std::size_t>(std::distance(input->cbegin(), search_begin));
                auto match_value =
                    make_match_value(heap, intern_table, *(*regex_obj), match, base, args[1], "%regex-find-all");
                if (!match_value) return std::unexpected(match_value.error());
                matches.push_back(*match_value);

                auto match_begin = search_begin + match.position(0);
                auto match_end = match_begin + static_cast<std::ptrdiff_t>(match.length(0));
                if (match_begin == input->cend() && match.length(0) == 0) break;
                search_begin = (match.length(0) == 0) ? (match_begin + 1) : match_end;
            }

            return vector_to_list(heap, matches);
        });

    env.register_builtin("%regex-replace", 3, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto regex_obj = require_regex(heap, args[0], "%regex-replace");
            if (!regex_obj) return std::unexpected(regex_obj.error());
            auto input = require_string(intern_table, args[1], "%regex-replace", "second argument");
            if (!input) return std::unexpected(input.error());
            auto replacement = require_string(intern_table, args[2], "%regex-replace", "third argument");
            if (!replacement) return std::unexpected(replacement.error());

            try {
                const std::string rewritten =
                    rewrite_named_replacement(*replacement, *(*regex_obj));
                const std::string replaced =
                    std::regex_replace(*input, *(*regex_obj)->compiled, rewritten);
                return make_string(heap, intern_table, replaced);
            } catch (const std::regex_error& e) {
                return regex_compile_error("%regex-replace", (*regex_obj)->pattern, e.what());
            }
        });

    env.register_builtin("%regex-replace-fn", 3, false,
        [&heap, &intern_table, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            auto regex_obj = require_regex(heap, args[0], "%regex-replace-fn");
            if (!regex_obj) return std::unexpected(regex_obj.error());
            auto input = require_string(intern_table, args[1], "%regex-replace-fn", "second argument");
            if (!input) return std::unexpected(input.error());

            LispVal callback = args[2];
            if (!ops::is_boxed(callback) || ops::tag(callback) != Tag::HeapObject) {
                return type_error("%regex-replace-fn: callback must be a procedure");
            }
            if (!heap.try_get_as<ObjectKind::Primitive, types::Primitive>(ops::payload(callback))
                && !heap.try_get_as<ObjectKind::Closure, types::Closure>(ops::payload(callback))) {
                return type_error("%regex-replace-fn: callback must be a procedure");
            }

            std::string output;
            std::size_t cursor = 0;
            for (std::sregex_iterator it(input->begin(), input->end(), *(*regex_obj)->compiled), end;
                 it != end; ++it) {
                const auto& match = *it;
                const std::size_t pos = static_cast<std::size_t>(match.position(0));
                const std::size_t len = static_cast<std::size_t>(match.length(0));

                output.append(*input, cursor, pos - cursor);

                auto match_value = make_match_value(
                    heap, intern_table, *(*regex_obj), match, 0, args[1], "%regex-replace-fn");
                if (!match_value) return std::unexpected(match_value.error());

                auto replacement = call_replace_callback(
                    vm, heap, intern_table, callback, *match_value, "%regex-replace-fn");
                if (!replacement) return std::unexpected(replacement.error());
                output += *replacement;

                cursor = pos + len;
            }
            output.append(*input, cursor, std::string::npos);
            return make_string(heap, intern_table, output);
        });

    env.register_builtin("%regex-split", 2, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto regex_obj = require_regex(heap, args[0], "%regex-split");
            if (!regex_obj) return std::unexpected(regex_obj.error());
            auto input = require_string(intern_table, args[1], "%regex-split", "second argument");
            if (!input) return std::unexpected(input.error());

            std::vector<LispVal> parts;
            std::size_t cursor = 0;
            auto search_begin = input->cbegin();
            while (search_begin <= input->cend()) {
                std::match_results<std::string::const_iterator> match;
                if (!std::regex_search(search_begin, input->cend(), match, *(*regex_obj)->compiled)) {
                    break;
                }

                const std::size_t base =
                    static_cast<std::size_t>(std::distance(input->cbegin(), search_begin));
                const std::size_t pos = base + static_cast<std::size_t>(match.position(0));
                const std::size_t len = static_cast<std::size_t>(match.length(0));

                auto piece = make_string(heap, intern_table, input->substr(cursor, pos - cursor));
                if (!piece) return std::unexpected(piece.error());
                parts.push_back(*piece);

                if (len == 0) {
                    if (pos >= input->size()) {
                        cursor = pos;
                        break;
                    }
                    cursor = pos;
                    search_begin = input->cbegin() + static_cast<std::ptrdiff_t>(pos + 1);
                    continue;
                }

                cursor = pos + len;
                search_begin = input->cbegin() + static_cast<std::ptrdiff_t>(cursor);
            }

            auto tail = make_string(heap, intern_table, input->substr(cursor));
            if (!tail) return std::unexpected(tail.error());
            parts.push_back(*tail);

            return make_vector(heap, std::move(parts));
        });

    env.register_builtin("%regex-quote", 1, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto input = require_string(intern_table, args[0], "%regex-quote", "argument");
            if (!input) return std::unexpected(input.error());
            static constexpr std::string_view metachars = R"(\.^$|()[]{}*+?)";

            std::string escaped;
            escaped.reserve(input->size() * 2);
            for (char ch : *input) {
                if (metachars.find(ch) != std::string_view::npos) {
                    escaped.push_back('\\');
                }
                escaped.push_back(ch);
            }
            return make_string(heap, intern_table, escaped);
        });

    env.register_builtin("%regex-match?-str", 2, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto pattern = require_string(intern_table, args[0], "%regex-match?-str", "first argument");
            if (!pattern) return std::unexpected(pattern.error());
            auto regex_obj = cache_default_regex(*pattern, "%regex-match?-str");
            if (!regex_obj) return std::unexpected(regex_obj.error());
            auto input = require_string(intern_table, args[1], "%regex-match?-str", "second argument");
            if (!input) return std::unexpected(input.error());
            const bool matched = std::regex_match(*input, *regex_obj->compiled);
            return matched ? True : False;
        });

    env.register_builtin("%regex-search-str", 3, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto pattern = require_string(intern_table, args[0], "%regex-search-str", "first argument");
            if (!pattern) return std::unexpected(pattern.error());
            auto regex_obj = cache_default_regex(*pattern, "%regex-search-str");
            if (!regex_obj) return std::unexpected(regex_obj.error());
            auto input = require_string(intern_table, args[1], "%regex-search-str", "second argument");
            if (!input) return std::unexpected(input.error());
            auto start = decode_non_negative_index(heap, args[2], "%regex-search-str", "start");
            if (!start) return std::unexpected(start.error());
            if (*start > input->size()) return False;

            std::match_results<std::string::const_iterator> match;
            const auto begin = input->cbegin() + static_cast<std::ptrdiff_t>(*start);
            const bool found = std::regex_search(begin, input->cend(), match, *regex_obj->compiled);
            if (!found) return False;
            return make_match_value(heap, intern_table, *regex_obj, match, *start, args[1], "%regex-search-str");
        });

    env.register_builtin("%regex-find-all-str", 2, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto pattern = require_string(intern_table, args[0], "%regex-find-all-str", "first argument");
            if (!pattern) return std::unexpected(pattern.error());
            auto regex_obj = cache_default_regex(*pattern, "%regex-find-all-str");
            if (!regex_obj) return std::unexpected(regex_obj.error());
            auto input = require_string(intern_table, args[1], "%regex-find-all-str", "second argument");
            if (!input) return std::unexpected(input.error());

            std::vector<LispVal> matches;
            auto search_begin = input->cbegin();
            while (search_begin <= input->cend()) {
                std::match_results<std::string::const_iterator> match;
                if (!std::regex_search(search_begin, input->cend(), match, *regex_obj->compiled)) {
                    break;
                }
                const std::size_t base =
                    static_cast<std::size_t>(std::distance(input->cbegin(), search_begin));
                auto match_value =
                    make_match_value(heap, intern_table, *regex_obj, match, base, args[1], "%regex-find-all-str");
                if (!match_value) return std::unexpected(match_value.error());
                matches.push_back(*match_value);

                auto match_begin = search_begin + match.position(0);
                auto match_end = match_begin + static_cast<std::ptrdiff_t>(match.length(0));
                if (match_begin == input->cend() && match.length(0) == 0) break;
                search_begin = (match.length(0) == 0) ? (match_begin + 1) : match_end;
            }

            return vector_to_list(heap, matches);
        });

    env.register_builtin("%regex-replace-str", 3, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto pattern = require_string(intern_table, args[0], "%regex-replace-str", "first argument");
            if (!pattern) return std::unexpected(pattern.error());
            auto regex_obj = cache_default_regex(*pattern, "%regex-replace-str");
            if (!regex_obj) return std::unexpected(regex_obj.error());
            auto input = require_string(intern_table, args[1], "%regex-replace-str", "second argument");
            if (!input) return std::unexpected(input.error());
            auto replacement = require_string(intern_table, args[2], "%regex-replace-str", "third argument");
            if (!replacement) return std::unexpected(replacement.error());

            try {
                const std::string rewritten = rewrite_named_replacement(*replacement, *regex_obj);
                const std::string replaced = std::regex_replace(*input, *regex_obj->compiled, rewritten);
                return make_string(heap, intern_table, replaced);
            } catch (const std::regex_error& e) {
                return regex_compile_error("%regex-replace-str", regex_obj->pattern, e.what());
            }
        });

    env.register_builtin("%regex-split-str", 2, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto pattern = require_string(intern_table, args[0], "%regex-split-str", "first argument");
            if (!pattern) return std::unexpected(pattern.error());
            auto regex_obj = cache_default_regex(*pattern, "%regex-split-str");
            if (!regex_obj) return std::unexpected(regex_obj.error());
            auto input = require_string(intern_table, args[1], "%regex-split-str", "second argument");
            if (!input) return std::unexpected(input.error());

            std::vector<LispVal> parts;
            std::size_t cursor = 0;
            auto search_begin = input->cbegin();
            while (search_begin <= input->cend()) {
                std::match_results<std::string::const_iterator> match;
                if (!std::regex_search(search_begin, input->cend(), match, *regex_obj->compiled)) {
                    break;
                }

                const std::size_t base =
                    static_cast<std::size_t>(std::distance(input->cbegin(), search_begin));
                const std::size_t pos = base + static_cast<std::size_t>(match.position(0));
                const std::size_t len = static_cast<std::size_t>(match.length(0));

                auto piece = make_string(heap, intern_table, input->substr(cursor, pos - cursor));
                if (!piece) return std::unexpected(piece.error());
                parts.push_back(*piece);

                if (len == 0) {
                    if (pos >= input->size()) {
                        cursor = pos;
                        break;
                    }
                    cursor = pos;
                    search_begin = input->cbegin() + static_cast<std::ptrdiff_t>(pos + 1);
                    continue;
                }

                cursor = pos + len;
                search_begin = input->cbegin() + static_cast<std::ptrdiff_t>(cursor);
            }

            auto tail = make_string(heap, intern_table, input->substr(cursor));
            if (!tail) return std::unexpected(tail.error());
            parts.push_back(*tail);

            return make_vector(heap, std::move(parts));
        });

    env.register_builtin("%regex-cache-stats", 0, false,
        [&heap](Args /*args*/) -> std::expected<LispVal, RuntimeError> {
            return make_cache_stats_vector(heap, "%regex-cache-stats");
        });

    env.register_builtin("%regex-cache-reset!", 0, false,
        [](Args /*args*/) -> std::expected<LispVal, RuntimeError> {
            g_regex_cache.reset();
            return True;
        });
}

} // namespace eta::runtime
