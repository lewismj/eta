import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


SCRIPT_PATH = Path(__file__).resolve().parent / "check_legacy_builtin_names_symbol.py"
SPEC = importlib.util.spec_from_file_location("check_legacy_builtin_names_symbol", SCRIPT_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC is not None and SPEC.loader is not None
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class LegacyBuiltinNameSymbolGuardTests(unittest.TestCase):
    def test_detects_legacy_symbol_in_cpp_sources(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            source = root / "sample.cpp"
            source.write_text(
                "#include <eta/runtime/builtin_env.h>\n"
                "void f(eta::runtime::BuiltinEnvironment& env) {\n"
                "    register_builtin_names(env);\n"
                "}\n",
                encoding="utf-8",
            )

            usages = MODULE.find_symbol_usages(root)
            self.assertEqual(len(usages), 1)
            usage = usages[0]
            self.assertEqual(usage.path, source)
            self.assertEqual(usage.line_number, 3)

    def test_ignores_non_source_files_and_skipped_directories(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "docs").mkdir()
            (root / "out").mkdir()
            (root / "docs" / "note.md").write_text(
                "register_builtin_names(env)\n",
                encoding="utf-8",
            )
            (root / "out" / "generated.cpp").write_text(
                "register_builtin_names(env)\n",
                encoding="utf-8",
            )
            (root / "clean.cpp").write_text(
                "void f() {}\n",
                encoding="utf-8",
            )

            usages = MODULE.find_symbol_usages(root)
            self.assertEqual(usages, [])


if __name__ == "__main__":
    unittest.main()
