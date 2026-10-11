#include <boost/test/unit_test.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <thread>

#include "eta/cst/bridge_sexpr.h"
#include "eta/cst/diagnostic.h"

namespace {
    using namespace eta::cst;

    ParseResult check(std::string source, ReaderConfig config = {}) {
        auto parsed = parse(source, config);
        BOOST_REQUIRE_MESSAGE(parsed.has_value(), (parsed ? "" : parsed.error().message));
        BOOST_REQUIRE(has_unique_byte_ownership(*parsed->file));
        auto serialized = serialize_losslessly(*parsed->file);
        BOOST_REQUIRE(serialized.has_value());
        BOOST_TEST(*serialized == source);
        auto reader = read_original(*parsed->file);
        if (reader && parsed->file->source->valid_utf8())
            BOOST_TEST(static_cast<int>(parsed->status) == static_cast<int>(Status::Valid));
        else if (!reader)
            BOOST_TEST(static_cast<int>(parsed->status) == static_cast<int>(Status::Recovered));
        if (parsed->status == Status::Valid) {
            BOOST_REQUIRE(reader.has_value());
            auto bridged = to_sexpr(*parsed);
            BOOST_REQUIRE_MESSAGE(bridged.has_value(), (bridged ? "" : bridged.error().message));
            BOOST_REQUIRE(bridged->size() == reader->size());
            for (std::size_t i = 0; i < reader->size(); ++i)
                BOOST_TEST(equivalent_sexpr(*(*reader)[i], *(*bridged)[i]));
        } else BOOST_TEST(!to_sexpr(*parsed).has_value());
        return std::move(*parsed);
    }
}

BOOST_AUTO_TEST_SUITE(cst_tests)

BOOST_AUTO_TEST_CASE(reader_inventory) {
    for (const std::string source: {
             "", " \r\n\t", "() [a b] (a . b)", "#(a [b]) #u8(0 255 #xFF)",
             "'x `[,x ,@y] ,x ,@x", "#t #TRUE #false #\\space #\\x1F600;", "Nil nil |a b| |a\\|b|",
             "\"a\\n\\x1F600;\" #b101 #o77 #xFF #e#xA #i1.5 1e3 .5 +inf.0 -nan.0",
             "; header\r\n(a ; same line\n #| nested #| comment |# body |# b) ; EOF",
             "#!fold-case A #!no-fold-case B #!unknown C", "#!/usr/bin/env eta\r\n(a)",
             "#; #; a b c", "' #; x y", "(a #;(ignored x) b)", "#;(. .) x", "#;#u8(bad . x) y",
             "#;([a) b) x", "#;`(,#;x y) z", "(module x (export a) (begin (define a 1)))"
         }) {
        BOOST_TEST_CONTEXT(source) {
            const auto parsed = check(source);
            BOOST_TEST(static_cast<int>(parsed.status) == static_cast<int>(Status::Valid));
        }
    }
    const auto strict = check(",x", {Grammar::EtaCurrent, true});
    BOOST_TEST(static_cast<int>(strict.status) == static_cast<int>(Status::Recovered));
    const auto quoted = check("`(,x ,@y)", {Grammar::EtaCurrent, true});
    BOOST_TEST(static_cast<int>(quoted.status) == static_cast<int>(Status::Valid));
}

BOOST_AUTO_TEST_CASE(headers_and_recovery) {
    for (const std::string source: {
             "\xEF\xBB\xBF", "\xEF\xBB\xBF(a)", "#!eta", "#!eta\r\n",
             ") a", "(a] b", "\"unterminated", "#| unterminated", "#;", "#;#;x", "'", "(a .)",
             "#q(unrecognised)", "|unterminated", "#u8(256)", "(a . b c)"
         })
        check(source);
    auto bom = check("\xEF\xBB\xBF");
    BOOST_TEST(static_cast<int>(bom.file->tokens.front().kind) == static_cast<int>(TokenKind::FileBom));
    BOOST_TEST(bom.file->tokens.front().span.begin == 0u);
    BOOST_TEST(bom.file->tokens.front().span.end == 3u);
    auto shebang = check("#!eta\r\n");
    BOOST_TEST(shebang.file->source->slice(shebang.file->tokens.front().span) == "#!eta");
    BOOST_TEST(shebang.file->tokens.back().span.begin == 7u);
}

BOOST_AUTO_TEST_CASE(discard_boundaries_and_comments) {
    const auto parsed = check("#; #; a b c");
    const auto &file = *parsed.file;
    BOOST_REQUIRE(file.roots.size() == 2u);
    const auto &outer = file.nodes[file.roots[0]];
    BOOST_TEST(static_cast<int>(outer.kind) == static_cast<int>(NodeKind::DatumComment));
    BOOST_TEST(outer.children.size() == 2u);
    BOOST_TEST(file.source->slice(file.tokens[outer.token_end - 1].span) == "b");
    auto comments = check("; header\n' ; operand\n x ; end\n( ; dangling\n)\n; final");
    BOOST_TEST(comments.file->associations.size() == 5u);
    BOOST_TEST(static_cast<int>(comments.file->associations[0].kind) == static_cast<int>(CommentAssociation::Header));
    BOOST_TEST(
        static_cast<int>(comments.file->associations[1].kind) == static_cast<int>(CommentAssociation::ReaderOperand));
}

BOOST_AUTO_TEST_CASE(state_traces) {
    auto a = check("#!fold-case (A #;#!no-fold-case B C)");
    auto b = check("#!fold-case\n( A  #; #!no-fold-case\nB C )\n");
    BOOST_TEST(equivalent_reader_trace(*a.file, *b.file));
    auto c = check("#!no-fold-case (A #;#!no-fold-case B C)");
    BOOST_TEST(!equivalent_reader_trace(*a.file, *c.file));
    bool discarded_directive = false;
    for (const auto &entry: a.file->trace)
        if (a.file->tokens[entry.token].kind == TokenKind::Directive && entry.path.size() == 2)
            discarded_directive = true;
    BOOST_TEST(discarded_directive);
}

BOOST_AUTO_TEST_CASE(unicode_source_maps) {
    Source source(1, "unicode", "a\t\xF0\x9F\x98\x80\r\n\xC3\xA9");
    BOOST_TEST(source.valid_utf8());
    BOOST_TEST(source.position(6, PositionEncoding::UnicodeScalar)->character == 3u);
    BOOST_TEST(source.position(6, PositionEncoding::Utf16)->character == 4u);
    BOOST_TEST(source.position(6, PositionEncoding::Utf8)->character == 6u);
    BOOST_TEST(source.position(8, PositionEncoding::Utf16)->line == 1u);
    BOOST_TEST(*source.offset({0, 4}, PositionEncoding::Utf16) == 6u);
    BOOST_TEST(!source.offset({0, 3}, PositionEncoding::Utf16).has_value());
    BOOST_TEST(!source.is_edit_boundary(7));
    BOOST_TEST(!source.is_edit_boundary(3));
    BOOST_TEST(source.is_edit_boundary(10));
    Source invalid(2, "invalid", std::string("a\xFF\x80z", 4));
    BOOST_TEST(!invalid.valid_utf8());
    BOOST_TEST(invalid.position(3, PositionEncoding::Utf16)->character == 2u);
    BOOST_TEST(!invalid.position(2, PositionEncoding::Utf16).has_value());
    check(std::string(";\xFF\x80\nx", 5));
    const std::string long_text = std::string(255, 'x') + "\xF0\x9F\x98\x80\r\n" + std::string(300, 'y');
    Source indexed(3, "indexed", long_text);
    for (const auto encoding : {PositionEncoding::UnicodeScalar, PositionEncoding::Utf16, PositionEncoding::Utf8}) {
        for (std::size_t offset = 0; offset <= long_text.size(); ++offset) {
            if (!indexed.is_edit_boundary(offset)) continue;
            const auto position = indexed.position(offset, encoding);
            BOOST_REQUIRE(position.has_value());
            const auto reversed = indexed.offset(*position, encoding);
            BOOST_REQUIRE(reversed.has_value());
            BOOST_TEST(*reversed == offset);
        }
    }
    std::string long_form = "(";
    for (int i = 0; i < 10'000; ++i) long_form += "value ";
    long_form += ")";
    check(long_form);
}

BOOST_AUTO_TEST_CASE(resource_limits_and_cancellation) {
    Operation operation;
    operation.limits.input_bytes = 3;
    BOOST_TEST(parse("abc", {}, operation).has_value());
    BOOST_TEST(!parse("abcd", {}, operation).has_value());
    operation = {};
    operation.limits.depth = 4;
    BOOST_TEST(parse("((((x))))", {}, operation).has_value());
    BOOST_TEST(!parse("(((((x)))))", {}, operation).has_value());
    BOOST_TEST(!parse("#;#;#;#;#;x x x x x", {}, operation).has_value());
    operation = {};
    operation.limits.tokens = 2;
    BOOST_TEST(parse("x", {}, operation).has_value());
    BOOST_TEST(!parse("x y", {}, operation).has_value());
    operation = {};
    operation.limits.nodes = 1;
    BOOST_TEST(parse("x", {}, operation).has_value());
    BOOST_TEST(!parse("x y", {}, operation).has_value());
    operation = {};
    operation.limits.memory_bytes = 1;
    BOOST_TEST(!parse("x", {}, operation).has_value());
    operation = {};
    operation.limits.output_bytes = 1;
    auto output = parse("xy", {}, operation);
    BOOST_REQUIRE(output.has_value());
    BOOST_TEST(!serialize_losslessly(*output->file).has_value());
    auto cancelled = std::make_shared<std::atomic_bool>(true);
    operation = {};
    operation.cancelled = cancelled;
    BOOST_TEST(!parse("x", {}, operation).has_value());
    cancelled->store(false);
    auto snapshot = parse("x", {}, operation);
    BOOST_REQUIRE(snapshot.has_value());
    cancelled->store(true);
    BOOST_TEST(!serialize_losslessly(*snapshot->file).has_value());
    BOOST_TEST(!to_sexpr(*snapshot).has_value());
    BOOST_TEST(!read_original(*snapshot->file).has_value());
    auto memory = parse("x", {});
    BOOST_REQUIRE(memory.has_value());
    operation = {};
    operation.limits.memory_bytes = memory->file->peak_memory_bytes();
    BOOST_TEST(parse("x", {}, operation).has_value());
    --operation.limits.memory_bytes;
    BOOST_TEST(!parse("x", {}, operation).has_value());
    operation = {};
    operation.limits.output_bytes = 2;
    auto exact_output = parse("xy", {}, operation);
    BOOST_REQUIRE(exact_output.has_value());
    BOOST_TEST(serialize_losslessly(*exact_output->file).has_value());
}

BOOST_AUTO_TEST_CASE(cancellation_during_scan_and_snapshot_lifetime) {
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    Operation operation;
    operation.cancelled = cancelled;
    const std::string source = ";" + std::string(16 * 1024 * 1024 - 1, 'x');
    std::thread requester([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        cancelled->store(true);
    });
    auto result = parse(source, {}, operation);
    requester.join();
    BOOST_REQUIRE(!result.has_value());
    BOOST_TEST(result.error().code == "cst.Cancelled");
    cancelled->store(false);
    const std::string line_heavy(16 * 1024 * 1024, '\n');
    std::thread line_requester([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        cancelled->store(true);
    });
    auto line_result = parse(line_heavy, {}, operation);
    line_requester.join();
    BOOST_REQUIRE(!line_result.has_value());
    BOOST_TEST(line_result.error().code == "cst.Cancelled");
    std::weak_ptr<const File> lifetime;
    {
        auto snapshot = parse("x", {});
        BOOST_REQUIRE(snapshot.has_value());
        lifetime = snapshot->file;
        auto consumer = snapshot->file;
        snapshot->file.reset();
        BOOST_TEST(!lifetime.expired());
    }
    BOOST_TEST(lifetime.expired());
}

BOOST_AUTO_TEST_CASE(arbitrary_byte_property) {
    std::mt19937 generator(0xE7A0);
    for (std::size_t i = 0; i < 1000; ++i) {
        std::string bytes;
        const auto size = generator() % 128;
        for (std::size_t j = 0; j < size; ++j) bytes.push_back(static_cast<char>(generator() % 256));
        check(bytes);
    }
}

BOOST_AUTO_TEST_CASE(repository_corpus) {
    const auto root = std::filesystem::path(ETA_COOKBOOK_DIR).parent_path();
    std::size_t count = 0;
    for (const auto *directory: {"stdlib", "cookbook", "demo"}) {
        const auto path = root / directory;
        if (!std::filesystem::exists(path)) continue;
        BOOST_REQUIRE(std::filesystem::is_directory(path));
        for (const auto &entry: std::filesystem::recursive_directory_iterator(path)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".eta") continue;
            BOOST_TEST_CONTEXT(entry.path().string()) {
                std::ifstream input(entry.path(), std::ios::binary);
                BOOST_REQUIRE(input.good());
                check(std::string(std::istreambuf_iterator<char>(input), {}));
                ++count;
            }
        }
    }
    BOOST_TEST(count > 100u);
}

BOOST_AUTO_TEST_CASE(diagnostic_adapter_inventory) {
    auto source = std::make_shared<const Source>(17, "provider.eta", "abc");
    eta::cst::DiagnosticContext context;
    context.producer = "compiler-service";
    context.module = "provider";
    context.resolve = [&](auto id) { return id == 17 ? source : nullptr; };
    const eta::reader::lexer::Span located{17, {0, 1, 1}, {1, 1, 2}};
    auto check_adapter = [&](const auto &error, int expected_exit) {
        const auto adapted = eta::cst::adapt(error, context);
        BOOST_TEST(adapted.classified);
        BOOST_TEST(adapted.exit_class == expected_exit);
        BOOST_TEST(adapted.source == "compiler");
        BOOST_REQUIRE(adapted.location.file.has_value());
        BOOST_TEST(*adapted.location.file == "provider.eta");
        BOOST_REQUIRE(adapted.location.span.has_value());
        BOOST_TEST(adapted.location.span->begin == 0u);
        BOOST_TEST(adapted.location.span->end == 1u);
        auto unlocated = error;
        unlocated.span = {};
        BOOST_TEST(!eta::cst::adapt(unlocated, context).location.span.has_value());
    };

#define CST_LEX_KIND(name, exit) check_adapter(eta::reader::lexer::LexError{eta::reader::lexer::LexErrorKind::name, located, "message"}, exit);
#define CST_PARSE_KIND(name, exit) check_adapter(eta::reader::parser::ParseError{eta::reader::parser::ParseErrorKind::name, located}, exit);
#define CST_EXPAND_KIND(name, exit) check_adapter(eta::reader::expander::ExpandError{eta::reader::expander::ExpandError::Kind::name, located, "message"}, exit);
#define CST_LINK_KIND(name, exit) check_adapter(eta::reader::linker::LinkError{eta::reader::linker::LinkError::Kind::name, located, "message"}, exit);
#define CST_UNIFIED_KIND(name, exit) check_adapter(eta::diagnostic::Diagnostic{eta::diagnostic::DiagnosticCode::name, eta::diagnostic::Severity::Warning, located, "message"}, exit);
#include "cst_diagnostic_kinds.inc"
#undef CST_LEX_KIND
#undef CST_PARSE_KIND
#undef CST_EXPAND_KIND
#undef CST_LINK_KIND
#undef CST_UNIFIED_KIND
    const auto unknown = eta::cst::adapt(eta::reader::linker::LinkError{
        static_cast<eta::reader::linker::LinkError::Kind>(255), {}, "future failure"}, context);
    BOOST_TEST(!unknown.classified);
    BOOST_TEST(unknown.exit_class == 1);
    BOOST_TEST(unknown.message == "future failure");
    const auto first = eta::cst::adapt_unclassified("first", context);
    const auto second = eta::cst::adapt_unclassified("second", context);
    BOOST_TEST(!eta::cst::same_diagnostic(first, second));
    eta::diagnostic::Diagnostic native{eta::diagnostic::DiagnosticCode::UndefinedName,
        eta::diagnostic::Severity::Note, located, "native"};
    native.related.push_back({located, "related"});
    const auto related = eta::cst::adapt(native, context);
    BOOST_TEST(related.related.size() == 1u);
    BOOST_TEST(static_cast<int>(related.severity) == static_cast<int>(eta::diagnostic::Severity::Note));
}

BOOST_AUTO_TEST_SUITE_END()
