"""Fail if docs regress on builtin catalog source-of-truth guarantees."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import sys

REPO_ROOT = Path(__file__).resolve().parents[2]
DOCS_ROOT = Path("docs")
ARCHITECTURE_DOC = DOCS_ROOT / "architecture.md"
EXCLUDED_DOC_PREFIXES = (
    DOCS_ROOT / "img",
    DOCS_ROOT / "plan",
)
LEGACY_DOC_PATTERNS = (
    ("builtin_names.h", re.compile(r"\bbuiltin_names\.h\b")),
    ("register_builtin_names*", re.compile(r"\bregister_builtin_names(?:_legacy)?\b")),
)
ARCHITECTURE_REQUIRED_PATTERNS = (
    ("builtin_catalog.h reference", re.compile(r"builtin_catalog\.h")),
    ("single source of truth statement", re.compile(r"single source of truth", re.IGNORECASE)),
    ("register_builtin_specs(...) reference", re.compile(r"register_builtin_specs\s*\(")),
)


@dataclass(frozen=True)
class LegacyDocReference:
    pattern_name: str
    path: Path
    line_number: int
    line_text: str


def is_excluded_doc_path(relative_path: Path) -> bool:
    return any(
        relative_path == prefix or prefix in relative_path.parents
        for prefix in EXCLUDED_DOC_PREFIXES
    )


def iter_scanned_docs(root: Path):
    docs_root = root / DOCS_ROOT
    if not docs_root.is_dir():
        return
    for path in sorted(docs_root.rglob("*.md")):
        relative_path = path.relative_to(root)
        if is_excluded_doc_path(relative_path):
            continue
        yield path


def read_utf8_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        raise RuntimeError(f"Failed to decode UTF-8 file: {path} ({exc})") from exc


def find_legacy_doc_references(root: Path) -> list[LegacyDocReference]:
    references: list[LegacyDocReference] = []
    for doc_path in iter_scanned_docs(root):
        text = read_utf8_text(doc_path)
        for line_number, line in enumerate(text.splitlines(), start=1):
            for pattern_name, pattern in LEGACY_DOC_PATTERNS:
                if not pattern.search(line):
                    continue
                references.append(
                    LegacyDocReference(
                        pattern_name=pattern_name,
                        path=doc_path,
                        line_number=line_number,
                        line_text=line.rstrip(),
                    ))
    return references


def architecture_doc_issues(root: Path) -> list[str]:
    issues: list[str] = []
    architecture_path = root / ARCHITECTURE_DOC
    if not architecture_path.is_file():
        return [f"Missing architecture doc: {ARCHITECTURE_DOC.as_posix()}"]

    text = read_utf8_text(architecture_path)
    for description, pattern in ARCHITECTURE_REQUIRED_PATTERNS:
        if pattern.search(text):
            continue
        issues.append(
            f"Architecture doc missing required {description}: "
            f"{ARCHITECTURE_DOC.as_posix()}")
    return issues


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        default=str(REPO_ROOT),
        help="Repository root to scan (defaults to this repository).",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = Path(args.root).resolve()

    try:
        legacy_refs = find_legacy_doc_references(root)
        architecture_issues = architecture_doc_issues(root)
    except RuntimeError as exc:
        print(str(exc))
        return 1

    has_errors = False
    if legacy_refs:
        has_errors = True
        print("legacy builtin-name doc references detected:")
        for ref in legacy_refs:
            rel = ref.path.relative_to(root)
            print(f"{rel}:{ref.line_number}: [{ref.pattern_name}] {ref.line_text}")

    if architecture_issues:
        has_errors = True
        print("architecture doc guard failures:")
        for issue in architecture_issues:
            print(f"- {issue}")

    if has_errors:
        return 1

    print("clean -- docs use builtin catalog contract and no legacy builtin-name references.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
