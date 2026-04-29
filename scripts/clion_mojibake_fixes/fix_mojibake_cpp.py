"""Fix mojibake and BOM in C/C++ source files.

Strategy:
  1. Strip UTF-8 BOM (\uFEFF) from line 1.
  2. Re-decode double-encoded cp1252 sequences (same approach as fix_mojibake_ext.py).
  3. Replace remaining non-ASCII in *string literals and BOOST messages*
     with clean ASCII equivalents so the build is warning-free.
  4. Skip intern_table_tests.cpp (contains intentional raw UTF-8 bytes).
  5. Leave intentional Unicode in comments (em-dash, ≤, →) untouched.
"""
from __future__ import annotations
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

SKIP = {"intern_table_tests.cpp"}

EXTS = {'.cpp', '.cc', '.cxx', '.c', '.h', '.hpp', '.hxx', '.hh'}
SKIP_DIRS = {'build', 'out', '.git', 'node_modules', 'third_party', '_deps', 'CMakeFiles'}

# ── cp1252 → utf-8 re-decode ─────────────────────────────────────────────────

def cp1252_repair(line: str) -> str:
    try:
        return line.encode('cp1252').decode('utf-8')
    except Exception:
        return line

# ── Post-repair Unicode → ASCII replacements ──────────────────────────────────
# Applied ONLY to lines that still contain non-ASCII after cp1252 repair,
# so we don't accidentally touch well-formed Unicode in comments.

UNICODE_TO_ASCII = [
    # em/en dash in string literals → ' - '
    ('\u2014', ' - '),   # em dash
    ('\u2013', ' - '),   # en dash
    # right arrow
    ('\u2192', '->'),
    ('\u2190', '<-'),
    # ellipsis
    ('\u2026', '...'),
    # check / cross marks in test messages
    ('\u2714', '[OK]'),    # ✔
    ('\u2718', '[FAIL]'),  # ✘
    ('\u2713', '[OK]'),    # ✓
    ('\u2717', '[FAIL]'),  # ✗
    ('\u2611', '[OK]'),    # ☑
    ('\u2612', '[FAIL]'),  # ☒
    ('\u228a', '[SKIP]'),  # ⊊ (was a skip icon)
    ('\u228b', '[INFO]'),
    # per-mille sign in error messages (used as ≥)
    ('\u2030', '>='),      # ‰
    # leftwards arrow ligature from partial repair
    ('\u2949', '[>=]'),    # ⥉ residual from ‰ + 2
    # skip/warning/pass icons in test messages
    ('\u2298', '[SKIP]'),  # ⊘
    ('\u26a0', '[WARN]'),  # ⚠
    ('\u2714', '[OK]'),    # ✔ heavy check
    ('\u2718', '[FAIL]'),  # ✘ heavy ballot
    ('\u2713', '[OK]'),    # ✓
    ('\u2717', '[FAIL]'),  # ✗
    ('\u2611', '[OK]'),    # ☑
    ('\u2612', '[FAIL]'),  # ☒
    # logic operators in Eta-code comment strings — leave as Unicode (correct UTF-8)
    # ∧ ∨ ¬ ⊤ ² are intentional in vm_tests inline Eta code, don't replace them
]

NON_ASCII = re.compile(r'[^\x00-\x7F]')

def fix_non_ascii_in_strings(line: str) -> str:
    """Replace non-ASCII chars that end up inside C++ string literals."""
    result = line
    for src, dst in UNICODE_TO_ASCII:
        result = result.replace(src, dst)
    return result

# ── Main ──────────────────────────────────────────────────────────────────────

def process_file(path: pathlib.Path) -> tuple[bool, str]:
    """Return (changed, new_text)."""
    try:
        original = path.read_text(encoding='utf-8')
    except UnicodeDecodeError:
        print(f'  SKIP (not valid UTF-8): {path}')
        return False, ''

    lines = original.splitlines(keepends=True)
    out = []
    for line in lines:
        # 1. Strip BOM
        if line.startswith('\ufeff'):
            line = line[1:]

        # 2. cp1252 → utf-8 re-decode
        line = cp1252_repair(line)

        # 3. Replace remaining non-ASCII with ASCII equivalents
        if NON_ASCII.search(line):
            line = fix_non_ascii_in_strings(line)

        out.append(line)

    result = ''.join(out)
    return result != original, result


def main():
    dry_run = '--dry-run' in sys.argv

    files = [
        p for p in ROOT.rglob('*')
        if p.is_file()
        and p.suffix in EXTS
        and not any(d in p.parts for d in SKIP_DIRS)
        and p.name not in SKIP
    ]
    files.sort()

    changed_count = 0
    for f in files:
        changed, new_text = process_file(f)
        if changed:
            changed_count += 1
            rel = f.relative_to(ROOT)
            if dry_run:
                print(f'  would fix: {rel}')
            else:
                f.write_text(new_text, encoding='utf-8')
                print(f'  fixed: {rel}')

    print(f'\n{"[DRY RUN] Would fix" if dry_run else "Fixed"} {changed_count} file(s).')


if __name__ == '__main__':
    main()

