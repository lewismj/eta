import json
import pathlib
import subprocess
import sys
import tempfile
import textwrap


def run(args):
    return subprocess.run(args, capture_output=True, text=True, check=False)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_linter_cli.py <eta_lint_binary>", file=sys.stderr)
        return 2
    binary = pathlib.Path(sys.argv[1])

    listed = run([str(binary), "--list-rules"])
    if listed.returncode != 0 or "ETA104" not in listed.stdout or "ETA301" not in listed.stdout:
        print(listed.stdout, file=sys.stdout)
        print(listed.stderr, file=sys.stderr)
        return listed.returncode or 1

    explained = run([str(binary), "--explain", "ETA401"])
    if explained.returncode != 0 or "status: enabled" not in explained.stdout or "fix: safe" not in explained.stdout:
        print(explained.stdout, file=sys.stdout)
        print(explained.stderr, file=sys.stderr)
        return explained.returncode or 1

    help_result = run([str(binary), "--help"])
    if help_result.returncode != 0:
        print(help_result.stdout, file=sys.stdout)
        print(help_result.stderr, file=sys.stderr)
        return help_result.returncode

    with tempfile.TemporaryDirectory() as tempdir:
        source = textwrap.dedent(
            """
            (define foo_bar 1)
            ;; proper comment
            """
        ).lstrip()
        path = pathlib.Path(tempdir) / "sample.eta"
        path.write_text(source, encoding="utf-8")

        config = pathlib.Path(tempdir) / ".eta-lint.toml"
        config.write_text(
            textwrap.dedent(
                """
                [eta-lint]
                preset = "all"

                [rules]
                ETA104 = "off"
                """
            ).lstrip(),
            encoding="utf-8",
        )

        disabled = run([str(binary), "--config", str(config), "--format", "json", str(path)])
        if disabled.returncode != 0:
            print(disabled.stdout, file=sys.stdout)
            print(disabled.stderr, file=sys.stderr)
            return disabled.returncode
        payload = json.loads(disabled.stdout)
        if payload.get("diagnostics"):
            print("expected ETA104 to be disabled by config", file=sys.stderr)
            return 1

        option_source = pathlib.Path(tempdir) / "long.eta"
        option_source.write_text("(define (f x)\n  x\n)\n", encoding="utf-8")
        option_config = pathlib.Path(tempdir) / "options.toml"
        option_config.write_text(
            textwrap.dedent(
                """
                [eta-lint]
                preset = "all"

                [rules]
                ETA501 = { severity = "error", max-lines = 2 }
                """
            ).lstrip(),
            encoding="utf-8",
        )
        option_result = run([str(binary), "--config", str(option_config), "--format", "json", str(option_source)])
        if option_result.returncode != 1:
            print(option_result.stdout, file=sys.stdout)
            print(option_result.stderr, file=sys.stderr)
            return option_result.returncode
        option_payload = json.loads(option_result.stdout)
        rule = [item for item in option_payload.get("diagnostics", []) if item.get("code") == "ETA501"]
        if len(rule) != 1 or rule[0].get("severity") != "error":
            print(option_result.stdout, file=sys.stdout)
            return 1

        bad_config = pathlib.Path(tempdir) / "bad.toml"
        bad_config.write_text(
            textwrap.dedent(
                """
                [eta-lint]
                preset = "all"

                [rules]
                ETA501 = { unknown = 2 }
                """
            ).lstrip(),
            encoding="utf-8",
        )
        bad_result = run([str(binary), "--config", str(bad_config), str(option_source)])
        if bad_result.returncode != 64:
            print(bad_result.stdout, file=sys.stdout)
            print(bad_result.stderr, file=sys.stderr)
            return bad_result.returncode

        suppressed = pathlib.Path(tempdir) / "suppressed.eta"
        suppressed.write_text(";; eta-lint:disable=ETA104\n(define foo_bar 1)\n", encoding="utf-8")
        suppressed_result = run([str(binary), "--format", "json", str(suppressed)])
        if suppressed_result.returncode != 0:
            print(suppressed_result.stdout, file=sys.stdout)
            print(suppressed_result.stderr, file=sys.stderr)
            return suppressed_result.returncode
        if json.loads(suppressed_result.stdout).get("diagnostics"):
            print("expected eta-lint:disable to suppress ETA104", file=sys.stderr)
            return 1

        enabled = pathlib.Path(tempdir) / "enabled.eta"
        enabled.write_text(
            ";; eta-lint:disable=ETA104\n(define foo_bar 1)\n;; eta-lint:enable=ETA104\n(define foo_bar 2)\n",
            encoding="utf-8",
        )
        enabled_result = run([str(binary), "--format", "json", str(enabled)])
        if enabled_result.returncode != 1:
            print(enabled_result.stdout, file=sys.stdout)
            print(enabled_result.stderr, file=sys.stderr)
            return enabled_result.returncode
        enabled_payload = json.loads(enabled_result.stdout)
        enabled_findings = [item for item in enabled_payload.get("diagnostics", []) if item.get("code") == "ETA104"]
        if len(enabled_findings) != 1 or enabled_findings[0].get("span") != [73, 91]:
            print(enabled_result.stdout, file=sys.stdout)
            return 1

        invalid = pathlib.Path(tempdir) / "invalid.eta"
        invalid.write_text(";; eta-lint:disable=NOPE\n(define foo_bar 1)\n", encoding="utf-8")
        invalid_result = run([str(binary), "--format", "json", str(invalid)])
        if invalid_result.returncode != 2:
            print(invalid_result.stdout, file=sys.stdout)
            print(invalid_result.stderr, file=sys.stderr)
            return invalid_result.returncode
        invalid_payload = json.loads(invalid_result.stdout)
        if not any(item.get("code") == "eta_lint.Directive" for item in invalid_payload.get("diagnostics", [])):
            print(invalid_result.stdout, file=sys.stdout)
            return 1

        result = run([str(binary), "--format", "json", str(path)])
        if result.returncode not in (0, 1):
            print(result.stdout, file=sys.stdout)
            print(result.stderr, file=sys.stderr)
            return result.returncode
        payload = json.loads(result.stdout)
        if not payload.get("diagnostics"):
            print("expected Eta104 finding in CLI JSON output", file=sys.stderr)
            return 1
        assert any(item.get("code") == "ETA104" for item in payload["diagnostics"]), payload

        sarif = run([str(binary), "--format", "sarif", str(path)])
        if sarif.returncode not in (0, 1):
            print(sarif.stdout, file=sys.stdout)
            print(sarif.stderr, file=sys.stderr)
            return sarif.returncode
        sarif_payload = json.loads(sarif.stdout)
        if sarif_payload.get("version") != "2.1.0" or not sarif_payload.get("runs"):
            print(sarif.stdout, file=sys.stdout)
            return 1

        github = run([str(binary), "--format", "github", str(path)])
        if github.returncode not in (0, 1):
            print(github.stdout, file=sys.stdout)
            print(github.stderr, file=sys.stderr)
            return github.returncode
        if "::notice file=" not in github.stdout or "ETA104" not in github.stdout:
            print(github.stdout, file=sys.stdout)
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

