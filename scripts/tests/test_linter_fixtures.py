"""Run the built eta_lint binary against every rule fixture and compare findings.

For each enabled rule specification, the fixtures under each category are linted
in isolation (``--rules <CODE>``). Positive fixtures must reproduce the exact
rule findings and byte spans; negative, dependency and unknown fixtures must
produce none. Dependency and unknown fixtures additionally assert that the
linter records analysis coverage rather than silently skipping work.
"""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile
import tomllib

CATEGORIES = ("positive-fixtures", "negative-fixtures", "dependency-fixtures", "unknown-fixtures", "fix-fixtures")


def findings(payload: dict, code: str) -> list[tuple[str, tuple[int, int]]]:
    result = []
    for item in payload.get("diagnostics", []):
        if item.get("code") == code and item.get("span"):
            result.append((code, tuple(item["span"])))
    return sorted(result)


def expected_findings(data: dict, code: str) -> list[tuple[str, tuple[int, int]]]:
    return sorted((item["code"], tuple(item["span"])) for item in data.get("diagnostics", []) if item["code"] == code)


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: test_linter_fixtures.py <eta_lint_binary> <rules_dir> <fixtures_dir>", file=sys.stderr)
        return 2
    binary, rules_dir, fixtures = (pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3]))
    failures = 0
    for spec_path in sorted(rules_dir.glob("*.toml")):
        spec = tomllib.loads(spec_path.read_text(encoding="utf-8"))
        if spec["status"] != "enabled":
            continue
        code = spec["code"]
        for category in CATEGORIES:
            for name in spec.get(category, []):
                source = fixtures / f"{name}.eta"
                data = json.loads((fixtures / f"{name}.json").read_text(encoding="utf-8"))
                process = subprocess.run([str(binary), "--rules", code, "--format", "json", str(source)],
                                         capture_output=True, text=True, check=False)
                if process.returncode in (64, 70):
                    print(f"{code}/{name}: unexpected exit {process.returncode}\n{process.stderr}", file=sys.stderr)
                    failures += 1
                    continue
                payload = json.loads(process.stdout)
                actual = findings(payload, code)
                expected = expected_findings(data, code)
                if actual != expected:
                    print(f"{code}/{name}: findings {actual} != expected {expected}", file=sys.stderr)
                    failures += 1
                if data.get("coverage") and not payload.get("coverage"):
                    print(f"{code}/{name}: expected recorded coverage", file=sys.stderr)
                    failures += 1
                if category == "fix-fixtures":
                    actual_fixed = ""
                    with tempfile.TemporaryDirectory(prefix="eta-lint-fix-") as tempdir:
                        candidate = pathlib.Path(tempdir) / source.name
                        candidate.write_bytes(source.read_bytes())
                        fixed = subprocess.run([str(binary), "--rules", code, "--fix", str(candidate)],
                                               capture_output=True, text=True, check=False)
                        actual_fixed = candidate.read_text(encoding="utf-8")
                    if fixed.returncode in (64, 70):
                        print(f"{code}/{name}: unexpected --fix exit {fixed.returncode}\n{fixed.stderr}", file=sys.stderr)
                        failures += 1
                        continue
                    expected_fixed = data.get("fixed-source")
                    if expected_fixed is None:
                        print(f"{code}/{name}: fix fixture requires fixed-source", file=sys.stderr)
                        failures += 1
                        continue
                    if actual_fixed != expected_fixed:
                        print(f"{code}/{name}: fixed source mismatch", file=sys.stderr)
                        failures += 1
    if failures:
        print(f"{failures} fixture check(s) failed", file=sys.stderr)
        return 1
    print("All linter fixtures reproduced their specified findings and coverage")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
