import importlib.util
from pathlib import Path
import sys
import unittest


SCRIPT_PATH = Path(__file__).resolve().parents[1] / "build_stdlib_docs.py"
SPEC = importlib.util.spec_from_file_location("build_stdlib_docs", SCRIPT_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC is not None and SPEC.loader is not None
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class StdlibDocExtractorTests(unittest.TestCase):
    def test_stdlib_doc_extractor_parses_doc_block_tags(self):
        source = """
(module std.sample
  (begin
    ;;@doc (sample:foo x)
    ;;@category Samples
    ;;@module std.sample
    ;;@alias-of sample:bar
    ;;@since 1.2.3
    ;;@deprecated use sample:baz instead
    ;; Return the same value.
    (defun sample:foo (x) x)))
"""
        docs, diagnostics = MODULE.extract_docs_from_text(source, Path("sample.eta"))
        self.assertEqual(diagnostics, [])
        self.assertEqual(len(docs), 1)

        entry = docs[0]
        self.assertEqual(entry.name, "sample:foo")
        self.assertEqual(entry.signature, "(sample:foo x)")
        self.assertEqual(entry.category, "Samples")
        self.assertEqual(entry.module, "std.sample")
        self.assertEqual(entry.alias_of, "sample:bar")
        self.assertEqual(entry.since, "1.2.3")
        self.assertEqual(entry.deprecated, "use sample:baz instead")
        self.assertEqual(entry.summary, "Return the same value.")

    def test_stdlib_doc_extractor_parses_examples(self):
        source = """
(module std.sample
  (begin
    ;;@doc (sample:sum xs)
    ;; Sum all numbers in xs.
    ;;@example
    ;; (sample:sum '(1 2 3))
    ;;@example
    ;; (sample:sum '(10 20))
    (defun sample:sum (xs) xs)))
"""
        docs, diagnostics = MODULE.extract_docs_from_text(source, Path("sample.eta"))
        self.assertEqual(diagnostics, [])
        self.assertEqual(len(docs), 1)
        entry = docs[0]

        self.assertEqual(len(entry.examples), 2)
        self.assertIn("(sample:sum '(1 2 3))", entry.details)
        self.assertIn("(sample:sum '(10 20))", entry.details)
        self.assertIn("```scheme", entry.details)

    def test_stdlib_doc_extractor_rejects_ambiguous_or_detached_blocks(self):
        ambiguous_source = """
(module std.sample
  (begin
    ;;@doc (sample:a)
    ;;@doc (sample:b)
    (defun sample:a () 1)))
"""
        _, ambiguous_diags = MODULE.extract_docs_from_text(
            ambiguous_source, Path("ambiguous.eta"))
        self.assertTrue(ambiguous_diags)
        self.assertTrue(any("multiple ;;@doc" in d.message for d in ambiguous_diags))

        detached_source = """
(module std.sample
  (begin
    ;;@doc (sample:a)
    ;;

    (defun sample:a () 1)))
"""
        _, detached_diags = MODULE.extract_docs_from_text(
            detached_source, Path("detached.eta"))
        self.assertTrue(detached_diags)
        self.assertTrue(any("blank line" in d.message for d in detached_diags))


if __name__ == "__main__":
    unittest.main()
