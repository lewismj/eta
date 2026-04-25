import re

path = r'C:\Users\lewis\develop\eta\editors\vscode\src\extension.ts'
with open(path, 'r', encoding='utf-8') as f:
    text = f.read()

# The file has double-encoded UTF-8: originally Unicode chars were read as
# Latin-1/Win-1252 and re-saved as UTF-8. Fix by encoding to latin-1 then
# decoding as utf-8, line by line (lines with only ASCII pass through unchanged).
lines = text.splitlines(keepends=True)
fixed = []
for line in lines:
    try:
        fixed.append(line.encode('cp1252').decode('utf-8'))
    except Exception:
        fixed.append(line)
result = ''.join(fixed)

# Strip UTF-8 BOM if present
result = result.lstrip('\ufeff')

# Replace decorative Unicode with plain ASCII equivalents
result = re.sub(r'\u2500+', lambda m: '-' * len(m.group()), result)  # box-drawing dashes
result = result.replace('\u2026', '...')   # ellipsis
result = result.replace('\u2192', '->')    # rightwards arrow
result = result.replace('\u2190', '<-')    # leftwards arrow
result = result.replace('\u2191', '->')    # upwards arrow (DAP send)
result = result.replace('\u2193', '<-')    # downwards arrow (DAP receive)
result = result.replace('\u2014', ' - ')   # em dash
result = result.replace('\u2013', '-')     # en dash

# Handle residual sequences where \x90 (undefined in cp1252) prevented full fix.
# â†\x90 = bytes E2 86 90 = ← (U+2190 leftwards arrow)
result = result.replace('\u00e2\u2020\u0090', '<-')   # ← residual
# â†" = bytes E2 86 93 = ↓ (U+2193 downwards arrow)
result = result.replace('\u00e2\u2020\u201c', '<-')   # ↓ residual
# â†' = bytes E2 86 91 = ↑ (U+2191 upwards arrow)
result = result.replace('\u00e2\u2020\u2018', '->')   # ↑ residual
# â€" = bytes E2 80 93 = – (en dash), but after partial fix may appear as â€\u201d
result = result.replace('\u00e2\u20ac\u201d', '-')    # – residual
# Catch-all: any remaining stray â† sequences (truncated arrow sequences)
result = re.sub(r'\u00e2\u2020.?', '<-', result)

with open(path, 'w', encoding='utf-8') as f:
    f.write(result)

# Report any remaining non-ASCII
for i, line in enumerate(result.splitlines(), 1):
    non_ascii = [(j, c) for j, c in enumerate(line) if ord(c) > 127]
    if non_ascii:
        print(f'  L{i}: {line.rstrip()}')
        for j, c in non_ascii:
            print(f'    col {j}: U+{ord(c):04X} {repr(c)}')

print('Done.')


