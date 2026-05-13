"""Fail if legacy builtin-name registration symbols reappear in C/C++ sources."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import re
import sys

REPO_ROOT = Path(__file__).resolve().parents[2]
LEGACY_SYMBOL_PATTERN = re.compile(r"\bregister_builtin_names(?:_legacy)?\b")
SOURCE_EXTENSIONS = {".h", ".hh", ".hpp", ".hxx", ".c", ".cc", ".cpp", ".cxx"}
SKIP_DIRS = {
    ".git",
    ".idea",
    ".vs",
    "build",
    "dist",
    "node_modules",
    "out",
    "third_party",
    "_deps",
    "CMakeFiles",
}


@dataclass(frozen=True)
class SymbolUsage:
    path: Path
    line_number: int
    line_text: str


def iter_source_files(root: Path):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [name for name in dirnames if name not in SKIP_DIRS]
        current_dir = Path(dirpath)
        for filename in filenames:
            path = current_dir / filename
            if path.suffix not in SOURCE_EXTENSIONS:
                continue
            yield path


def find_symbol_usages(root: Path) -> list[SymbolUsage]:
    usages: list[SymbolUsage] = []
    for source in sorted(iter_source_files(root)):
        try:
            text = source.read_text(encoding="utf-8")
        except UnicodeDecodeError as exc:
            raise RuntimeError(f"Failed to decode UTF-8 file: {source} ({exc})") from exc

        for line_number, line in enumerate(text.splitlines(), start=1):
            if LEGACY_SYMBOL_PATTERN.search(line):
                usages.append(
                    SymbolUsage(
                        path=source,
                        line_number=line_number,
                        line_text=line.rstrip(),
                    ))
    return usages


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
        usages = find_symbol_usages(root)
    except RuntimeError as exc:
        print(str(exc))
        return 1

    if not usages:
        print("clean -- no register_builtin_names* symbol usage found.")
        return 0

    print("legacy builtin registration symbol usage detected:")
    for usage in usages:
        rel = usage.path.relative_to(root)
        print(f"{rel}:{usage.line_number}: {usage.line_text}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
