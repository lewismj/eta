/**
 *
 * Tests exercise the LSP server in-process by injecting std::istringstream /
 * std::ostringstream instead of the real stdin/stdout pipe.  This mirrors
 * the approach used by dap_tests.cpp.
 *
 * Covers Plan items:
 *   2.1  textDocument/documentSymbol
 *   2.2  LSP Keyword Completions Update (raise, catch, logic-var, etc.)
 *   2.3  textDocument/references
 *   2.4  textDocument/rename
 */

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include "eta/lsp/lsp_server.h"
#include "eta/util/json.h"

namespace json = eta::json;

/**
 * Helpers
 */

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

/**
 * Find the first response message matching a given method (via "id" matching
 * is fragile, so we match on id == expected_id).
 */
static json::Value find_response(const std::vector<json::Value>& msgs, int id) {
    for (const auto& m : msgs) {
        auto mid = m.get_int("id");
        if (mid && *mid == id && m.has("result")) return m;
    }
    return {};
}

/**
 * Run the LSP server synchronously with the given framed input and return
 * all parsed output messages.
 */
static std::vector<json::Value> run_server(const std::string& input) {
    std::istringstream in(input);
    std::ostringstream out;
    eta::lsp::LspServer server(in, out);
    server.run();
    return parse_output(out.str());
}

/**
 * Standard initialization + open document preamble, then appends custom
 * requests and shutdown/exit.
 */
static std::string build_input(const std::string& uri,
                                const std::string& source,
                                const std::vector<std::string>& extra_frames) {
    /// Escape source for JSON string embedding
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

/**
 * Test suite
 */

BOOST_AUTO_TEST_SUITE(lsp_protocol)

/**
 * 2.1a  initialize advertises documentSymbolProvider
 */
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

/**
 * 2.1b  initialize advertises referencesProvider
 */
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

/**
 * 2.1c  initialize advertises renameProvider
 */
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

/**
 * 2.1d  documentSymbol returns symbols for define, defun, module
 */
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
    BOOST_REQUIRE_GE(symbols.size(), 3u);  ///< m1, foo, bar

    /// Collect symbol names
    std::vector<std::string> names;
    for (const auto& s : symbols) {
        auto n = s.get_string("name");
        if (n) names.push_back(*n);
    }

    BOOST_CHECK(std::find(names.begin(), names.end(), "m1") != names.end());
    BOOST_CHECK(std::find(names.begin(), names.end(), "foo") != names.end());
    BOOST_CHECK(std::find(names.begin(), names.end(), "bar") != names.end());

    /// Verify kinds
    for (const auto& s : symbols) {
        auto n = s.get_string("name");
        auto k = s.get_int("kind");
        if (n && k) {
            if (*n == "m1")  BOOST_TEST(*k == 2);   ///< Module
            if (*n == "foo") BOOST_TEST(*k == 12);   ///< Function
            if (*n == "bar") BOOST_TEST(*k == 13);   ///< Variable
        }
    }
}

/**
 * 2.1e  documentSymbol returns record types with Class kind
 */
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

/**
 * 2.2a  completion includes raise keyword
 */
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

/**
 * 2.3a  references finds all usages of a symbol
 */
BOOST_AUTO_TEST_CASE(references_finds_all_usages) {
    const std::string uri = "file:///test/refs.eta";
    const std::string src =
        "(module m1\n"
        "  (define x 1)\n"
        "  (define y x)\n"
        "  (define z (+ x y)))\n";

    /// Position on 'x' at line 1, character 10 (inside "(define x 1)")
    auto input = build_input(uri, src, {
        frame(request(10, "textDocument/references",
            R"({"textDocument":{"uri":")" + uri
            + R"("},"position":{"line":1,"character":10},"context":{"includeDeclaration":true}})"))
    });

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 10);
    BOOST_REQUIRE(!resp.is_null());

    const auto& refs = resp["result"].as_array();
    /// x appears in: (define x 1), (define y x), (+ x y) = 3 occurrences
    BOOST_TEST(refs.size() >= 3u);

    /// All references must be in the same file
    for (const auto& r : refs) {
        auto ref_uri = r.get_string("uri");
        BOOST_REQUIRE(ref_uri.has_value());
        BOOST_TEST(*ref_uri == uri);
    }
}

/**
 * 2.3b  references on unknown position returns empty
 */
BOOST_AUTO_TEST_CASE(references_on_whitespace_returns_empty) {
    const std::string uri = "file:///test/refs2.eta";
    const std::string src = "(module m1 (define x 1))\n";

    /// Position on whitespace (past end of content on line 1)
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

/**
 * 2.4a  rename produces correct workspace edit
 */
BOOST_AUTO_TEST_CASE(rename_produces_workspace_edit) {
    const std::string uri = "file:///test/rename.eta";
    const std::string src =
        "(module m1\n"
        "  (define x 1)\n"
        "  (define y x))\n";

    /// Rename 'x' to 'new-x' at line 1, character 10
    auto input = build_input(uri, src, {
        frame(request(10, "textDocument/rename",
            R"({"textDocument":{"uri":")" + uri
            + R"("},"position":{"line":1,"character":10},"newName":"new-x"})"))
    });

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 10);
    BOOST_REQUIRE(!resp.is_null());

    /// Result should be a WorkspaceEdit with changes
    const auto& changes = resp["result"]["changes"];
    BOOST_TEST(!changes.is_null());

    /// The changes should include the file URI
    const auto& file_edits = changes[uri];
    BOOST_TEST(file_edits.is_array());

    const auto& edits = file_edits.as_array();
    /// x appears at: (define x 1) and (define y x)
    BOOST_TEST(edits.size() >= 2u);

    /// All edits should have newText = "new-x"
    for (const auto& edit : edits) {
        auto new_text = edit.get_string("newText");
        BOOST_REQUIRE(new_text.has_value());
        BOOST_TEST(*new_text == "new-x");
    }
}

/**
 * 2.4b  rename with empty newName returns null
 */
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

/**
 * Unit test: collect_symbols detects all definition forms
 */
BOOST_AUTO_TEST_CASE(collect_symbols_all_forms) {
    /// Test collect_symbols directly (it's a static method)
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

    /// Should contain: my-mod, my-func, my-var, my-macro, my-rec
    BOOST_CHECK(std::find(names.begin(), names.end(), "my-mod") != names.end());
    BOOST_CHECK(std::find(names.begin(), names.end(), "my-func") != names.end());
    BOOST_CHECK(std::find(names.begin(), names.end(), "my-var") != names.end());
    BOOST_CHECK(std::find(names.begin(), names.end(), "my-macro") != names.end());
    BOOST_CHECK(std::find(names.begin(), names.end(), "my-rec") != names.end());
}

/**
 * Unit test: find_all_occurrences with word boundaries
 */
BOOST_AUTO_TEST_CASE(find_all_occurrences_word_boundaries) {
    const std::string src =
        "(define x 1)\n"
        "(define xx 2)\n"
        "(+ x xx)\n";

    auto occ = eta::lsp::LspServer::find_all_occurrences(src, "x");

    /**
     * "x" should match the standalone x only, not the x inside "xx"
     * "xx" should NOT match
     */
    BOOST_REQUIRE_EQUAL(occ.size(), 2u);

    BOOST_TEST(occ[0].start.line == 0);
    BOOST_TEST(occ[0].start.character == 8);
    BOOST_TEST(occ[1].start.line == 2);
    BOOST_TEST(occ[1].start.character == 3);
}

/**
 * 3.1  initialize advertises foldingRangeProvider
 */
BOOST_AUTO_TEST_CASE(initialize_advertises_folding_range) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "shutdown"))
      + frame(notification("exit"));

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 1);
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["result"]["capabilities"]["foldingRangeProvider"].as_bool() == true);
}

/**
 * 3.2  initialize advertises selectionRangeProvider
 */
BOOST_AUTO_TEST_CASE(initialize_advertises_selection_range) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "shutdown"))
      + frame(notification("exit"));

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 1);
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["result"]["capabilities"]["selectionRangeProvider"].as_bool() == true);
}

/**
 * 3.3  initialize advertises workspaceSymbolProvider
 */
BOOST_AUTO_TEST_CASE(initialize_advertises_workspace_symbol) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "shutdown"))
      + frame(notification("exit"));

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 1);
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["result"]["capabilities"]["workspaceSymbolProvider"].as_bool() == true);
}

/**
 * 3.4  foldingRange returns ranges for multi-line S-expressions
 */
BOOST_AUTO_TEST_CASE(folding_range_multi_line_sexp) {
    const std::string uri = "file:///test/fold.eta";
    const std::string src =
        "(module m1\n"
        "  (defun foo (x)\n"
        "    (+ x 1))\n"
        "  (define bar 42))\n";

    auto input = build_input(uri, src, {
        frame(request(10, "textDocument/foldingRange",
            R"({"textDocument":{"uri":")" + uri + R"("}})"))
    });

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 10);
    BOOST_REQUIRE(!resp.is_null());

    const auto& ranges = resp["result"].as_array();
    /// At minimum, the outer (module ...) form spans lines 0..3
    BOOST_TEST(!ranges.empty());

    /// Find the outermost fold (starts at line 0)
    bool found_outer = false;
    for (const auto& r : ranges) {
        auto sl = r.get_int("startLine");
        auto el = r.get_int("endLine");
        if (sl && el && *sl == 0 && *el == 3) {
            found_outer = true;
            break;
        }
    }
    BOOST_TEST(found_outer);
}

/**
 * 3.5  foldingRange folds consecutive comment lines
 */
BOOST_AUTO_TEST_CASE(folding_range_comment_block) {
    const std::string uri = "file:///test/fold_comment.eta";
    const std::string src =
        "; This is a\n"
        "; multi-line comment\n"
        "; block\n"
        "(define x 1)\n";

    auto input = build_input(uri, src, {
        frame(request(10, "textDocument/foldingRange",
            R"({"textDocument":{"uri":")" + uri + R"("}})"))
    });

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 10);
    BOOST_REQUIRE(!resp.is_null());

    const auto& ranges = resp["result"].as_array();
    bool found_comment_fold = false;
    for (const auto& r : ranges) {
        auto kind = r.get_string("kind");
        if (kind && *kind == "comment") {
            auto sl = r.get_int("startLine");
            auto el = r.get_int("endLine");
            if (sl && el && *sl == 0 && *el == 2) {
                found_comment_fold = true;
                break;
            }
        }
    }
    BOOST_TEST(found_comment_fold);
}

/**
 * 3.6  workspace/symbol returns symbols from open documents
 */
BOOST_AUTO_TEST_CASE(workspace_symbol_finds_open_doc_symbols) {
    const std::string uri = "file:///test/ws.eta";
    const std::string src =
        "(module ws-test\n"
        "  (defun my-func (x) x)\n"
        "  (define my-var 42))\n";

    auto input = build_input(uri, src, {
        frame(request(10, "workspace/symbol",
            R"({"query":"my-"})"))
    });

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 10);
    BOOST_REQUIRE(!resp.is_null());

    const auto& symbols = resp["result"].as_array();
    /// Should find my-func and my-var (and maybe ws-test module, but that doesn't start with "my-")
    std::vector<std::string> names;
    for (const auto& s : symbols) {
        auto n = s.get_string("name");
        if (n) names.push_back(*n);
    }

    BOOST_CHECK(std::find(names.begin(), names.end(), "my-func") != names.end());
    BOOST_CHECK(std::find(names.begin(), names.end(), "my-var") != names.end());

    /// Each symbol must have a location with the correct URI
    for (const auto& s : symbols) {
        auto n = s.get_string("name");
        if (n && (n->find("my-") == 0)) {
            auto loc_uri = s["location"].get_string("uri");
            BOOST_REQUIRE(loc_uri.has_value());
            BOOST_TEST(*loc_uri == uri);
        }
    }
}

/**
 * 3.7  workspace/symbol with empty query returns symbols
 */
BOOST_AUTO_TEST_CASE(workspace_symbol_empty_query) {
    const std::string uri = "file:///test/ws2.eta";
    const std::string src = "(module m1 (define x 1))\n";

    auto input = build_input(uri, src, {
        frame(request(10, "workspace/symbol", R"({"query":""})"))
    });

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 10);
    BOOST_REQUIRE(!resp.is_null());

    /// With empty query, should return at least the document-local symbols
    const auto& symbols = resp["result"].as_array();
    BOOST_TEST(!symbols.empty());
}

/**
 * 3.8  selectionRange returns nested S-expression ranges
 */
BOOST_AUTO_TEST_CASE(selection_range_nested_sexp) {
    const std::string uri = "file:///test/sel.eta";
    const std::string src =
        "(module m1\n"
        "  (define x (+ 1 2)))\n";

    auto input = build_input(uri, src, {
        frame(request(10, "textDocument/selectionRange",
            R"({"textDocument":{"uri":")" + uri
            + R"("},"positions":[{"line":1,"character":15}]})"))
    });

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 10);
    BOOST_REQUIRE(!resp.is_null());

    const auto& sel_ranges = resp["result"].as_array();
    BOOST_REQUIRE_EQUAL(sel_ranges.size(), 1u);

    /// The innermost range should exist
    const auto& inner = sel_ranges[0];
    BOOST_TEST(inner.has("range"));

    /// There should be a parent (the define form) and a grandparent (the module form)
    BOOST_TEST(inner.has("parent"));
}

/**
 * 3.9  selectionRange on whitespace returns a range without parent
 */
BOOST_AUTO_TEST_CASE(selection_range_on_whitespace) {
    const std::string uri = "file:///test/sel2.eta";
    const std::string src = "(define x 1)\n\n";

    /// Position on the blank line
    auto input = build_input(uri, src, {
        frame(request(10, "textDocument/selectionRange",
            R"({"textDocument":{"uri":")" + uri
            + R"("},"positions":[{"line":1,"character":0}]})"))
    });

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 10);
    BOOST_REQUIRE(!resp.is_null());

    const auto& sel_ranges = resp["result"].as_array();
    BOOST_REQUIRE_EQUAL(sel_ranges.size(), 1u);
    /// Should have a range but no enclosing S-expression parent
    BOOST_TEST(sel_ranges[0].has("range"));
}

/**
 * Unit test: path_to_uri produces valid file:// URIs
 */
BOOST_AUTO_TEST_CASE(path_to_uri_basic) {
    /// Unix-style path
    auto uri = eta::lsp::LspServer::path_to_uri("/tmp/test.eta");
    BOOST_TEST(uri.find("file:///") == 0u);
    BOOST_TEST(uri.find("test.eta") != std::string::npos);
}

/**
 * 4.1  initialize advertises semanticTokensProvider
 */
BOOST_AUTO_TEST_CASE(initialize_advertises_semantic_tokens) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "shutdown"))
      + frame(notification("exit"));

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 1);
    BOOST_REQUIRE(!resp.is_null());
    const auto& stp = resp["result"]["capabilities"]["semanticTokensProvider"];
    BOOST_TEST(!stp.is_null());
    /// Check legend has tokenTypes
    const auto& legend = stp["legend"];
    BOOST_TEST(!legend.is_null());
    const auto& token_types = legend["tokenTypes"];
    BOOST_TEST(token_types.is_array());
    BOOST_TEST(!token_types.as_array().empty());
}

/**
 * 4.2  initialize advertises documentFormattingProvider
 */
BOOST_AUTO_TEST_CASE(initialize_advertises_formatting) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "shutdown"))
      + frame(notification("exit"));

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 1);
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["result"]["capabilities"]["documentFormattingProvider"].as_bool() == true);
}

/**
 * 4.3  semanticTokens/full returns data array
 */
BOOST_AUTO_TEST_CASE(semantic_tokens_returns_data) {
    const std::string uri = "file:///test/semtok.eta";
    const std::string src =
        "(module m1\n"
        "  (defun foo (x) x)\n"
        "  (define bar 42))\n";

    auto input = build_input(uri, src, {
        frame(request(10, "textDocument/semanticTokens/full",
            R"({"textDocument":{"uri":")" + uri + R"("}})"))
    });

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 10);
    BOOST_REQUIRE(!resp.is_null());

    const auto& data = resp["result"]["data"];
    BOOST_TEST(data.is_array());
    /// Data should contain token entries (each 5 ints)
    BOOST_TEST(data.as_array().size() >= 5u);
    /// Data length must be a multiple of 5
    BOOST_TEST(data.as_array().size() % 5 == 0u);
}

/**
 * 4.4  semanticTokens classifies keywords correctly
 */
BOOST_AUTO_TEST_CASE(semantic_tokens_classifies_keywords) {
    const std::string uri = "file:///test/semtok2.eta";
    const std::string src = "(define x 42)\n";

    auto input = build_input(uri, src, {
        frame(request(10, "textDocument/semanticTokens/full",
            R"({"textDocument":{"uri":")" + uri + R"("}})"))
    });

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 10);
    BOOST_REQUIRE(!resp.is_null());

    const auto& data = resp["result"]["data"].as_array();
    BOOST_REQUIRE(data.size() >= 5u);
    /**
     * First token should be "define" which is keyword (type 0)
     * Token format: deltaLine, deltaCol, length, tokenType, tokenModifiers
     */
    auto first_type = data[3].as_int();
    BOOST_TEST(first_type == 0);  ///< 0 = keyword
}

/**
 * 4.5  semanticTokens on empty document returns empty data
 */
BOOST_AUTO_TEST_CASE(semantic_tokens_empty_doc) {
    const std::string uri = "file:///test/semtok_empty.eta";
    const std::string src = "";

    auto input = build_input(uri, src, {
        frame(request(10, "textDocument/semanticTokens/full",
            R"({"textDocument":{"uri":")" + uri + R"("}})"))
    });

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 10);
    BOOST_REQUIRE(!resp.is_null());

    const auto& data = resp["result"]["data"];
    BOOST_TEST(data.is_array());
    BOOST_TEST(data.as_array().empty());
}

/**
 * 4.6  formatting indents nested S-expressions
 */
BOOST_AUTO_TEST_CASE(formatting_indents_nested) {
    const std::string uri = "file:///test/fmt.eta";
    /// Badly indented source
    const std::string src =
        "(module m1\n"
        "(defun foo (x)\n"
        "(+ x 1))\n"
        "(define bar 42))\n";

    auto input = build_input(uri, src, {
        frame(request(10, "textDocument/formatting",
            R"({"textDocument":{"uri":")" + uri + R"("},"options":{"tabSize":2,"insertSpaces":true}})"))
    });

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 10);
    BOOST_REQUIRE(!resp.is_null());

    const auto& edits = resp["result"].as_array();
    BOOST_REQUIRE(!edits.empty());

    /// Check that the replacement text exists
    auto new_text = edits[0].get_string("newText");
    BOOST_REQUIRE(new_text.has_value());

    /// The formatted output should have indentation
    BOOST_TEST(new_text->find("  (defun") != std::string::npos);
    BOOST_TEST(new_text->find("    (+ x 1)") != std::string::npos);
}

/**
 * 4.7  formatting preserves empty document
 */
BOOST_AUTO_TEST_CASE(formatting_empty_doc) {
    const std::string uri = "file:///test/fmt_empty.eta";
    const std::string src = "";

    auto input = build_input(uri, src, {
        frame(request(10, "textDocument/formatting",
            R"({"textDocument":{"uri":")" + uri + R"("},"options":{"tabSize":2,"insertSpaces":true}})"))
    });

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 10);
    BOOST_REQUIRE(!resp.is_null());

    const auto& edits = resp["result"].as_array();
    BOOST_TEST(edits.empty());
}

/**
 * 4.8  formatting with custom tab size
 */
BOOST_AUTO_TEST_CASE(formatting_custom_tab_size) {
    const std::string uri = "file:///test/fmt4.eta";
    const std::string src =
        "(module m1\n"
        "(define x 1))\n";

    auto input = build_input(uri, src, {
        frame(request(10, "textDocument/formatting",
            R"({"textDocument":{"uri":")" + uri + R"("},"options":{"tabSize":4,"insertSpaces":true}})"))
    });

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 10);
    BOOST_REQUIRE(!resp.is_null());

    const auto& edits = resp["result"].as_array();
    BOOST_REQUIRE(!edits.empty());

    auto new_text = edits[0].get_string("newText");
    BOOST_REQUIRE(new_text.has_value());

    /// With tab size 4, inner form should be indented 4 spaces
    BOOST_TEST(new_text->find("    (define x 1)") != std::string::npos);
}

/**
 * 4.9  references across open documents
 */
BOOST_AUTO_TEST_CASE(references_cross_file) {
    /**
     * We test that references in one doc find occurrences
     * by verifying the result includes the current document matches
     */
    const std::string uri = "file:///test/refs_cross.eta";
    const std::string src =
        "(module m1\n"
        "  (define x 1)\n"
        "  (define y x)\n"
        "  (define z (+ x y)))\n";

    /// Position on 'x' at line 1, character 10
    auto input = build_input(uri, src, {
        frame(request(10, "textDocument/references",
            R"({"textDocument":{"uri":")" + uri
            + R"("},"position":{"line":1,"character":10},"context":{"includeDeclaration":true}})"))
    });

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 10);
    BOOST_REQUIRE(!resp.is_null());

    const auto& refs = resp["result"].as_array();
    /// x appears 3 times in the current document
    BOOST_TEST(refs.size() >= 3u);
}

/**
 * 4.10  semanticTokens classifies strings and numbers
 */
BOOST_AUTO_TEST_CASE(semantic_tokens_strings_and_numbers) {
    const std::string uri = "file:///test/semtok3.eta";
    const std::string src = R"((define msg "hello") (define n 42))";

    auto input = build_input(uri, src, {
        frame(request(10, "textDocument/semanticTokens/full",
            R"({"textDocument":{"uri":")" + uri + R"("}})"))
    });

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 10);
    BOOST_REQUIRE(!resp.is_null());

    const auto& data = resp["result"]["data"].as_array();
    /// Should have multiple tokens including string (type 3) and number (type 4)
    bool found_string = false;
    bool found_number = false;
    for (std::size_t i = 3; i < data.size(); i += 5) {
        auto type = data[i].as_int();
        if (type == 3) found_string = true;
        if (type == 4) found_number = true;
    }
    BOOST_TEST(found_string);
    BOOST_TEST(found_number);
}

/**
 * 4.11  formatting trims trailing whitespace
 */
BOOST_AUTO_TEST_CASE(formatting_trims_trailing) {
    const std::string uri = "file:///test/fmt_trail.eta";
    const std::string src = "(define x 1)   \n";

    auto input = build_input(uri, src, {
        frame(request(10, "textDocument/formatting",
            R"({"textDocument":{"uri":")" + uri + R"("},"options":{"tabSize":2,"insertSpaces":true}})"))
    });

    auto msgs = run_server(input);
    auto resp = find_response(msgs, 10);
    BOOST_REQUIRE(!resp.is_null());

    const auto& edits = resp["result"].as_array();
    BOOST_REQUIRE(!edits.empty());

    auto new_text = edits[0].get_string("newText");
    BOOST_REQUIRE(new_text.has_value());

    /// Should not have trailing spaces
    auto first_newline = new_text->find('\n');
    BOOST_REQUIRE(first_newline != std::string::npos);
    if (first_newline > 0) {
        BOOST_TEST((*new_text)[first_newline - 1] != ' ');
    }
}

/**
 * Unit test: enclosing_sexp_ranges finds nested ranges
 */
BOOST_AUTO_TEST_CASE(enclosing_sexp_ranges_nested) {
    const std::string src = "(a (b (c)))";
    /// Position on 'c' at line 0, col 7
    auto ranges = eta::lsp::LspServer::enclosing_sexp_ranges(src, 0, 7);

    /// Should find 3 ranges: (c), (b (c)), (a (b (c)))
    BOOST_REQUIRE_EQUAL(ranges.size(), 3u);

    /// Innermost should be the smallest
    BOOST_TEST(ranges[0].start.character == 6);  ///< '(' of (c) at index 6
    BOOST_TEST(ranges[0].end.character == 9);     ///< ')' of (c) at index 8, +1 = 9
}

BOOST_AUTO_TEST_SUITE_END()


BOOST_AUTO_TEST_SUITE(lsp_framing_robustness)

/// A non-numeric Content-Length must not throw; server exits cleanly.
BOOST_AUTO_TEST_CASE(malformed_content_length_does_not_throw) {
    std::string input = "Content-Length: not-a-number\r\n\r\n{}";
    BOOST_CHECK_NO_THROW(run_server(input));
}

/// An empty Content-Length value must not throw.
BOOST_AUTO_TEST_CASE(empty_content_length_does_not_throw) {
    std::string input = "Content-Length: \r\n\r\n{}";
    BOOST_CHECK_NO_THROW(run_server(input));
}

/**
 * A Content-Length that exceeds MAX_MESSAGE_SIZE must not allocate / crash.
 * We use 4 GiB (well above the 64 MiB cap) with no actual body bytes following.
 */
BOOST_AUTO_TEST_CASE(overlimit_content_length_does_not_crash) {
    std::string input = "Content-Length: 4294967295\r\n\r\n";
    BOOST_CHECK_NO_THROW(run_server(input));
}

/// Zero Content-Length silently returns nullopt (existing behaviour, regression guard).
BOOST_AUTO_TEST_CASE(zero_content_length_no_throw) {
    std::string input = "Content-Length: 0\r\n\r\n";
    BOOST_CHECK_NO_THROW(run_server(input));
}

/**
 * A valid message following a malformed one is NOT processed (server stops after
 * the first nullopt from read_message), but no exception must escape.
 */
BOOST_AUTO_TEST_CASE(malformed_then_valid_no_throw) {
    std::string body = R"({"jsonrpc":"2.0","id":1,"method":"shutdown","params":{}})";
    std::string input = "Content-Length: bad\r\n\r\n"
                      + frame(body);
    BOOST_CHECK_NO_THROW(run_server(input));
}

BOOST_AUTO_TEST_SUITE_END() ///< lsp_framing_robustness



