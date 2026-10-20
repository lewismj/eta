#include <boost/test/unit_test.hpp>

#include <filesystem>
#include <fstream>
#include <random>

#include "eta/format/doc.h"
#include "eta/format/format_source.h"
#include "eta/tooling/files.h"
#include "eta/session/driver.h"
#include "eta/interpreter/module_path.h"

namespace {
    using namespace eta;
    namespace fs = std::filesystem;

    std::string checked(std::string input, format::Options options = {}) {
        auto result = format::format_source(input, options);
        std::string messages;
        for (const auto &diagnostic : result.diagnostics) messages += diagnostic.code + ": " + diagnostic.message + "\n";
        BOOST_REQUIRE_MESSAGE(result.output.has_value(), messages + "Input:\n" + input);
        auto repeated = format::format_source(*result.output, result.effective_options);
        BOOST_REQUIRE_MESSAGE(repeated.output.has_value(), "second pass refused");
        BOOST_TEST(*result.output == *repeated.output);
        return *result.output;
    }

    std::string read(const fs::path &path) {
        std::ifstream input(path, std::ios::binary);
        BOOST_REQUIRE(input.good());
        return std::string(std::istreambuf_iterator<char>(input), {});
    }

    void write(const fs::path &path, std::string_view bytes) {
        std::ofstream output(path, std::ios::binary);
        output << bytes;
        BOOST_REQUIRE(output.good());
    }
}

BOOST_AUTO_TEST_SUITE(formatter_tests)

BOOST_AUTO_TEST_CASE(doc_ir) {
    format::Docs docs;
    const auto body = docs.concat(docs.text("(h"), docs.concat(docs.nest(2, docs.concat(docs.line(), docs.text("argument"))), docs.text(")")));
    const auto group = docs.group(body);
    BOOST_TEST(*format::render(docs, group, 12, "\n") == "(h argument)");
    BOOST_TEST(*format::render(docs, group, 11, "\n") == "(h\n  argument)");
    auto soft = docs.group(docs.concat(docs.text("a"), docs.concat(docs.soft_line(), docs.text("b"))));
    BOOST_TEST(*format::render(docs, soft, 2, "\n") == "ab");
    BOOST_TEST(*format::render(docs, soft, 1, "\n") == "a\nb");
    auto conditional = docs.group(docs.concat(docs.text("a"), docs.if_flat(docs.text("b"), docs.concat(docs.hard_line(), docs.text("c")))));
    BOOST_TEST(*format::render(docs, conditional, 2, "\n") == "ab");
    BOOST_TEST(*format::render(docs, conditional, 1, "\n") == "a\nc");
    auto hard = docs.group(docs.concat(docs.text("a"), docs.concat(docs.hard_line(), docs.text("b"))));
    BOOST_TEST(*format::render(docs, hard, 100, "\r\n") == "a\r\nb");
    auto opaque = docs.concat(docs.text("a\n raw\n"), docs.align(docs.text("b")));
    BOOST_TEST(*format::render(docs, opaque, 100, "\n") == "a\n raw\nb");
}

BOOST_AUTO_TEST_CASE(golden_layouts) {
    const auto directory = fs::path(ETA_COOKBOOK_DIR).parent_path() / "eta/tools/formatter/tests/golden";
    std::size_t count = 0;
    for (const auto &entry : fs::directory_iterator(directory)) {
        if (entry.path().extension() != ".input") continue;
        const auto name = entry.path().stem().string();
        format::Options options;
        options.width = name.starts_with("narrow-") ? 12 : 100;
        BOOST_TEST_CONTEXT(name) { BOOST_TEST(checked(read(entry.path()), options) == read(directory / (name + ".expected"))); }
        ++count;
    }
    BOOST_TEST(count >= 12u);
    BOOST_TEST(checked("(h a b)", {.width = 7}) == "(h a b)");
    BOOST_TEST(checked("(h a b)", {.width = 6}) == "(h\n  a\n  b)");
    BOOST_TEST(checked("[h a b]", {.width = 6, .indent = 3}) == "[h\n   a\n   b]");
}

BOOST_AUTO_TEST_CASE(special_form_alias_layouts) {
    BOOST_TEST(checked("(defun demo (x) (+ x 1))", {.width = 12}) == "(defun demo\n  (x)\n  (+ x 1))");
    BOOST_TEST(checked("(letrec* ((x 1)) (+ x 1))", {.width = 12}) == "(letrec*\n  ((x 1))\n  (+ x 1))");
}

BOOST_AUTO_TEST_CASE(reader_comment_and_header_matrix) {
    const std::vector<std::string> inputs{
        "", " \t", "\n", "() [a b] (a . b)", "#(a [b]) #u8(0 255 #xFF)",
        "'x `[,x ,@y] ,x ,@x", "#t #TRUE #false #\\space #\\x1F600;",
        "Nil nil |a b| |a\\|b|", "\"a\\n\\x1F600;\" #b101 #o77 #xFF #e#xA #i1.5 1e3 .5 +inf.0 -nan.0 -0.0",
        "; header\n(a ; same\n #| nested #| comment |# body |# b) ; EOF",
        "#!fold-case A #!no-fold-case B #!unknown C", "#!/usr/bin/env eta\n(a)", "#!eta",
        "#; #; a b c", "' #; x y", "(a #;(ignored x) b)", "#;(. .) x", "#;#u8(bad . x) y",
        "#;([a) b) x", "#;`(,#;x y) z", "(module x (export a) (begin (define a 1)))",
        "' ; operand\n x", "(h ; repeated\n a ; repeated\n b)", "( ; dangling\n)", "(h a ; close\n)",
        "(h #|multi\n unchanged\n body|# a)", "(h a #| same |# b)", "(h a\n #| leading |# b)",
        "(define value (lambda (x . rest) (let ((a x)) (mystery _ ... a rest))))",
        "(unknown-macro [(_ ... x) 'y] #;pattern |escaped name|)",
        ";; eta-lint:disable=ETA301\n(let ((x 1)) x)", "(h a) ;; eta-lint:disable-line=ETA401\n(h b)",
        "(begin\n\n\n(a)\n\n\n(b))", "(h\n\n a\n\n b)", "  (h a b)"
    };
    for (const auto &input : inputs) {
        for (auto width : {100u, 80u, 40u, 20u}) {
            BOOST_TEST_CONTEXT(input << " width=" << width) {
                checked(input, {.width = width});
                std::string crlf;
                for (char c : input) { if (c == '\n') crlf += '\r'; crlf += c; }
                checked(crlf, {.width = width});
            }
        }
    }
    for (const auto &input : {"`(,x ,@y)", "`(a `(,b ,@c))", "'#;x y"}) checked(input, {.reader = {cst::Grammar::EtaCurrent, true}});
    const auto suppressed = format::format_source("(h #;(. .) a)");
    BOOST_REQUIRE(suppressed.output.has_value());
    BOOST_TEST(static_cast<int>(suppressed.outcome) == static_cast<int>(format::Outcome::Passthrough));
    BOOST_REQUIRE(suppressed.passthrough.size() == 1u);
    BOOST_TEST(suppressed.passthrough[0].span.begin == 3u);
    BOOST_TEST(suppressed.passthrough[0].span.end == 10u);
}

BOOST_AUTO_TEST_CASE(refusal_and_independent_corruption_guards) {
    for (const std::string source : {"(h", "(a]", "#q(x)", "\xEF\xBB\xBF(a)", ";\xFF\nx", "(h)\r", "(h)\r\n(a)\n"}) {
        const auto result = format::format_source(source);
        BOOST_TEST(!result.output.has_value());
        BOOST_TEST(result.exit_class == 2);
    }
    const auto original = cst::parse("(h ; first\n a ; second\n b)", {});
    BOOST_REQUIRE(original.has_value());
    for (const std::string corrupt : {"(h a b)", "(h ; second\n a ; first\n b)", "[h ; first\n a ; second\n b]", "(h ; first\n a ; second\n c)", "(h"}) {
        const auto valid = format::validate_output(*original->file, corrupt);
        BOOST_REQUIRE(!valid.has_value());
        BOOST_TEST(valid.error().exit_class == 70);
    }
    cst::Operation limited;
    limited.limits.output_bytes = 5;
    BOOST_TEST(!format::format_source("(h abc)", {}, limited).output.has_value());
    limited = {};
    limited.cancelled = std::make_shared<const std::atomic_bool>(true);
    BOOST_TEST(!format::format_source("(h abc)", {}, limited).output.has_value());
    BOOST_TEST(format::format_source("(h)", {.width = 0}).exit_class == 64);
}

BOOST_AUTO_TEST_CASE(repository_roundtrip_and_generated_idempotence) {
    const auto root = fs::path(ETA_COOKBOOK_DIR).parent_path();
    std::size_t count = 0;
    for (const auto *name : {"stdlib", "cookbook", "demo"}) {
        if (!fs::exists(root / name)) continue;
        for (const auto &entry : fs::recursive_directory_iterator(root / name)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".eta") continue;
            // A working copy with mixed LF/CRLF (e.g. stdlib/std/test.eta under
            // Windows autocrlf; git stores it LF-only) must be refused, not
            // normalized. Decide from the bytes on disk, not the path, so a
            // clean Linux checkout takes the ordinary round-trip below.
            const auto text = read(entry.path());
            const bool has_crlf = text.find("\r\n") != std::string::npos;
            bool has_bare_lf = false;
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) { has_bare_lf = true; break; }
            }
            if (has_crlf && has_bare_lf) {
                const auto refused = format::format_source(text);
                BOOST_TEST(!refused.output.has_value());
                BOOST_TEST(refused.exit_class == 2);
                BOOST_REQUIRE(!refused.diagnostics.empty());
                BOOST_TEST(refused.diagnostics.back().code == "format.LineEndings");
                ++count;
                continue;
            }
            for (auto width : {100u, 40u}) {
                BOOST_TEST_CONTEXT(entry.path().string() << " width=" << width) { checked(text, {.width = width}); }
            }
            ++count;
        }
    }
    BOOST_TEST(count > 100u);
    {
        /// Same refusal, independent of how this checkout stores line endings.
        const auto refused = format::format_source("(a)\r\n(b)\n");
        BOOST_TEST(!refused.output.has_value());
        BOOST_TEST(refused.exit_class == 2);
        BOOST_REQUIRE(!refused.diagnostics.empty());
        BOOST_TEST(refused.diagnostics.back().code == "format.LineEndings");
    }
    std::mt19937 random(0xE7A1);
    for (int i = 0; i < 200; ++i) {
        std::string source = "(unknown ";
        for (unsigned j = 0, n = random() % 20; j < n; ++j) source += j % 3 ? "[x 'y #;(. .) z] " : "(a ; repeated\n b) ";
        source += ')';
        checked(source, {.width = 20 + random() % 81, .indent = 1 + random() % 4});
    }
    for (int i = 0; i < 200; ++i) {
        std::string bytes;
        for (unsigned j = 0, n = random() % 128; j < n; ++j) bytes += static_cast<char>(random() % 256);
        auto result = format::format_source(bytes);
        if (result.output) checked(bytes, result.effective_options);
        else BOOST_TEST(result.exit_class == 2);
    }
}

BOOST_AUTO_TEST_CASE(controlled_runtime_semantics) {
    const std::vector<std::string> programs{
        "(module fmt-let (export result) (begin (define result (let ((x 4) (y 7)) (+ x y)))))",
        "(module fmt-reader (export result) (begin (define result (car '(11 20)))))",
        "(module fmt-cond (export result) (begin (define result (if (= 2 2) 11 0))))",
        "(module fmt-macro (export result) (begin (define-syntax twice (syntax-rules () ((twice x) (+ x x)))) (define result (twice 6))))"
    };
    for (const auto &source : programs) {
        const auto output = checked(source, {.width = 20});
        std::vector<std::int64_t> values;
        for (const auto &text : {source, output}) {
            session::Driver driver(interpreter::ModulePathResolver({fs::path(ETA_STDLIB_DIR)}));
            runtime::nanbox::LispVal result{runtime::nanbox::Nil};
            BOOST_REQUIRE(driver.run_source(text, &result, "result"));
            const auto number = runtime::nanbox::ops::decode<std::int64_t>(result);
            BOOST_REQUIRE(number.has_value());
            values.push_back(*number);
        }
        BOOST_TEST(values[0] == values[1]);
    }
}

BOOST_AUTO_TEST_CASE(atomic_replacement_faults_and_metadata) {
    const auto directory = fs::temp_directory_path() / ("eta-fmt-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(directory);
    struct Cleanup { fs::path directory; ~Cleanup() { std::error_code error; fs::remove_all(directory, error); } } cleanup{directory};
    const auto path = directory / "test.eta";
    write(path, "(h   a)");
    for (auto stage : {tooling::CommitStage::BeforeTemporary, tooling::CommitStage::BeforeFlush, tooling::CommitStage::BeforeReplace}) {
        auto snapshot = tooling::read_snapshot(path, true);
        BOOST_REQUIRE(snapshot.has_value());
        const auto outcome = tooling::replace_atomically(*snapshot, "(h a)", {{}, [stage](auto current) { return current == stage; }});
        BOOST_TEST(!outcome.success);
        BOOST_TEST(static_cast<int>(outcome.outcome) == static_cast<int>(tooling::CommitOutcome::NotCommitted));
        BOOST_TEST(read(path) == "(h   a)");
        BOOST_TEST(std::distance(fs::directory_iterator(directory), fs::directory_iterator{}) == 1);
    }
    auto stale = tooling::read_snapshot(path, true);
    BOOST_REQUIRE(stale.has_value());
    write(path, "(external writer)");
    auto refused = tooling::replace_atomically(*stale, "(h a)");
    BOOST_TEST(!refused.success);
    BOOST_TEST(read(path) == "(external writer)");
    write(path, "(h   a)");
    auto snapshot = tooling::read_snapshot(path, true);
    BOOST_REQUIRE(snapshot.has_value());
    const auto committed = tooling::replace_atomically(*snapshot, "(h a)", {{}, [](auto current) { return current == tooling::CommitStage::AfterReplace; }});
    BOOST_TEST(!committed.success);
    BOOST_TEST(static_cast<int>(committed.outcome) == static_cast<int>(tooling::CommitOutcome::DurabilityUnconfirmed));
    BOOST_TEST(read(path) == "(h a)");
    auto unchanged = tooling::read_snapshot(path, true);
    BOOST_REQUIRE(unchanged.has_value());
    const auto modified = fs::last_write_time(path);
    BOOST_TEST(tooling::replace_atomically(*unchanged, "(h a)").success);
    BOOST_TEST(fs::last_write_time(path) == modified);
    BOOST_TEST(tooling::glob_matches("**/x*.eta", "a/b/x.eta"));
    BOOST_TEST(tooling::glob_matches("**/x*.eta", "x.eta"));
    BOOST_TEST(!tooling::glob_matches("*.eta", "a/x.eta"));
}

BOOST_AUTO_TEST_SUITE_END()
