"""Fail if M0 actor design-lock decisions drift."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import sys


REPO_ROOT = Path(__file__).resolve().parents[2]


REQUIRED_MARKERS: dict[Path, tuple[str, ...]] = {
    Path("docs/adr/0001-actors-vm-mailboxes-and-nng-transport.md"): (
        "Actors Use VM Mailboxes; NNG Is Distribution Transport",
        "eta/core/src/eta/runtime/memory/heap.h",
        "eta/core/src/eta/runtime/factory.h",
        "eta/session/src/eta/session/driver.h",
        "`receive-match` / `receive-after`",
    ),
    Path("docs/language_guide.md"): (
        "**Warning (Transition):**",
        "socket-based",
        "VM mailboxes addressed by PIDs",
    ),
    Path("docs/guide/reference/message-passing.md"): (
        "current socket-based concurrency compatibility",
        "VM mailboxes with PID",
    ),
    Path("docs/guide/reference/network-message-passing.md"): (
        "current socket mailbox model",
        "receive semantics to VM mailboxes",
    ),
    Path("docs/guide/macros.md"): (
        "`syntax-rules`",
        "procedural macros",
    ),
    Path("eta/core/src/eta/runtime/memory/heap.h"): (
        "enum class ObjectKind",
        "NativeObject",
    ),
    Path("eta/core/src/eta/runtime/factory.h"): (
        "template<typename T, ObjectKind Kind, typename... Args>",
        "make_heap_object",
    ),
    Path("eta/session/src/eta/session/driver.h"): (
        "std::unique_ptr<native::ActorRuntime> actor_runtime_;",
    ),
    Path("eta/session/src/eta/session/driver.cpp"): (
        "actor_runtime_(eta::nng::make_session_actor_runtime()),",
    ),
}


@dataclass(frozen=True)
class DesignLockIssue:
    path: Path
    marker: str
    reason: str


def read_utf8_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        raise RuntimeError(f"Failed to decode UTF-8 file: {path} ({exc})") from exc


def find_design_lock_issues(root: Path) -> list[DesignLockIssue]:
    issues: list[DesignLockIssue] = []
    for relative_path, markers in REQUIRED_MARKERS.items():
        path = root / relative_path
        if not path.is_file():
            issues.append(
                DesignLockIssue(
                    path=relative_path,
                    marker="<file>",
                    reason="missing required file",
                ))
            continue

        text = read_utf8_text(path)
        for marker in markers:
            if marker in text:
                continue
            issues.append(
                DesignLockIssue(
                    path=relative_path,
                    marker=marker,
                    reason="missing required marker",
                ))
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
        issues = find_design_lock_issues(root)
    except RuntimeError as exc:
        print(str(exc))
        return 1

    if not issues:
        print("clean -- actor M0 design-lock markers are present.")
        return 0

    print("actor M0 design-lock guard failures:")
    for issue in issues:
        print(
            f"- {issue.path.as_posix()}: {issue.reason}: {issue.marker}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
