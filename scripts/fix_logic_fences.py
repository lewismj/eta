import re, pathlib

path = pathlib.Path(r"C:\Users\lewis\develop\eta\docs\guide\reference\logic.md")
lines = path.read_text(encoding="utf-8").splitlines(keepends=True)
in_block = False
out = []
for line in lines:
    stripped = line.rstrip()
    if not in_block and re.match(r'^```\s*$', stripped):
        out.append('```text\n')
        in_block = True
    elif in_block and re.match(r'^```\s*$', stripped):
        out.append('```\n')
        in_block = False
    else:
        if re.match(r'^```', stripped):
            in_block = not in_block
        out.append(line)
path.write_text(''.join(out), encoding='utf-8')
print("done, blocks tagged")

