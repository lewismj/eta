import pathlib

p = pathlib.Path(r'C:\Users\lewis\develop\eta\eta\qa\test\src\builtin_sync_tests.cpp')
raw = p.read_bytes()
print(f'File size: {len(raw)} bytes')
print(f'First 6 bytes: {raw[:6].hex()}')

if raw[:3] == b'\xef\xbb\xbf':
    print('Has UTF-8 BOM (EF BB BF) -- this is the problem CLion sees')
elif raw[:2] == b'\xff\xfe':
    print('Has UTF-16 LE BOM')
elif raw[:2] == b'\xfe\xff':
    print('Has UTF-16 BE BOM')
else:
    print('No BOM')

try:
    raw.decode('utf-8')
    print('Valid UTF-8: YES')
except UnicodeDecodeError as e:
    print(f'UTF-8 decode FAILED at byte {e.start}: {e}')
    pos = e.start
    ctx = raw[max(0, pos-10):pos+10]
    print(f'Bytes around error: {ctx.hex()}')

