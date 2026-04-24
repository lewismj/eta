#include "eta/runtime/csv_builtins.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <csv.hpp>

#include "eta/runtime/error.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/string_view.h"
#include "eta/runtime/types/types.h"
#include "eta/runtime/value_formatter.h"

namespace eta::runtime {
using namespace eta::runtime::nanbox;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::memory::factory;
using namespace eta::runtime::error;

namespace {
using Args = const std::vector<LispVal>&;

enum class CsvQuotePolicy : std::uint8_t {
    Minimal,
    All,
    NonNumeric,
    None,
};

struct CsvCell {
    std::string text;
    bool numeric{false};
};

struct CsvReaderBackend {
    std::shared_ptr<std::stringstream> source;
    std::unique_ptr<csv::CSVReader> reader;
};

struct CsvWriterBackend {
    std::unique_ptr<std::ofstream> stream;
    char delimiter{','};
    char quote{'"'};
    CsvQuotePolicy quote_policy{CsvQuotePolicy::Minimal};
};

struct CsvReaderOptions {
    char delimiter{','};
    char quote{'"'};
    bool header{true};
    int header_row{0};
    bool trim{true};
    std::optional<char> comment;
    std::optional<std::vector<std::string>> column_names;
    std::vector<std::string> null_tokens{"", "NA", "NaN"};
};

struct CsvWriterOptions {
    char delimiter{','};
    char quote{'"'};
    CsvQuotePolicy quote_policy{CsvQuotePolicy::Minimal};
    std::optional<std::vector<std::string>> column_names;
    bool header{true};
};

struct FactTableLoadOptions {
    CsvReaderOptions reader;
    bool infer_types{true};
};

enum class InferredColumnType : std::uint8_t {
    Int,
    Float,
    String,
};

std::unexpected<RuntimeError> type_error(std::string message) {
    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, std::move(message)}});
}

std::unexpected<RuntimeError> open_error(const char* who, std::string detail) {
    return std::unexpected(RuntimeError{
        VMError{RuntimeErrorCode::TypeError, std::string(who) + ": " + std::move(detail)}});
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

std::expected<std::string, RuntimeError> require_symbol_or_string(
    InternTable& intern_table,
    LispVal value,
    const char* who,
    const char* label) {
    if (ops::is_boxed(value) && ops::tag(value) == Tag::Symbol) {
        auto text = intern_table.get_string(ops::payload(value));
        if (!text) {
            return std::unexpected(RuntimeError{
                VMError{RuntimeErrorCode::InternalError, std::string(who) + ": unresolved symbol id"}});
        }
        return std::string(*text);
    }

    auto sv = StringView::try_from(value, intern_table);
    if (!sv) {
        return type_error(std::string(who) + ": " + label + " must be a symbol or string");
    }
    return std::string(sv->view());
}

std::expected<char, RuntimeError> require_char(LispVal value, const char* who, const char* label) {
    if (!ops::is_boxed(value) || ops::tag(value) != Tag::Char) {
        return type_error(std::string(who) + ": " + label + " must be a character");
    }
    auto decoded = ops::decode<char32_t>(value);
    if (!decoded || *decoded < 0 || *decoded > 0x7f) {
        return type_error(std::string(who) + ": " + label + " must be an ASCII character");
    }
    return static_cast<char>(*decoded);
}

std::expected<bool, RuntimeError> require_bool(LispVal value, const char* who, const char* label) {
    if (value == True) return true;
    if (value == False) return false;
    return type_error(std::string(who) + ": " + label + " must be #t or #f");
}

std::expected<int64_t, RuntimeError> require_non_negative_fixnum(
    Heap& heap,
    LispVal value,
    const char* who,
    const char* label) {
    auto n = classify_numeric(value, heap);
    if (!n.is_valid() || n.is_flonum() || n.int_val < 0) {
        return type_error(std::string(who) + ": " + label + " must be a non-negative fixnum");
    }
    return n.int_val;
}

std::string normalize_option_key(std::string key) {
    while (!key.empty() && key.front() == ':') {
        key.erase(key.begin());
    }
    return key;
}

std::expected<std::vector<std::string>, RuntimeError> parse_string_list(
    Heap& heap,
    InternTable& intern_table,
    LispVal list,
    const char* who,
    const char* label) {
    std::vector<std::string> out;
    LispVal cur = list;
    while (cur != Nil) {
        if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject) {
            return type_error(std::string(who) + ": " + label + " must be a proper list");
        }
        auto* cell = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
        if (!cell) {
            return type_error(std::string(who) + ": " + label + " must be a proper list");
        }

        auto item = require_symbol_or_string(intern_table, cell->car, who, label);
        if (!item) return std::unexpected(item.error());
        out.push_back(std::move(*item));
        cur = cell->cdr;
    }

    return out;
}

std::expected<CsvQuotePolicy, RuntimeError> parse_quote_policy(
    InternTable& intern_table,
    LispVal value,
    const char* who) {
    auto text = require_symbol_or_string(intern_table, value, who, "quote-policy");
    if (!text) return std::unexpected(text.error());
    auto key = normalize_option_key(std::move(*text));

    if (key == "minimal") return CsvQuotePolicy::Minimal;
    if (key == "all") return CsvQuotePolicy::All;
    if (key == "non-numeric") return CsvQuotePolicy::NonNumeric;
    if (key == "none") return CsvQuotePolicy::None;
    return type_error(std::string(who) + ": quote-policy must be one of minimal, all, non-numeric, none");
}

std::expected<CsvReaderOptions, RuntimeError> parse_reader_options(
    Heap& heap,
    InternTable& intern_table,
    LispVal options,
    const char* who) {
    CsvReaderOptions parsed;
    if (options == Nil) return parsed;

    LispVal cur = options;
    while (cur != Nil) {
        if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject) {
            return type_error(std::string(who) + ": options must be an alist");
        }
        auto* cell = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
        if (!cell) {
            return type_error(std::string(who) + ": options must be an alist");
        }

        LispVal pair = cell->car;
        if (!ops::is_boxed(pair) || ops::tag(pair) != Tag::HeapObject) {
            return type_error(std::string(who) + ": options must be an alist");
        }
        auto* kv = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(pair));
        if (!kv) {
            return type_error(std::string(who) + ": options must be an alist");
        }

        auto key_text = require_symbol_or_string(intern_table, kv->car, who, "option key");
        if (!key_text) return std::unexpected(key_text.error());
        const std::string key = normalize_option_key(*key_text);
        const LispVal value = kv->cdr;

        if (key == "delimiter") {
            auto delim = require_char(value, who, "delimiter");
            if (!delim) return std::unexpected(delim.error());
            parsed.delimiter = *delim;
        } else if (key == "quote") {
            auto quote = require_char(value, who, "quote");
            if (!quote) return std::unexpected(quote.error());
            parsed.quote = *quote;
        } else if (key == "header") {
            auto header = require_bool(value, who, "header");
            if (!header) return std::unexpected(header.error());
            parsed.header = *header;
        } else if (key == "header-row") {
            auto row = require_non_negative_fixnum(heap, value, who, "header-row");
            if (!row) return std::unexpected(row.error());
            parsed.header_row = static_cast<int>(*row);
        } else if (key == "trim") {
            auto trim = require_bool(value, who, "trim");
            if (!trim) return std::unexpected(trim.error());
            parsed.trim = *trim;
        } else if (key == "comment") {
            if (value == False) {
                parsed.comment.reset();
            } else {
                auto comment = require_char(value, who, "comment");
                if (!comment) return std::unexpected(comment.error());
                parsed.comment = *comment;
            }
        } else if (key == "column-names") {
            if (value == False) {
                parsed.column_names.reset();
            } else {
                auto names = parse_string_list(heap, intern_table, value, who, "column-names");
                if (!names) return std::unexpected(names.error());
                parsed.column_names = std::move(*names);
            }
        } else if (key == "null-tokens") {
            if (value == False) {
                parsed.null_tokens.clear();
            } else {
                auto tokens = parse_string_list(heap, intern_table, value, who, "null-tokens");
                if (!tokens) return std::unexpected(tokens.error());
                parsed.null_tokens = std::move(*tokens);
            }
        } else {
            return type_error(std::string(who) + ": unknown option key '" + key + "'");
        }

        cur = cell->cdr;
    }

    return parsed;
}

std::expected<CsvWriterOptions, RuntimeError> parse_writer_options(
    Heap& heap,
    InternTable& intern_table,
    LispVal options,
    const char* who) {
    CsvWriterOptions parsed;
    if (options == Nil) return parsed;

    LispVal cur = options;
    while (cur != Nil) {
        if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject) {
            return type_error(std::string(who) + ": options must be an alist");
        }
        auto* cell = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
        if (!cell) {
            return type_error(std::string(who) + ": options must be an alist");
        }

        LispVal pair = cell->car;
        if (!ops::is_boxed(pair) || ops::tag(pair) != Tag::HeapObject) {
            return type_error(std::string(who) + ": options must be an alist");
        }
        auto* kv = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(pair));
        if (!kv) {
            return type_error(std::string(who) + ": options must be an alist");
        }

        auto key_text = require_symbol_or_string(intern_table, kv->car, who, "option key");
        if (!key_text) return std::unexpected(key_text.error());
        const std::string key = normalize_option_key(*key_text);
        const LispVal value = kv->cdr;

        if (key == "delimiter") {
            auto delim = require_char(value, who, "delimiter");
            if (!delim) return std::unexpected(delim.error());
            parsed.delimiter = *delim;
        } else if (key == "quote") {
            auto quote = require_char(value, who, "quote");
            if (!quote) return std::unexpected(quote.error());
            parsed.quote = *quote;
        } else if (key == "quote-policy") {
            auto policy = parse_quote_policy(intern_table, value, who);
            if (!policy) return std::unexpected(policy.error());
            parsed.quote_policy = *policy;
        } else if (key == "column-names") {
            if (value == False) {
                parsed.column_names.reset();
            } else {
                auto names = parse_string_list(heap, intern_table, value, who, "column-names");
                if (!names) return std::unexpected(names.error());
                parsed.column_names = std::move(*names);
            }
        } else if (key == "header") {
            auto header = require_bool(value, who, "header");
            if (!header) return std::unexpected(header.error());
            parsed.header = *header;
        } else {
            return type_error(std::string(who) + ": unknown option key '" + key + "'");
        }

        cur = cell->cdr;
    }

    return parsed;
}

std::expected<FactTableLoadOptions, RuntimeError> parse_fact_table_load_options(
    Heap& heap,
    InternTable& intern_table,
    LispVal options,
    const char* who) {
    FactTableLoadOptions parsed;
    if (options == Nil) return parsed;

    LispVal cur = options;
    while (cur != Nil) {
        if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject) {
            return type_error(std::string(who) + ": options must be an alist");
        }
        auto* cell = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
        if (!cell) {
            return type_error(std::string(who) + ": options must be an alist");
        }

        LispVal pair = cell->car;
        if (!ops::is_boxed(pair) || ops::tag(pair) != Tag::HeapObject) {
            return type_error(std::string(who) + ": options must be an alist");
        }
        auto* kv = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(pair));
        if (!kv) {
            return type_error(std::string(who) + ": options must be an alist");
        }

        auto key_text = require_symbol_or_string(intern_table, kv->car, who, "option key");
        if (!key_text) return std::unexpected(key_text.error());
        const std::string key = normalize_option_key(*key_text);
        const LispVal value = kv->cdr;

        if (key == "delimiter") {
            auto delim = require_char(value, who, "delimiter");
            if (!delim) return std::unexpected(delim.error());
            parsed.reader.delimiter = *delim;
        } else if (key == "quote") {
            auto quote = require_char(value, who, "quote");
            if (!quote) return std::unexpected(quote.error());
            parsed.reader.quote = *quote;
        } else if (key == "header") {
            auto header = require_bool(value, who, "header");
            if (!header) return std::unexpected(header.error());
            parsed.reader.header = *header;
        } else if (key == "header-row") {
            auto row = require_non_negative_fixnum(heap, value, who, "header-row");
            if (!row) return std::unexpected(row.error());
            parsed.reader.header_row = static_cast<int>(*row);
        } else if (key == "trim") {
            auto trim = require_bool(value, who, "trim");
            if (!trim) return std::unexpected(trim.error());
            parsed.reader.trim = *trim;
        } else if (key == "comment") {
            if (value == False) {
                parsed.reader.comment.reset();
            } else {
                auto comment = require_char(value, who, "comment");
                if (!comment) return std::unexpected(comment.error());
                parsed.reader.comment = *comment;
            }
        } else if (key == "column-names") {
            if (value == False) {
                parsed.reader.column_names.reset();
            } else {
                auto names = parse_string_list(heap, intern_table, value, who, "column-names");
                if (!names) return std::unexpected(names.error());
                parsed.reader.column_names = std::move(*names);
            }
        } else if (key == "null-tokens") {
            if (value == False) {
                parsed.reader.null_tokens.clear();
            } else {
                auto tokens = parse_string_list(heap, intern_table, value, who, "null-tokens");
                if (!tokens) return std::unexpected(tokens.error());
                parsed.reader.null_tokens = std::move(*tokens);
            }
        } else if (key == "infer-types" || key == "infer-types?") {
            auto infer = require_bool(value, who, "infer-types?");
            if (!infer) return std::unexpected(infer.error());
            parsed.infer_types = *infer;
        } else {
            return type_error(std::string(who) + ": unknown option key '" + key + "'");
        }

        cur = cell->cdr;
    }

    return parsed;
}

csv::CSVFormat make_reader_format(const CsvReaderOptions& options) {
    csv::CSVFormat format;
    format.delimiter(options.delimiter)
          .quote(options.quote)
          .variable_columns(csv::VariableColumnPolicy::KEEP);

    if (options.trim) {
        format.trim({' ', '\t'});
    } else {
        format.trim({});
    }

    if (options.column_names.has_value()) {
        format.column_names(*options.column_names);
    } else if (options.header) {
        format.header_row(options.header_row);
    } else {
        format.no_header();
    }

    return format;
}

std::expected<std::vector<LispVal>, RuntimeError> make_column_symbols(
    InternTable& intern_table,
    const std::vector<std::string>& columns,
    const char* who) {
    std::vector<LispVal> out;
    out.reserve(columns.size());
    for (const auto& col : columns) {
        auto sym = make_symbol(intern_table, col);
        if (!sym) {
            return std::unexpected(RuntimeError{
                VMError{RuntimeErrorCode::TypeError, std::string(who) + ": failed to intern column name"}});
        }
        out.push_back(*sym);
    }
    return out;
}

std::expected<types::CsvReader*, RuntimeError> require_csv_reader(
    Heap& heap,
    LispVal value,
    const char* who) {
    if (!ops::is_boxed(value) || ops::tag(value) != Tag::HeapObject) {
        return type_error(std::string(who) + ": first argument must be a csv-reader");
    }
    auto* reader = heap.try_get_as<ObjectKind::CsvReader, types::CsvReader>(ops::payload(value));
    if (!reader) {
        return type_error(std::string(who) + ": first argument must be a csv-reader");
    }
    return reader;
}

std::expected<types::CsvWriter*, RuntimeError> require_csv_writer(
    Heap& heap,
    LispVal value,
    const char* who) {
    if (!ops::is_boxed(value) || ops::tag(value) != Tag::HeapObject) {
        return type_error(std::string(who) + ": first argument must be a csv-writer");
    }
    auto* writer = heap.try_get_as<ObjectKind::CsvWriter, types::CsvWriter>(ops::payload(value));
    if (!writer) {
        return type_error(std::string(who) + ": first argument must be a csv-writer");
    }
    return writer;
}

std::expected<types::FactTable*, RuntimeError> require_fact_table(
    Heap& heap,
    LispVal value,
    const char* who) {
    if (!ops::is_boxed(value) || ops::tag(value) != Tag::HeapObject) {
        return type_error(std::string(who) + ": first argument must be a fact-table");
    }
    auto* table = heap.try_get_as<ObjectKind::FactTable, types::FactTable>(ops::payload(value));
    if (!table) {
        return type_error(std::string(who) + ": first argument must be a fact-table");
    }
    return table;
}

std::expected<std::shared_ptr<CsvReaderBackend>, RuntimeError> require_reader_backend(
    types::CsvReader& reader,
    const char* who) {
    if (!reader.state) {
        return type_error(std::string(who) + ": reader is closed");
    }

    auto state = std::static_pointer_cast<CsvReaderBackend>(reader.state);
    if (!state || !state->reader) {
        return type_error(std::string(who) + ": reader is closed");
    }

    return state;
}

std::expected<std::shared_ptr<CsvWriterBackend>, RuntimeError> require_writer_backend(
    types::CsvWriter& writer,
    const char* who) {
    if (!writer.state) {
        return type_error(std::string(who) + ": writer is closed");
    }

    auto state = std::static_pointer_cast<CsvWriterBackend>(writer.state);
    if (!state || !state->stream || !state->stream->is_open()) {
        return type_error(std::string(who) + ": writer is closed");
    }

    return state;
}

void close_reader(types::CsvReader& reader) {
    reader.state.reset();
    reader.columns.clear();
    reader.column_symbols.clear();
    reader.row_index = 0;
    reader.comment.reset();
    reader.null_tokens.clear();
}

void close_writer(types::CsvWriter& writer) {
    if (writer.state) {
        auto state = std::static_pointer_cast<CsvWriterBackend>(writer.state);
        if (state && state->stream) {
            state->stream->flush();
            state->stream->close();
        }
    }
    writer.state.reset();
    writer.row_index = 0;
}

bool read_next_row(csv::CSVReader& reader, std::optional<char> comment, csv::CSVRow& row) {
    while (reader.read_row(row)) {
        if (!comment.has_value()) return true;
        if (row.size() == 0) return true;

        auto first = row[0].get<csv::string_view>();
        if (!first.empty() && first.front() == *comment) {
            continue;
        }

        return true;
    }

    return false;
}

bool read_next_row(types::CsvReader& handle, CsvReaderBackend& backend, csv::CSVRow& row) {
    return read_next_row(*backend.reader, handle.comment, row);
}

std::expected<std::vector<LispVal>, RuntimeError> row_to_string_values(
    Heap& heap,
    InternTable& intern_table,
    const csv::CSVRow& row,
    const char* who) {
    std::vector<LispVal> values;
    values.reserve(row.size());
    for (std::size_t i = 0; i < row.size(); ++i) {
        auto text = row[i].get<csv::string_view>();
        auto str = make_string(heap, intern_table, std::string(text));
        if (!str) {
            return std::unexpected(RuntimeError{
                VMError{RuntimeErrorCode::TypeError, std::string(who) + ": failed to allocate string"}});
        }
        values.push_back(*str);
    }
    return values;
}

std::expected<LispVal, RuntimeError> vector_to_alist(
    Heap& heap,
    const std::vector<LispVal>& keys,
    const std::vector<LispVal>& values) {
    LispVal out = Nil;
    auto roots = heap.make_external_root_frame();
    roots.push(out);

    for (std::size_t i = keys.size(); i > 0; --i) {
        auto kv = make_cons(heap, keys[i - 1], values[i - 1]);
        if (!kv) return std::unexpected(kv.error());
        roots.push(*kv);

        auto row = make_cons(heap, *kv, out);
        if (!row) return std::unexpected(row.error());
        out = *row;
        roots.push(out);
    }
    return out;
}

std::expected<LispVal, RuntimeError> parse_typed_value(
    Heap& heap,
    InternTable& intern_table,
    const types::CsvReader& reader,
    csv::string_view raw,
    const char* who) {
    const auto is_null_token = [&reader](csv::string_view value) {
        for (const auto& token : reader.null_tokens) {
            if (value == token) return true;
        }
        return false;
    };

    const auto parse_int64 = [](csv::string_view value, int64_t& out) {
        const auto* begin = value.data();
        const auto* end = begin + value.size();
        auto [ptr, ec] = std::from_chars(begin, end, out, 10);
        return ec == std::errc{} && ptr == end;
    };

    const auto parse_double = [](csv::string_view value, double& out) {
        const auto* begin = value.data();
        const auto* end = begin + value.size();
        auto [ptr, ec] = std::from_chars(begin, end, out, std::chars_format::general);
        return ec == std::errc{} && ptr == end;
    };

    if (is_null_token(raw)) return Nil;

    int64_t iv = 0;
    if (parse_int64(raw, iv)) {
        auto out = make_fixnum(heap, iv);
        if (!out) return std::unexpected(out.error());
        return *out;
    }

    double dv = 0.0;
    if (parse_double(raw, dv)) {
        auto out = make_flonum(dv);
        if (!out) return std::unexpected(out.error());
        return *out;
    }

    auto str = make_string(heap, intern_table, std::string(raw));
    if (!str) {
        return std::unexpected(RuntimeError{
            VMError{RuntimeErrorCode::TypeError, std::string(who) + ": failed to allocate string"}});
    }
    return *str;
}

bool is_null_token(csv::string_view value, const std::vector<std::string>& null_tokens) {
    for (const auto& token : null_tokens) {
        if (value == token) return true;
    }
    return false;
}

bool parse_int64_text(csv::string_view value, int64_t& out) {
    const auto* begin = value.data();
    const auto* end = begin + value.size();
    auto [ptr, ec] = std::from_chars(begin, end, out, 10);
    return ec == std::errc{} && ptr == end;
}

bool parse_double_text(csv::string_view value, double& out) {
    const auto* begin = value.data();
    const auto* end = begin + value.size();
    auto [ptr, ec] = std::from_chars(begin, end, out, std::chars_format::general);
    return ec == std::errc{} && ptr == end;
}

void infer_column_types(
    std::vector<InferredColumnType>& types,
    const csv::CSVRow& row,
    const std::vector<std::string>& null_tokens) {
    for (std::size_t i = 0; i < row.size(); ++i) {
        if (types[i] == InferredColumnType::String) continue;

        const auto value = row[i].get<csv::string_view>();
        if (is_null_token(value, null_tokens)) continue;

        int64_t iv = 0;
        if (parse_int64_text(value, iv)) continue;

        double dv = 0.0;
        if (parse_double_text(value, dv)) {
            types[i] = InferredColumnType::Float;
            continue;
        }

        types[i] = InferredColumnType::String;
    }
}

std::expected<LispVal, RuntimeError> parse_fact_table_cell(
    Heap& heap,
    InternTable& intern_table,
    csv::string_view value,
    InferredColumnType column_type,
    const std::vector<std::string>& null_tokens,
    const char* who) {
    if (is_null_token(value, null_tokens)) return Nil;

    if (column_type == InferredColumnType::Int) {
        int64_t iv = 0;
        if (parse_int64_text(value, iv)) {
            auto out = make_fixnum(heap, iv);
            if (!out) return std::unexpected(out.error());
            return *out;
        }
    } else if (column_type == InferredColumnType::Float) {
        double dv = 0.0;
        if (parse_double_text(value, dv)) {
            auto out = make_flonum(dv);
            if (!out) return std::unexpected(out.error());
            return *out;
        }
    }

    auto str = make_string(heap, intern_table, std::string(value));
    if (!str) {
        return std::unexpected(RuntimeError{
            VMError{RuntimeErrorCode::TypeError, std::string(who) + ": failed to allocate string"}});
    }
    return *str;
}

std::vector<std::string> fallback_column_names(std::size_t count) {
    std::vector<std::string> names;
    names.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        names.push_back("col" + std::to_string(i + 1));
    }
    return names;
}

std::expected<std::vector<LispVal>, RuntimeError> decode_row_values(
    Heap& heap,
    LispVal value,
    const char* who) {
    if (ops::is_boxed(value) && ops::tag(value) == Tag::HeapObject) {
        if (auto* vec = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(value))) {
            return vec->elements;
        }
    }

    std::vector<LispVal> out;
    LispVal cur = value;
    while (cur != Nil) {
        if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject) {
            return type_error(std::string(who) + ": second argument must be a vector or proper list");
        }
        auto* cell = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
        if (!cell) {
            return type_error(std::string(who) + ": second argument must be a vector or proper list");
        }
        out.push_back(cell->car);
        cur = cell->cdr;
    }
    return out;
}

std::expected<std::vector<std::string>, RuntimeError> decode_column_names(
    Heap& heap,
    InternTable& intern_table,
    LispVal value,
    const char* who) {
    if (ops::is_boxed(value) && ops::tag(value) == Tag::HeapObject) {
        if (auto* vec = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(value))) {
            std::vector<std::string> out;
            out.reserve(vec->elements.size());
            for (auto item : vec->elements) {
                auto key = require_symbol_or_string(intern_table, item, who, "column key");
                if (!key) return std::unexpected(key.error());
                out.push_back(std::move(*key));
            }
            return out;
        }
    }

    std::vector<std::string> out;
    LispVal cur = value;
    while (cur != Nil) {
        if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject) {
            return type_error(std::string(who) + ": third argument must be a vector or proper list");
        }
        auto* cell = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
        if (!cell) {
            return type_error(std::string(who) + ": third argument must be a vector or proper list");
        }
        auto key = require_symbol_or_string(intern_table, cell->car, who, "column key");
        if (!key) return std::unexpected(key.error());
        out.push_back(std::move(*key));
        cur = cell->cdr;
    }
    return out;
}

std::expected<std::unordered_map<std::string, LispVal>, RuntimeError> decode_record_map(
    Heap& heap,
    InternTable& intern_table,
    LispVal alist,
    const char* who) {
    std::unordered_map<std::string, LispVal> out;
    LispVal cur = alist;
    while (cur != Nil) {
        if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject) {
            return type_error(std::string(who) + ": second argument must be an alist");
        }
        auto* row = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
        if (!row || !ops::is_boxed(row->car) || ops::tag(row->car) != Tag::HeapObject) {
            return type_error(std::string(who) + ": second argument must be an alist");
        }
        auto* kv = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(row->car));
        if (!kv) {
            return type_error(std::string(who) + ": second argument must be an alist");
        }

        auto key = require_symbol_or_string(intern_table, kv->car, who, "record key");
        if (!key) return std::unexpected(key.error());
        out.emplace(std::move(*key), kv->cdr);
        cur = row->cdr;
    }
    return out;
}

std::expected<CsvCell, RuntimeError> value_to_cell(
    Heap& heap,
    InternTable& intern_table,
    LispVal value,
    const char* who) {
    if (value == Nil) return CsvCell{"", false};

    auto n = classify_numeric(value, heap);
    if (n.is_fixnum()) return CsvCell{std::to_string(n.int_val), true};
    if (n.is_flonum()) {
        std::ostringstream oss;
        oss << n.float_val;
        return CsvCell{oss.str(), true};
    }

    if (ops::is_boxed(value) && ops::tag(value) == Tag::Symbol) {
        auto text = intern_table.get_string(ops::payload(value));
        if (!text) {
            return std::unexpected(RuntimeError{
                VMError{RuntimeErrorCode::InternalError, std::string(who) + ": unresolved symbol id"}});
        }
        return CsvCell{std::string(*text), false};
    }

    auto sv = StringView::try_from(value, intern_table);
    if (sv) return CsvCell{std::string(sv->view()), false};

    return CsvCell{format_value(value, FormatMode::Display, heap, intern_table), false};
}

bool needs_quotes(std::string_view value, char delimiter, char quote) {
    for (char ch : value) {
        if (ch == delimiter || ch == quote || ch == '\n' || ch == '\r') {
            return true;
        }
    }
    return false;
}

std::string encode_field(const CsvWriterBackend& backend, const CsvCell& cell) {
    bool quote = false;
    switch (backend.quote_policy) {
        case CsvQuotePolicy::Minimal:
            quote = needs_quotes(cell.text, backend.delimiter, backend.quote);
            break;
        case CsvQuotePolicy::All:
            quote = true;
            break;
        case CsvQuotePolicy::NonNumeric:
            quote = !cell.numeric;
            break;
        case CsvQuotePolicy::None:
            quote = false;
            break;
    }

    if (!quote) return cell.text;

    std::string out;
    out.reserve(cell.text.size() + 2);
    out.push_back(backend.quote);
    for (char ch : cell.text) {
        if (ch == backend.quote) out.push_back(backend.quote);
        out.push_back(ch);
    }
    out.push_back(backend.quote);
    return out;
}

std::expected<void, RuntimeError> write_cells(
    CsvWriterBackend& backend,
    const std::vector<CsvCell>& cells,
    const char* who) {
    if (!backend.stream || !backend.stream->is_open()) {
        return type_error(std::string(who) + ": writer is closed");
    }

    auto& out = *backend.stream;
    for (std::size_t i = 0; i < cells.size(); ++i) {
        out << encode_field(backend, cells[i]);
        if (i + 1 != cells.size()) out << backend.delimiter;
    }
    out << '\n';

    if (!out.good()) {
        return open_error(who, "failed to write row");
    }
    return {};
}

} // namespace

void register_csv_builtins(BuiltinEnvironment& env,
                           Heap& heap,
                           InternTable& intern_table) {
    env.register_builtin("%csv-open-reader", 2, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto path = require_string(intern_table, args[0], "%csv-open-reader", "first argument");
            if (!path) return std::unexpected(path.error());

            auto options = parse_reader_options(heap, intern_table, args[1], "%csv-open-reader");
            if (!options) return std::unexpected(options.error());

            csv::CSVFormat format;
            try {
                format = make_reader_format(*options);
            } catch (const std::exception& ex) {
                return open_error("%csv-open-reader", ex.what());
            }

            auto backend = std::make_shared<CsvReaderBackend>();
            try {
                backend->reader = std::make_unique<csv::CSVReader>(*path, format);
            } catch (const std::exception& ex) {
                return open_error("%csv-open-reader", ex.what());
            }

            auto columns = backend->reader->get_col_names();
            auto symbols = make_column_symbols(intern_table, columns, "%csv-open-reader");
            if (!symbols) return std::unexpected(symbols.error());

            types::CsvReader reader;
            reader.state = std::static_pointer_cast<void>(backend);
            reader.columns = std::move(columns);
            reader.column_symbols = std::move(*symbols);
            reader.row_index = 0;
            reader.comment = options->comment;
            reader.null_tokens = std::move(options->null_tokens);

            return make_csv_reader(heap, std::move(reader));
        });

    env.register_builtin("%csv-reader-from-string", 2, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto input = require_string(intern_table, args[0], "%csv-reader-from-string", "first argument");
            if (!input) return std::unexpected(input.error());

            auto options = parse_reader_options(heap, intern_table, args[1], "%csv-reader-from-string");
            if (!options) return std::unexpected(options.error());

            csv::CSVFormat format;
            try {
                format = make_reader_format(*options);
            } catch (const std::exception& ex) {
                return open_error("%csv-reader-from-string", ex.what());
            }

            auto backend = std::make_shared<CsvReaderBackend>();
            backend->source = std::make_shared<std::stringstream>(*input);
            try {
                backend->reader = std::make_unique<csv::CSVReader>(*backend->source, format);
            } catch (const std::exception& ex) {
                return open_error("%csv-reader-from-string", ex.what());
            }

            auto columns = backend->reader->get_col_names();
            auto symbols = make_column_symbols(intern_table, columns, "%csv-reader-from-string");
            if (!symbols) return std::unexpected(symbols.error());

            types::CsvReader reader;
            reader.state = std::static_pointer_cast<void>(backend);
            reader.columns = std::move(columns);
            reader.column_symbols = std::move(*symbols);
            reader.row_index = 0;
            reader.comment = options->comment;
            reader.null_tokens = std::move(options->null_tokens);

            return make_csv_reader(heap, std::move(reader));
        });

    env.register_builtin("%csv-columns", 1, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            auto reader = require_csv_reader(heap, args[0], "%csv-columns");
            if (!reader) return std::unexpected(reader.error());
            return make_vector(heap, (*reader)->column_symbols);
        });

    env.register_builtin("%csv-read-row", 1, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto reader = require_csv_reader(heap, args[0], "%csv-read-row");
            if (!reader) return std::unexpected(reader.error());

            auto backend = require_reader_backend(**reader, "%csv-read-row");
            if (!backend) return std::unexpected(backend.error());

            csv::CSVRow row;
            if (!read_next_row(**reader, **backend, row)) return False;

            auto values = row_to_string_values(heap, intern_table, row, "%csv-read-row");
            if (!values) return std::unexpected(values.error());

            (*reader)->row_index += 1;
            return make_vector(heap, std::move(*values));
        });

    env.register_builtin("%csv-read-record", 1, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto reader = require_csv_reader(heap, args[0], "%csv-read-record");
            if (!reader) return std::unexpected(reader.error());

            auto backend = require_reader_backend(**reader, "%csv-read-record");
            if (!backend) return std::unexpected(backend.error());

            csv::CSVRow row;
            if (!read_next_row(**reader, **backend, row)) return False;

            if ((*reader)->column_symbols.empty()) {
                return type_error("%csv-read-record: reader has no column names");
            }
            if (row.size() != (*reader)->column_symbols.size()) {
                return type_error("%csv-read-record: row width does not match column count");
            }

            auto values = row_to_string_values(heap, intern_table, row, "%csv-read-record");
            if (!values) return std::unexpected(values.error());

            auto alist = vector_to_alist(heap, (*reader)->column_symbols, *values);
            if (!alist) return std::unexpected(alist.error());

            (*reader)->row_index += 1;
            return *alist;
        });

    env.register_builtin("%csv-read-typed-row", 1, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto reader = require_csv_reader(heap, args[0], "%csv-read-typed-row");
            if (!reader) return std::unexpected(reader.error());

            auto backend = require_reader_backend(**reader, "%csv-read-typed-row");
            if (!backend) return std::unexpected(backend.error());

            csv::CSVRow row;
            if (!read_next_row(**reader, **backend, row)) return False;

            std::vector<LispVal> values;
            values.reserve(row.size());
            for (std::size_t i = 0; i < row.size(); ++i) {
                auto typed = parse_typed_value(heap, intern_table, **reader, row[i].get<csv::string_view>(), "%csv-read-typed-row");
                if (!typed) return std::unexpected(typed.error());
                values.push_back(*typed);
            }

            (*reader)->row_index += 1;
            return make_vector(heap, std::move(values));
        });

    env.register_builtin("%csv-close", 1, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            if (ops::is_boxed(args[0]) && ops::tag(args[0]) == Tag::HeapObject) {
                const auto id = ops::payload(args[0]);
                if (auto* reader = heap.try_get_as<ObjectKind::CsvReader, types::CsvReader>(id)) {
                    close_reader(*reader);
                    return Nil;
                }
                if (auto* writer = heap.try_get_as<ObjectKind::CsvWriter, types::CsvWriter>(id)) {
                    close_writer(*writer);
                    return Nil;
                }
            }
            return type_error("%csv-close: first argument must be a csv-reader or csv-writer");
        });

    env.register_builtin("%csv-open-writer", 2, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto path = require_string(intern_table, args[0], "%csv-open-writer", "first argument");
            if (!path) return std::unexpected(path.error());

            auto options = parse_writer_options(heap, intern_table, args[1], "%csv-open-writer");
            if (!options) return std::unexpected(options.error());

            auto backend = std::make_shared<CsvWriterBackend>();
            backend->delimiter = options->delimiter;
            backend->quote = options->quote;
            backend->quote_policy = options->quote_policy;
            backend->stream = std::make_unique<std::ofstream>(*path, std::ios::out | std::ios::binary | std::ios::trunc);
            if (!backend->stream || !backend->stream->is_open()) {
                return open_error("%csv-open-writer", "failed to open output file");
            }

            if (options->header && options->column_names.has_value()) {
                std::vector<CsvCell> header;
                header.reserve(options->column_names->size());
                for (const auto& col : *options->column_names) {
                    header.push_back(CsvCell{col, false});
                }

                auto written = write_cells(*backend, header, "%csv-open-writer");
                if (!written) return std::unexpected(written.error());
            }

            types::CsvWriter writer;
            writer.state = std::static_pointer_cast<void>(backend);
            writer.row_index = 0;
            return make_csv_writer(heap, std::move(writer));
        });

    env.register_builtin("%csv-write-row", 2, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto writer = require_csv_writer(heap, args[0], "%csv-write-row");
            if (!writer) return std::unexpected(writer.error());

            auto backend = require_writer_backend(**writer, "%csv-write-row");
            if (!backend) return std::unexpected(backend.error());

            auto row_values = decode_row_values(heap, args[1], "%csv-write-row");
            if (!row_values) return std::unexpected(row_values.error());

            std::vector<CsvCell> cells;
            cells.reserve(row_values->size());
            for (auto value : *row_values) {
                auto cell = value_to_cell(heap, intern_table, value, "%csv-write-row");
                if (!cell) return std::unexpected(cell.error());
                cells.push_back(std::move(*cell));
            }

            auto written = write_cells(**backend, cells, "%csv-write-row");
            if (!written) return std::unexpected(written.error());
            (*writer)->row_index += 1;
            return Nil;
        });

    env.register_builtin("%csv-write-record", 3, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto writer = require_csv_writer(heap, args[0], "%csv-write-record");
            if (!writer) return std::unexpected(writer.error());

            auto backend = require_writer_backend(**writer, "%csv-write-record");
            if (!backend) return std::unexpected(backend.error());

            auto record = decode_record_map(heap, intern_table, args[1], "%csv-write-record");
            if (!record) return std::unexpected(record.error());

            auto columns = decode_column_names(heap, intern_table, args[2], "%csv-write-record");
            if (!columns) return std::unexpected(columns.error());

            std::vector<CsvCell> cells;
            cells.reserve(columns->size());
            for (const auto& col : *columns) {
                auto it = record->find(col);
                if (it == record->end()) {
                    cells.push_back(CsvCell{"", false});
                    continue;
                }

                auto cell = value_to_cell(heap, intern_table, it->second, "%csv-write-record");
                if (!cell) return std::unexpected(cell.error());
                cells.push_back(std::move(*cell));
            }

            auto written = write_cells(**backend, cells, "%csv-write-record");
            if (!written) return std::unexpected(written.error());
            (*writer)->row_index += 1;
            return Nil;
        });

    env.register_builtin("%csv-flush", 1, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            auto writer = require_csv_writer(heap, args[0], "%csv-flush");
            if (!writer) return std::unexpected(writer.error());

            auto backend = require_writer_backend(**writer, "%csv-flush");
            if (!backend) return std::unexpected(backend.error());

            (*backend)->stream->flush();
            if (!(*backend)->stream->good()) {
                return open_error("%csv-flush", "flush failed");
            }
            return Nil;
        });

    env.register_builtin("%fact-table-load-csv", 2, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto path = require_string(intern_table, args[0], "%fact-table-load-csv", "first argument");
            if (!path) return std::unexpected(path.error());

            auto options = parse_fact_table_load_options(heap, intern_table, args[1], "%fact-table-load-csv");
            if (!options) return std::unexpected(options.error());

            csv::CSVFormat format;
            try {
                format = make_reader_format(options->reader);
            } catch (const std::exception& ex) {
                return open_error("%fact-table-load-csv", ex.what());
            }

            auto open_reader = [&]() -> std::expected<std::unique_ptr<csv::CSVReader>, RuntimeError> {
                try {
                    return std::make_unique<csv::CSVReader>(*path, format);
                } catch (const std::exception& ex) {
                    return open_error("%fact-table-load-csv", ex.what());
                }
            };

            auto probe = open_reader();
            if (!probe) return std::unexpected(probe.error());

            std::vector<std::string> columns = (*probe)->get_col_names();
            std::vector<InferredColumnType> inferred;
            if (!columns.empty()) {
                inferred.assign(
                    columns.size(),
                    options->infer_types ? InferredColumnType::Int : InferredColumnType::String);
            }

            csv::CSVRow row;
            while (read_next_row(**probe, options->reader.comment, row)) {
                if (columns.empty()) {
                    columns = fallback_column_names(row.size());
                    inferred.assign(
                        columns.size(),
                        options->infer_types ? InferredColumnType::Int : InferredColumnType::String);
                }
                if (row.size() != columns.size()) {
                    return type_error("%fact-table-load-csv: row width does not match column count");
                }
                if (options->infer_types) {
                    infer_column_types(inferred, row, options->reader.null_tokens);
                }
            }

            if (columns.empty()) {
                return type_error("%fact-table-load-csv: CSV has no columns");
            }
            if (inferred.empty()) {
                inferred.assign(
                    columns.size(),
                    options->infer_types ? InferredColumnType::Int : InferredColumnType::String);
            }

            auto table_val = make_fact_table(heap, columns);
            if (!table_val) return std::unexpected(table_val.error());

            auto* table = heap.try_get_as<ObjectKind::FactTable, types::FactTable>(ops::payload(*table_val));
            if (!table) {
                return std::unexpected(RuntimeError{
                    VMError{RuntimeErrorCode::InternalError, "%fact-table-load-csv: failed to allocate fact-table"}});
            }

            auto ingest = open_reader();
            if (!ingest) return std::unexpected(ingest.error());

            while (read_next_row(**ingest, options->reader.comment, row)) {
                if (row.size() != columns.size()) {
                    return type_error("%fact-table-load-csv: row width does not match column count");
                }

                std::vector<LispVal> values;
                values.reserve(columns.size());
                for (std::size_t i = 0; i < columns.size(); ++i) {
                    auto cell = parse_fact_table_cell(
                        heap,
                        intern_table,
                        row[i].get<csv::string_view>(),
                        inferred[i],
                        options->reader.null_tokens,
                        "%fact-table-load-csv");
                    if (!cell) return std::unexpected(cell.error());
                    values.push_back(*cell);
                }

                if (!table->add_row(values)) {
                    return type_error("%fact-table-load-csv: row arity mismatch");
                }
            }

            return *table_val;
        });

    env.register_builtin("%fact-table-save-csv", 3, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto table = require_fact_table(heap, args[0], "%fact-table-save-csv");
            if (!table) return std::unexpected(table.error());

            auto path = require_string(intern_table, args[1], "%fact-table-save-csv", "second argument");
            if (!path) return std::unexpected(path.error());

            auto options = parse_writer_options(heap, intern_table, args[2], "%fact-table-save-csv");
            if (!options) return std::unexpected(options.error());

            CsvWriterBackend backend;
            backend.delimiter = options->delimiter;
            backend.quote = options->quote;
            backend.quote_policy = options->quote_policy;
            backend.stream = std::make_unique<std::ofstream>(*path, std::ios::out | std::ios::binary | std::ios::trunc);
            if (!backend.stream || !backend.stream->is_open()) {
                return open_error("%fact-table-save-csv", "failed to open output file");
            }

            const auto columns = options->column_names.has_value()
                ? *options->column_names
                : (*table)->col_names;

            std::vector<std::size_t> column_indices;
            column_indices.reserve(columns.size());
            for (const auto& name : columns) {
                std::optional<std::size_t> index;
                for (std::size_t i = 0; i < (*table)->col_names.size(); ++i) {
                    if ((*table)->col_names[i] == name) {
                        index = i;
                        break;
                    }
                }
                if (!index.has_value()) {
                    return type_error("%fact-table-save-csv: unknown column '" + name + "'");
                }
                column_indices.push_back(*index);
            }

            if (options->header) {
                std::vector<CsvCell> header;
                header.reserve(columns.size());
                for (const auto& name : columns) {
                    header.push_back(CsvCell{name, false});
                }
                auto wrote = write_cells(backend, header, "%fact-table-save-csv");
                if (!wrote) return std::unexpected(wrote.error());
            }

            for (std::size_t row_idx = 0; row_idx < (*table)->row_count; ++row_idx) {
                if ((*table)->live_mask[row_idx] == 0) continue;

                std::vector<CsvCell> cells;
                cells.reserve(column_indices.size());
                for (auto col_idx : column_indices) {
                    auto value = (*table)->columns[col_idx][row_idx];
                    auto cell = value_to_cell(heap, intern_table, value, "%fact-table-save-csv");
                    if (!cell) return std::unexpected(cell.error());
                    cells.push_back(std::move(*cell));
                }

                auto wrote = write_cells(backend, cells, "%fact-table-save-csv");
                if (!wrote) return std::unexpected(wrote.error());
            }

            backend.stream->flush();
            if (!backend.stream->good()) {
                return open_error("%fact-table-save-csv", "flush failed");
            }
            return Nil;
        });

    env.register_builtin("%csv-reader?", 1, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject) {
                return False;
            }
            return heap.try_get_as<ObjectKind::CsvReader, types::CsvReader>(ops::payload(args[0]))
                ? True
                : False;
        });

    env.register_builtin("%csv-writer?", 1, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject) {
                return False;
            }
            return heap.try_get_as<ObjectKind::CsvWriter, types::CsvWriter>(ops::payload(args[0]))
                ? True
                : False;
        });
}

} // namespace eta::runtime
