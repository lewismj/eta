import copy
import importlib.util
import json
import pathlib
import tempfile
import unittest

MODULE_PATH = pathlib.Path(__file__).resolve().parents[1] / "check_lint_rule_specs.py"
MODULE_SPEC = importlib.util.spec_from_file_location("lint_specs", MODULE_PATH)
lint_specs = importlib.util.module_from_spec(MODULE_SPEC)
MODULE_SPEC.loader.exec_module(lint_specs)


class RuleSpecsTest(unittest.TestCase):
    def setUp(self):
        self.spec = dict(code="ETA301", name="UnusedLetBinding", owner="eta_lint", status="gated", severity="warn",
                         rationale="Report only proven unused bindings.", prerequisites=["verified-let-scopes"], metadata=["binding-identities"],
                         fix="none", options={}, presets=[])
        self.spec.update({"analysis-level": "scope", "unknown-behaviour": "skip-affected-scope", "false-positives": [],
                          "fix-safety": "Initializers can have effects.", "positive-fixtures": [], "negative-fixtures": []})
        self.directory = tempfile.TemporaryDirectory()
        self.fixtures = pathlib.Path(self.directory.name)
        self.addCleanup(self.directory.cleanup)

    def test_valid_gated_schema(self):
        lint_specs.validate_spec(self.spec, self.fixtures)

    def test_rejects_missing_types_unknown_fields_and_options(self):
        changes = [{"severity": "warning"}, {"presets": ["recommended"]}, {"owner": "compiler"}, {"bogus": True},
                   {"options": {"limit": {"type": "integer", "default": True}}},
                   {"options": {"limit": {"type": "integer", "default": 1, "minimum": 2}}},
                   {"prerequisites": ["same", "same"]}, {"code": "ETA000"}]
        for change in changes:
            with self.subTest(change=change), self.assertRaises(ValueError):
                lint_specs.validate_spec(self.spec | change, self.fixtures)
        for field in self.spec:
            spec = copy.deepcopy(self.spec)
            del spec[field]
            with self.subTest(field=field), self.assertRaises(ValueError):
                lint_specs.validate_spec(spec, self.fixtures)

    def test_enabled_gate_and_exact_fixtures(self):
        self.spec["status"] = "enabled"
        for field in ("positive-fixtures", "negative-fixtures", "dependency-fixtures", "unknown-fixtures"):
            name = field.removesuffix("-fixtures")
            self.spec[field] = [name]
            (self.fixtures / f"{name}.eta").write_text("x", encoding="utf-8")
            expected = {"diagnostics": [{"code": "ETA301", "span": [0, 1]}] if name == "positive" else [], "coverage": ["unknown"]}
            (self.fixtures / f"{name}.json").write_text(json.dumps(expected), encoding="utf-8")
        with self.assertRaises(ValueError):
            lint_specs.validate_spec(self.spec, self.fixtures)
        self.spec["verified-prerequisites"] = self.spec["prerequisites"]
        lint_specs.validate_spec(self.spec, self.fixtures)
        (self.fixtures / "positive.json").write_text('{"diagnostics":[{"code":"ETA301","span":[0,2]}]}', encoding="utf-8")
        with self.assertRaises(ValueError):
            lint_specs.validate_spec(self.spec, self.fixtures)

    def test_duplicate_rule_code(self):
        content = "\n".join(f"{key} = {json.dumps(value)}" for key, value in self.spec.items()) + "\n"
        (self.fixtures / "ETA301.toml").write_text(content, encoding="utf-8")
        (self.fixtures / "ETA302.toml").write_text(content, encoding="utf-8")
        with self.assertRaises(ValueError):
            lint_specs.inventory(self.fixtures, self.fixtures)

    def test_missing_fixtures_and_fix_gate(self):
        with self.assertRaises(ValueError):
            lint_specs.validate_spec(self.spec | {"positive-fixtures": ["missing"]}, self.fixtures)
        with self.assertRaises(ValueError):
            lint_specs.validate_spec(self.spec | {"status": "enabled", "fix": "safe"}, self.fixtures)

    def test_typed_options(self):
        options = {"depth": {"type": "integer", "default": 5, "minimum": 1, "maximum": 20},
                   "mode": {"type": "string", "default": "local", "enum": ["local", "global"]}}
        lint_specs.validate_spec(self.spec | {"options": options}, self.fixtures)


if __name__ == "__main__":
    unittest.main()
