import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


SCRIPT_PATH = Path(__file__).resolve().parent / "check_builtin_catalog_docs.py"
SPEC = importlib.util.spec_from_file_location("check_builtin_catalog_docs", SCRIPT_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC is not None and SPEC.loader is not None
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class BuiltinCatalogDocsGuardTests(unittest.TestCase):
    def test_detects_legacy_reference_outside_plan_docs(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "docs" / "guide").mkdir(parents=True)
            (root / "docs" / "architecture.md").write_text(
                "builtin_catalog.h\n"
                "single source of truth\n"
                "register_builtin_specs(...)\n",
                encoding="utf-8",
            )
            stale_doc = root / "docs" / "guide" / "runtime.md"
            stale_doc.write_text("The old table lived in builtin_names.h.\n", encoding="utf-8")

            refs = MODULE.find_legacy_doc_references(root)
            self.assertEqual(len(refs), 1)
            self.assertEqual(refs[0].path, stale_doc)
            self.assertEqual(refs[0].pattern_name, "builtin_names.h")

    def test_ignores_plan_docs_for_legacy_reference_scan(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "docs" / "plan").mkdir(parents=True)
            (root / "docs" / "architecture.md").write_text(
                "builtin_catalog.h\n"
                "single source of truth\n"
                "register_builtin_specs(...)\n",
                encoding="utf-8",
            )
            (root / "docs" / "plan" / "old_plan.md").write_text(
                "historic note: builtin_names.h\n",
                encoding="utf-8",
            )

            refs = MODULE.find_legacy_doc_references(root)
            self.assertEqual(refs, [])

    def test_reports_missing_architecture_contract_markers(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "docs").mkdir(parents=True)
            (root / "docs" / "architecture.md").write_text(
                "# Architecture\n",
                encoding="utf-8",
            )

            issues = MODULE.architecture_doc_issues(root)
            self.assertEqual(len(issues), 3)
            self.assertTrue(any("builtin_catalog.h reference" in issue for issue in issues))
            self.assertTrue(any("single source of truth statement" in issue for issue in issues))
            self.assertTrue(any("register_builtin_specs(...) reference" in issue for issue in issues))

    def test_accepts_architecture_doc_with_catalog_contract(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "docs").mkdir(parents=True)
            (root / "docs" / "architecture.md").write_text(
                "builtin_catalog.h is the single source of truth.\n"
                "Semantic bootstrap uses register_builtin_specs(...).\n",
                encoding="utf-8",
            )

            issues = MODULE.architecture_doc_issues(root)
            self.assertEqual(issues, [])


if __name__ == "__main__":
    unittest.main()
