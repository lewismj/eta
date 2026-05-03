#!/usr/bin/env python3
"""Build stdlib .etac artifacts with the existing etac CLI."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--etac", required=True, help="Path to the etac executable")
    parser.add_argument("--src-root", required=True, help="Path to stdlib source root")
    parser.add_argument("--out-root", required=True, help="Output root for mirrored .eta/.etac files")
    return parser.parse_args()


def list_sources(src_root: Path) -> list[Path]:
    files: list[Path] = []
    for source in sorted(src_root.rglob("*.eta")):
        rel = source.relative_to(src_root)
        if rel.parts and rel.parts[0] == "tests":
            continue
        files.append(source)
    return files


def mirror_sources(sources: list[Path], src_root: Path, out_root: Path) -> None:
    for source in sources:
        rel = source.relative_to(src_root)
        destination = out_root / rel
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)


def remove_stale_etac(out_root: Path) -> None:
    if not out_root.exists():
        return
    for artifact in out_root.rglob("*.etac"):
        artifact.unlink()


def compile_source(etac_exe: Path, src_root: Path, source: Path, out_root: Path) -> None:
    rel = source.relative_to(src_root)
    out_file = (out_root / rel).with_suffix(".etac")
    out_file.parent.mkdir(parents=True, exist_ok=True)

    def run(no_prelude: bool) -> tuple[int, list[str], str, str]:
        command = [
            str(etac_exe),
            "--path",
            str(src_root),
            "-O",
            "--no-debug",
            str(source),
            "-o",
            str(out_file),
        ]
        if no_prelude:
            command.insert(1, "--no-prelude")
        completed = subprocess.run(command, capture_output=True, text=True)
        return completed.returncode, command, completed.stdout, completed.stderr

    first_rc, first_cmd, first_out, first_err = run(no_prelude=True)
    if first_rc == 0:
        return

    second_rc, second_cmd, second_out, second_err = run(no_prelude=False)
    if second_rc == 0:
        return

    message = [
        f"error: etac failed for {source}",
        f"command 1: {' '.join(first_cmd)}",
        f"command 2: {' '.join(second_cmd)}",
    ]
    if first_out:
        message.append("stdout (attempt 1):")
        message.append(first_out.rstrip())
    if first_err:
        message.append("stderr (attempt 1):")
        message.append(first_err.rstrip())
    if second_out:
        message.append("stdout (attempt 2):")
        message.append(second_out.rstrip())
    if second_err:
        message.append("stderr (attempt 2):")
        message.append(second_err.rstrip())
    raise RuntimeError("\n".join(message))


def main() -> int:
    args = parse_args()
    etac_exe = Path(args.etac).resolve()
    src_root = Path(args.src_root).resolve()
    out_root = Path(args.out_root).resolve()

    if not etac_exe.is_file():
        print(f"error: etac executable not found: {etac_exe}", file=sys.stderr)
        return 1
    if not src_root.is_dir():
        print(f"error: stdlib source root not found: {src_root}", file=sys.stderr)
        return 1

    sources = list_sources(src_root)
    if not sources:
        print(f"error: no .eta files found under {src_root}", file=sys.stderr)
        return 1

    out_root.mkdir(parents=True, exist_ok=True)
    remove_stale_etac(out_root)
    mirror_sources(sources, src_root, out_root)

    for source in sources:
        compile_source(etac_exe, src_root, source, out_root)

    print(f"built {len(sources)} stdlib .etac artifacts in {out_root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
