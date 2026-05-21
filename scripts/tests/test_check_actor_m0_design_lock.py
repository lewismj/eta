import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


SCRIPT_PATH = Path(__file__).resolve().parent / "check_actor_m0_design_lock.py"
SPEC = importlib.util.spec_from_file_location("check_actor_m0_design_lock", SCRIPT_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC is not None and SPEC.loader is not None
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def write_fixture(root: Path):
    for relative_path, markers in MODULE.REQUIRED_MARKERS.items():
        path = root / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("\n".join(markers) + "\n", encoding="utf-8")


class ActorM0DesignLockGuardTests(unittest.TestCase):
    def test_accepts_complete_design_lock_fixture(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_fixture(root)

            issues = MODULE.find_design_lock_issues(root)
            self.assertEqual(issues, [])

    def test_reports_missing_marker_in_existing_file(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_fixture(root)

            language_guide = root / "docs" / "language_guide.md"
            language_guide.write_text(
                "## 24. Concurrency & Distribution\n"
                "legacy socket docs only\n",
                encoding="utf-8",
            )

            issues = MODULE.find_design_lock_issues(root)
            self.assertTrue(
                any(issue.path == Path("docs/language_guide.md")
                    and issue.reason == "missing required marker"
                    for issue in issues))

    def test_reports_missing_required_file(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_fixture(root)

            adr_path = root / "docs" / "adr" / "0001-actors-vm-mailboxes-and-nng-transport.md"
            adr_path.unlink()

            issues = MODULE.find_design_lock_issues(root)
            self.assertTrue(
                any(issue.path == Path("docs/adr/0001-actors-vm-mailboxes-and-nng-transport.md")
                    and issue.reason == "missing required file"
                    for issue in issues))


if __name__ == "__main__":
    unittest.main()
