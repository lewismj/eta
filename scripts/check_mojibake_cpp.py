"""Scan all C/C++ source and header files for mojibake and any non-ASCII chars."""
from __future__ import annotations
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Same detector as check_mojibake.py
DETECT = re.compile(
    "(?:"
    r"\u00C3[\u0080-\u00BF]"
    r"|\u00C2[\u0080-\u00BF]"
    r"|\u00E2\u0080[\u0080-\u00BF]"
    r"|\u00E2\u0086[\u0080-\u00BF]"
    r"|\u00E2\u0094[\u0080-\u00BF]"
    r"|\u00E2\u0095[\u0080-\u00BF]"
    r"|\u00E2\u0096[\u0080-\u00BF]"
    r"|\u00CE\u00B7"
    ")"
)

NON_ASCII = re.compile(r'[^\x00-\x7F]')

EXTS = {'.cpp', '.cc', '.cxx', '.c', '.h', '.hpp', '.hxx', '.hh'}
SKIP_DIRS = {'build', 'out', '.git', 'node_modules', 'third_party', '_deps', 'CMakeFiles'}

def scan(root: pathlib.Path):
    files = [
        p for p in root.rglob('*')
        if p.is_file()
        and p.suffix in EXTS
        and not any(d in p.parts for d in SKIP_DIRS)
    ]
    files.sort()

    found_any = False
    for f in files:
        try:
            text = f.read_text(encoding='utf-8')
        except UnicodeDecodeError as e:
            print(f'NOT-UTF8  {f.relative_to(root)}: {e}')
            found_any = True
            continue

        lines = text.splitlines()
        for i, line in enumerate(lines, 1):
            mojibake = DETECT.findall(line)
            non_ascii = NON_ASCII.findall(line)
            if mojibake or non_ascii:
                rel = f.relative_to(root)
                print(f'{rel}:{i}: {line.rstrip()}')
                if mojibake:
                    print(f'  -> mojibake sequences: {mojibake}')
                if non_ascii:
                    extras = [(c, f"U+{ord(c):04X}") for c in set(non_ascii)]
                    print(f'  -> non-ASCII chars: {extras}')
                found_any = True

    if not found_any:
        print('clean -- no mojibake or non-ASCII found in C/C++ files.')
    return found_any

if __name__ == '__main__':
    root = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT
    sys.exit(0 if not scan(root) else 1)

