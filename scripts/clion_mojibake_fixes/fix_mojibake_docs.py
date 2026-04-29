"""Fix U+FFFD replacement characters in docs/guide/reference/modules.md.

Context-aware rules:
  - After markdown link ](url)       -> middle dot separator (U+00B7)
  - Inside `code` backtick spans     -> rightwards arrow ->
  - Module section headers ### `std. -> em dash
  - Pass headers #### Pass N         -> em dash
  - Code form placeholders (x ?)     -> ellipsis ...
  - Torch pipeline steps             -> first ? = em dash, rest = arrows
  - Trailing list ..., ?)            -> ellipsis
  - After closing ` in table cells   -> em dash
  - Catch-all                        -> em dash
"""

from __future__ import annotations
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
FFFD = "\ufffd"
MDOT = "\u00b7"    # middle dot
ARROW = "\u2192"   # rightwards arrow
EMDASH = "\u2014"  # em dash
ELLIP = "\u2026"   # ellipsis


def _fix_in_backtick(m: re.Match) -> str:
    """Replace FFFD inside a single-backtick span with an arrow."""
    return "`" + m.group(1).replace(FFFD, ARROW) + "`"


def repair_fffd_line(line: str) -> str:
    if FFFD not in line:
        return line

    # 1. Math special: square `(x) ? x?` -> `(x) -> x^2`
    if "`square`" in line:
        line = re.sub(
            r"`\(x\) " + FFFD + r" x" + FFFD + r"`",
            "`(x) \u2192 x\u00b2`",
            line,
        )
    if "`cube`" in line:
        line = re.sub(
            r"`\(x\) " + FFFD + r" x" + FFFD + r"`",
            "`(x) \u2192 x\u00b3`",
            line,
        )

    # 2. Code-form placeholders: (module ?), (export ?), (import ?)
    for form in ("module", "export", "import"):
        line = line.replace(f"({form} {FFFD})", f"({form} {ELLIP})")

    # 3. Torch pipeline: train-step! ? one-call (zero-grad ? forward ? ...)
    #    First ? = em dash, all subsequent ? = arrows
    if "zero-grad" in line and FFFD in line:
        parts = line.split(FFFD)
        line = parts[0] + EMDASH + ARROW.join(parts[1:])
        return line

    # 4. Navigation / key-sources separators: ](url) ? -> ](url) *
    line = re.sub(r"(\]\([^)]*\))\s*" + FFFD, r"\1 " + MDOT, line)

    # 5. Section headers: ### `std.xxx` ? Description  or  #### Pass N ? func
    if re.match(r"^#+\s+`std\.", line) or re.match(r"^####\s+Pass\s+\d+", line):
        line = line.replace(FFFD, EMDASH)
        return line

    # 6. FFFD inside backtick code spans -> arrow
    line = re.sub(r"`([^`]*" + FFFD + r"[^`]*)`", _fix_in_backtick, line)

    # 7. Flow arrows in numbered list items: "forms ? populate" / ") ? populate"
    line = re.sub(FFFD + r"\s*(populate)", ARROW + r" \1", line)

    # 8. Specific keyword patterns
    spec = {
        "dynamic-wind":       EMDASH,
        "Create a socket":    EMDASH,
        "publishDiagnostics": EMDASH,
        "survey-time":        ELLIP,
        "dag:parents":        ELLIP,
    }
    for kw, repl in spec.items():
        if kw in line and FFFD in line:
            line = line.replace(FFFD, repl)
            return line

    # 9. After closing backtick in table cell: `sig` ? description -> `sig` - desc
    line = re.sub(r"`\s*" + FFFD + r"\s*", "` " + EMDASH + " ", line)

    # 10. Catch-all: em dash
    line = line.replace(FFFD, EMDASH)
    return line


def repair_fffd(text: str) -> str:
    if FFFD not in text:
        return text
    return "".join(repair_fffd_line(ln) for ln in text.splitlines(keepends=True))


def _classic_repair(text: str) -> str:
    """Delegate to check_mojibake.py's repair() for double-encoded UTF-8."""
    scripts_dir = ROOT / "scripts"
    sys.path.insert(0, str(scripts_dir))
    try:
        import check_mojibake
        return check_mojibake.repair(text)
    except ImportError:
        return text
    finally:
        sys.path.pop(0)


def main() -> int:
    import argparse

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default=str(ROOT / "docs"),
                    help="directory to scan (default: docs/)")
    ap.add_argument("--dry-run", action="store_true",
                    help="report changes without writing files")
    args = ap.parse_args()

    root = pathlib.Path(args.root)
    files = sorted(p for p in root.rglob("*") if p.is_file() and p.suffix == ".md")

    changed = 0
    for f in files:
        try:
            original = f.read_text(encoding="utf-8")
        except UnicodeDecodeError as e:
            print(f"  !! {f.relative_to(root.parent)}: not valid UTF-8 ({e})")
            continue

        text = _classic_repair(original)
        text = repair_fffd(text)

        if text == original:
            continue

        rel = f.relative_to(root.parent)
        if args.dry_run:
            print(f"  would fix: {rel}")
            for i, line in enumerate(text.splitlines(), 1):
                if FFFD in line:
                    print(f"    L{i}: remaining FFFD: {line.rstrip()!r}")
        else:
            f.write_text(text, encoding="utf-8", newline="\n")
            print(f"  fixed: {rel}")
            changed += 1

    if not args.dry_run:
        print(f"\nTotal files fixed: {changed}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
