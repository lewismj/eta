// lsp_tests.cpp — LSP protocol tests for LspServer
//
// Tests exercise the LSP server in-process by injecting std::istringstream /
// std::ostringstream instead of the real stdin/stdout pipe.  This mirrors
// the approach used by dap_tests.cpp.
//
// Covers Plan items:
//   2.1  textDocument/documentSymbol
//   2.2  LSP Keyword Completions Update (raise, catch, logic-var, etc.)
//   2.3  textDocument/references
//   2.4  textDocument/rename

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include "eta/lsp/lsp_server.h"
#include "eta/util/json.h"

namespace json = eta::json;

// ============================================================================
// Helpers
// ============================================================================

/// Wrap a JSON body in the Content-Length frame used by the LSP wire protocol.
static std::string frame(const std::string& body) {
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

/// Build a minimal LSP request JSON string.
static std::string request(int id, const std::string& method,
                            const std::string& params = "{}") {
    return R"({"jsonrpc":"2.0","id":)" + std::to_string(id)
         + R"(,"method":")" + method + '"'
         + R"(,"params":)" + params + '}';
}

/// Build an LSP notification (no id).
static std::string notification(const std::string& method,
                                 const std::string& params = "{}") {
    return R"({"jsonrpc":"2.0","method":")" + method + '"'
         + R"(,"params":)" + params + '}';
}

/// Parse all Content-Length-framed LSP messages out of the raw output bytes.
static std::vector<json::Value> parse_output(const std::string& raw) {
    std::vector<json::Value> msgs;
    std::istringstream ss(raw);
    while (true) {
        std::size_t len = 0;
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) break;
            const std::string pfx = "Content-Length: ";
            if (line.substr(0, pfx.size()) == pfx)
                len = std::stoull(line.substr(pfx.size()));
        }
        if (ss.eof() || ss.fail() || len == 0) break;
        std::string body(len, '\0');
        ss.read(body.data(), static_cast<std::streamsize>(len));
        if (ss.fail()) break;
        msgs.push_back(json::parse(body));
    }
    return msgs;
}

/// Find the first response message matching a given method (via "id" matching
/// is fragile, so we match on id == expected_id).
static json::Value find_response(const std::vector<json::Value>& msgs, int id) {
    for (const auto& m : msgs) {
        auto mid = m.get_int("id");
        if (mid && *mid == id && m.has("result")) return m;
    }
    return {};
}

/// Run the LSP server synchronously with the given framed input and return
/// all parsed output messages.
static std::vector<json::Value> run_server(const std::string& input) {
    std::istringstream in(input);
    std::ostringstream out;
    eta::lsp::LspServer server(in, out);
    server.run();
    return parse_output(out.str());
}

/// Standard initialization + open document preamble, then appends custom
/// requests and shutdown/exit.
static std::string build_input(const std::string& uri,
                                const std::string& source,
                                const std::vector<std::string>& extra_frames) {
    // Escape source for JSON string embedding
    std::string escaped;
    for (char c : source) {
        if (c == '"') escaped += "\\\"";
        else if (c == '\\') escaped += "\\\\";
        else if (c == '\n') escaped += "\\n";
        else if (c == '\r') escaped += "\\r";
        else if (c == '\t') escaped += "\\t";
        else escaped += c;
    }

    std::string result;
    result += frame(request(1, "initialize", "{}"));
    result += frame(notification("initialized"));
    result += frame(notification("textDocument/didOpen",
        R"({"textDocument":{"uri":")" + uri
        + R"(","languageId":"eta","version":1,"text":")" + escaped + R"("}})"));

    for (const auto& f : extra_frames) {
        result += f;
    }

    result += frame(request(99, "shutdown"));
    result += frame(notification("exit"));
    return result;
}

// ============================================================================
// Test suite
// ============================================================================

BOOST_AUTO_TEST_SUITE(lsp_protocol)

// ---------------------------------------------------------------------------
// 2.1a  initialize advertises documentSymbolProvider
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(initialize_advertises_document_symbol) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "shutdown"))
      + frame(notification("exit"));

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 1);
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["result"]["capabilities"]["documentSymbolProvider"].as_bool() == true);
}

// ---------------------------------------------------------------------------
// 2.1b  initialize advertises referencesProvider
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(initialize_advertises_references) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "shutdown"))
      + frame(notification("exit"));

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 1);
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["result"]["capabilities"]["referencesProvider"].as_bool() == true);
}

// ---------------------------------------------------------------------------
// 2.1c  initialize advertises renameProvider
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(initialize_advertises_rename) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "shutdown"))
      + frame(notification("exit"));

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 1);
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["result"]["capabilities"]["renameProvider"].as_bool() == true);
}

// ---------------------------------------------------------------------------
// 2.1d  documentSymbol returns symbols for define, defun, module
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(document_symbol_returns_symbols) {
    const std::string uri = "file:///test/symbols.eta";
    const std::string src =
        "(module m1\n"
        "  (defun foo (x) x)\n"
        "  (define bar 42))\n";

    auto input = build_input(uri, src, {
        frame(request(10, "textDocument/documentSymbol",
            R"({"textDocument":{"uri":")" + uri + R"("}})"))
    });

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 10);
    BOOST_REQUIRE(!resp.is_null());

    const auto& symbols = resp["result"].as_array();
    BOOST_REQUIRE_GE(symbols.size(), 3u);  // m1, foo, bar

    // Collect symbol names
    std::vector<std::string> names;
    for (const auto& s : symbols) {
        auto n = s.get_string("name");
        if (n) names.push_back(*n);
    }

    BOOST_CHECK(std::find(names.begin(), names.end(), "m1") != names.end());
    BOOST_CHECK(std::find(names.begin(), names.end(), "foo") != names.end());
    BOOST_CHECK(std::find(names.begin(), names.end(), "bar") != names.end());

    // Verify kinds
    for (const auto& s : symbols) {
        auto n = s.get_string("name");
        auto k = s.get_int("kind");
        if (n && k) {
            if (*n == "m1")  BOOST_TEST(*k == 2);   // Module
            if (*n == "foo") BOOST_TEST(*k == 12);   // Function
            if (*n == "bar") BOOST_TEST(*k == 13);   // Variable
        }
    }
}

// ---------------------------------------------------------------------------
// 2.1e  documentSymbol returns record types with Class kind
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(document_symbol_record_type) {
    const std::string uri = "file:///test/record.eta";
    const std::string src =
        "(module m1\n"
        "  (define-record-type point (make-point x y) point? (x point-x) (y point-y)))\n";

    auto input = build_input(uri, src, {
        frame(request(10, "textDocument/documentSymbol",
            R"({"textDocument":{"uri":")" + uri + R"("}})"))
    });

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 10);
    BOOST_REQUIRE(!resp.is_null());

    const auto& symbols = resp["result"].as_array();
    bool found_record = false;
    for (const auto& s : symbols) {
        auto n = s.get_string("name");
        auto k = s.get_int("kind");
        if (n && *n == "point" && k && *k == 5) {
            found_record = true;
            break;
        }
    }
    BOOST_TEST(found_record);
}

// ---------------------------------------------------------------------------
// 2.2a  completion includes raise keyword
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(completion_includes_raise) {
    const std::string uri = "file:///test/comp.eta";
    const std::string src = "(module m1 (define x 1))\n";

    auto input = build_input(uri, src, {
        frame(request(10, "textDocument/completion",
            R"({"textDocument":{"uri":")" + uri
            + R"("},"position":{"line":0,"character":1}})"))
    });

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 10);
    BOOST_REQUIRE(!resp.is_null());

    const auto& items = resp["result"]["items"].as_array();
    bool found_raise = false;
    bool found_catch = false;
    bool found_logic_var = false;
    bool found_unify = false;
    bool found_copy_term = false;
    for (const auto& item : items) {
        auto label = item.get_string("label");
        if (!label) continue;
        if (*label == "raise") found_raise = true;
        if (*label == "catch") found_catch = true;
        if (*label == "logic-var") found_logic_var = true;
        if (*label == "unify") found_unify = true;
        if (*label == "copy-term") found_copy_term = true;
    }
    BOOST_TEST(found_raise);
    BOOST_TEST(found_catch);
    BOOST_TEST(found_logic_var);
    BOOST_TEST(found_unify);
    BOOST_TEST(found_copy_term);
}

// ---------------------------------------------------------------------------
// 2.3a  references finds all usages of a symbol
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(references_finds_all_usages) {
    const std::string uri = "file:///test/refs.eta";
    const std::string src =
        "(module m1\n"
        "  (define x 1)\n"
        "  (define y x)\n"
        "  (define z (+ x y)))\n";

    // Position on 'x' at line 1, character 10 (inside "(define x 1)")
    auto input = build_input(uri, src, {
        frame(request(10, "textDocument/references",
            R"({"textDocument":{"uri":")" + uri
            + R"("},"position":{"line":1,"character":10},"context":{"includeDeclaration":true}})"))
    });

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 10);
    BOOST_REQUIRE(!resp.is_null());

    const auto& refs = resp["result"].as_array();
    // x appears in: (define x 1), (define y x), (+ x y) = 3 occurrences
    BOOST_TEST(refs.size() >= 3u);

    // All references must be in the same file
    for (const auto& r : refs) {
        auto ref_uri = r.get_string("uri");
        BOOST_REQUIRE(ref_uri.has_value());
        BOOST_TEST(*ref_uri == uri);
    }
}

// ---------------------------------------------------------------------------
// 2.3b  references on unknown position returns empty
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(references_on_whitespace_returns_empty) {
    const std::string uri = "file:///test/refs2.eta";
    const std::string src = "(module m1 (define x 1))\n";

    // Position on whitespace (past end of content on line 1)
    auto input = build_input(uri, src, {
        frame(request(10, "textDocument/references",
            R"({"textDocument":{"uri":")" + uri
            + R"("},"position":{"line":1,"character":0},"context":{"includeDeclaration":true}})"))
    });

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 10);
    BOOST_REQUIRE(!resp.is_null());

    const auto& refs = resp["result"].as_array();
    BOOST_TEST(refs.empty());
}

// ---------------------------------------------------------------------------
// 2.4a  rename produces correct workspace edit
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(rename_produces_workspace_edit) {
    const std::string uri = "file:///test/rename.eta";
    const std::string src =
        "(module m1\n"
        "  (define x 1)\n"
        "  (define y x))\n";

    // Rename 'x' to 'new-x' at line 1, character 10
    auto input = build_input(uri, src, {
        frame(request(10, "textDocument/rename",
            R"({"textDocument":{"uri":")" + uri
            + R"("},"position":{"line":1,"character":10},"newName":"new-x"})"))
    });

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 10);
    BOOST_REQUIRE(!resp.is_null());

    // Result should be a WorkspaceEdit with changes
    const auto& changes = resp["result"]["changes"];
    BOOST_TEST(!changes.is_null());

    // The changes should include the file URI
    const auto& file_edits = changes[uri];
    BOOST_TEST(file_edits.is_array());

    const auto& edits = file_edits.as_array();
    // x appears at: (define x 1) and (define y x)
    BOOST_TEST(edits.size() >= 2u);

    // All edits should have newText = "new-x"
    for (const auto& edit : edits) {
        auto new_text = edit.get_string("newText");
        BOOST_REQUIRE(new_text.has_value());
        BOOST_TEST(*new_text == "new-x");
    }
}

// ---------------------------------------------------------------------------
// 2.4b  rename with empty newName returns null
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(rename_empty_name_returns_null) {
    const std::string uri = "file:///test/rename2.eta";
    const std::string src = "(module m1 (define x 1))\n";

    auto input = build_input(uri, src, {
        frame(request(10, "textDocument/rename",
            R"({"textDocument":{"uri":")" + uri
            + R"("},"position":{"line":0,"character":19},"newName":""})"))
    });

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 10);
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["result"].is_null());
}

// ---------------------------------------------------------------------------
// Unit test: collect_symbols detects all definition forms
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(collect_symbols_all_forms) {
    // Test collect_symbols directly (it's a static method)
    const std::string src =
        "(module my-mod\n"
        "  (defun my-func (a b) (+ a b))\n"
        "  (define my-var 42)\n"
        "  (define-syntax my-macro (syntax-rules () ((my-macro x) x)))\n"
        "  (define-record-type my-rec (make-my-rec f) my-rec? (f my-rec-f)))\n";

    auto symbols = eta::lsp::LspServer::collect_symbols(src, true);

    std::vector<std::string> names;
    std::vector<std::string> kinds;
    for (const auto& s : symbols) {
        names.push_back(s.name);
        kinds.push_back(s.kind);
    }

    // Should contain: my-mod, my-func, my-var, my-macro, my-rec
    BOOST_CHECK(std::find(names.begin(), names.end(), "my-mod") != names.end());
    BOOST_CHECK(std::find(names.begin(), names.end(), "my-func") != names.end());
    BOOST_CHECK(std::find(names.begin(), names.end(), "my-var") != names.end());
    BOOST_CHECK(std::find(names.begin(), names.end(), "my-macro") != names.end());
    BOOST_CHECK(std::find(names.begin(), names.end(), "my-rec") != names.end());
}

// ---------------------------------------------------------------------------
// Unit test: find_all_occurrences with word boundaries
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(find_all_occurrences_word_boundaries) {
    const std::string src =
        "(define x 1)\n"
        "(define xx 2)\n"
        "(+ x xx)\n";

    auto occ = eta::lsp::LspServer::find_all_occurrences(src, "x");

    // "x" should match the standalone x only, not the x inside "xx"
    // Line 0: (define x 1) → position 8
    // Line 2: (+ x xx) → position 3 (standalone x)
    // "xx" should NOT match
    BOOST_REQUIRE_EQUAL(occ.size(), 2u);

    BOOST_TEST(occ[0].start.line == 0);
    BOOST_TEST(occ[0].start.character == 8);
    BOOST_TEST(occ[1].start.line == 2);
    BOOST_TEST(occ[1].start.character == 3);
}

BOOST_AUTO_TEST_SUITE_END()

