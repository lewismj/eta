"""Detect (and optionally repair) mojibake in docs/*.md.

Mojibake patterns we look for arise when UTF-8 bytes were once
mis-decoded as Windows-1252 (or Latin-1) and then re-encoded as UTF-8.
That round trip turns a single non-ASCII codepoint into a 2-3 char
sequence whose bytes start with 0xC2/0xC3/0xCE/0xE2 in the *original*
UTF-8 byte stream.

Examples we have seen in this repo:
    'â€"'  -> '—'  (em dash)
    'â€"'  -> '–'  (en dash)
    'â€™'  -> '\u2019' (right single quote)
    'â€œ'  -> '\u201C'
    'â€\u009d'-> '\u201D'
    'â€¦'  -> '…'
    'â†'  -> '→'
    'Â·'  -> '·'
    'Â '  -> NBSP (drop / replace with ' ')
    'Î·'  -> 'η'
    'â–º'  -> '\u25BA'
    'â”€'  -> '─'  (and other box-drawing chars)
    'â•'  -> box drawing
    'â–'  -> block elements

Usage:
    python scripts/check_mojibake.py            # report
    python scripts/check_mojibake.py --fix      # rewrite files in place
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Map of mojibake -> intended character.  Order matters: longer keys first.
FIXES: dict[str, str] = {
    "â€\u009d": "\u201D",   # right double quotation mark
    "â€\u009c": "\u201C",   # left  double quotation mark
    "â€œ":      "\u201C",
    "â€™":      "\u2019",   # right single quote / apostrophe
    "â€˜":      "\u2018",
    "â€”":      "—",        # em dash
    "â€“":      "–",        # en dash
    "â€¢":      "•",
    "â€¦":      "…",
    "â†’":      "→",
    "â†':":     "→:",
    "â†'":      "→",
    "â†\u0090": "←",
    "â†\u0091": "↑",
    "â†\u0093": "↓",
    "Î·":       "η",
    "Â·":       "·",
    "Â\u00A0":  "\u00A0",   # already NBSP — drop the spurious Â prefix
    "Â»":       "»",
    "Â«":       "«",
    "Â©":       "©",
    "Â®":       "®",
    "Â°":       "°",
    "Â±":       "±",
    "Â²":       "²",
    "Â³":       "³",
    "Â¹":       "¹",
    "Â¼":       "¼",
    "Â½":       "½",
    "Â¾":       "¾",
    # Box drawing (used in CMake banner blocks copy-pasted into docs)
    "â•‘": "║", "â•—": "╗", "â•":  "═", "â•‘": "║", "â•":  "═",
    "â•”": "╔", "â•š": "╚", "â•": "╝", "â•”": "╔",
    "â”€": "─", "â”‚": "│", "â”Œ": "┌", "â”": "┐",
    "â””": "└", "â”˜": "┘", "â”œ": "├", "â”¤": "┤",
    "â”¬": "┬", "â”´": "┴", "â”¼": "┼",
    "â–º": "►", "â–¶": "▶", "â–ª": "▪", "â–«": "▫", "â–\u0091": "▒",
    # Common accented letters (just in case)
    "Ã©": "é", "Ã¨": "è", "Ãª": "ê", "Ã«": "ë",
    "Ã¡": "á", "Ã ": "à", "Ã¢": "â", "Ã¤": "ä",
    "Ã­": "í", "Ã®": "î", "Ã¯": "ï",
    "Ã³": "ó", "Ã²": "ò", "Ã´": "ô", "Ã¶": "ö",
    "Ãº": "ú", "Ã¹": "ù", "Ã»": "û", "Ã¼": "ü",
    "Ã±": "ñ", "Ã§": "ç",
    "Ã‰": "É", "Ãˆ": "È",
    "ÃŸ": "ß",
}

# A loose detector for "anything that *looks* like mojibake" so we can
# flag survivors that aren't yet in the fix map.
DETECT = re.compile(
    "(?:"
    r"\u00C3[\u0080-\u00BF]"        # Ã + cont
    r"|\u00C2[\u0080-\u00BF]"       # Â + cont
    r"|\u00E2\u0080[\u0080-\u00BF]" # â€…
    r"|\u00E2\u0086[\u0080-\u00BF]" # â†…
    r"|\u00E2\u0094[\u0080-\u00BF]" # â”… box-drawing light
    r"|\u00E2\u0095[\u0080-\u00BF]" # â•… box-drawing heavy
    r"|\u00E2\u0096[\u0080-\u00BF]" # â–… block elements
    r"|\u00CE\u00B7"                # Î·
    ")"
)


def repair(text: str) -> str:
    # Apply longest-key first so that 'â€"' wins over 'â€'
    for src in sorted(FIXES, key=len, reverse=True):
        if src in text:
            text = text.replace(src, FIXES[src])
    return text


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fix", action="store_true",
                    help="rewrite affected files in place (UTF-8, no BOM)")
    ap.add_argument("--root", default=str(ROOT / "docs"),
                    help="directory to scan (default: docs/)")
    args = ap.parse_args()

    root = pathlib.Path(args.root)

    files = sorted(
        p for p in root.rglob("*")
        if p.is_file() and p.suffix in {".md", ".js", ".ts"}
    )

    affected = []
    for f in files:
        try:
            text = f.read_text(encoding="utf-8")
        except UnicodeDecodeError as e:
            print(f"  !! {f.relative_to(root.parent)}: not valid UTF-8 ({e})")
            continue

        before = len(DETECT.findall(text))
        if before == 0:
            continue

        fixed = repair(text)
        after = len(DETECT.findall(fixed))
        affected.append((f, before, after, fixed != text, fixed))

    if not affected:
        print("clean — no mojibake patterns detected.")
        return 0

    width = max(len(str(f.relative_to(root.parent))) for f, *_ in affected)
    print(f"{'file':<{width}}  before  remaining  fixable")
    print("-" * (width + 28))
    for f, before, after, changed, _ in affected:
        rel = str(f.relative_to(root.parent))
        print(f"{rel:<{width}}  {before:>6}  {after:>9}  {'yes' if changed else 'no '}")

    if args.fix:
        n = 0
        for f, _, _, changed, fixed in affected:
            if changed:
                f.write_text(fixed, encoding="utf-8", newline="\n")
                n += 1
        print(f"\nrewrote {n} file(s).")
    else:
        print("\n(run with --fix to rewrite the files in place)")

    # exit non-zero if anything *unfixable* remains, so CI can gate on it
    return 1 if any(after > 0 and not changed for _, _, after, changed, _ in affected) else 0


if __name__ == "__main__":
    sys.exit(main())

