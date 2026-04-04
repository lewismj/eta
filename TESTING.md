# Eta — Testing & Release Bundle

## Prerequisites

| Tool            | Version      |
|-----------------|-------------|
| CMake           | ≥ 3.28      |
| C++ compiler    | C++23 (Clang 17+, GCC 13+, or MSVC 17.8+) |
| Boost           | ≥ 1.88 (unit_test_framework) |
| Node.js / npm   | ≥ 18        |
| vsce            | `npm install -g @vscode/vsce` |

---

## 1. Build the Eta binaries (Release)

### Linux / WSL

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
cmake --install build --prefix dist/eta-linux-x64
```

### Windows (MSVC)

```powershell
cmake -B build
cmake --build build --config Release
cmake --install build --prefix dist/eta-win-x64 --config Release
```

After install, the layout is:

```
dist/eta-<platform>/
  bin/
    etai(.exe)
    eta_repl(.exe)
    eta_lsp(.exe)
  stdlib/
    prelude.eta
    std/
      core.eta
      math.eta
      io.eta
      collections.eta
      test.eta
```

The binaries automatically locate `stdlib/` relative to themselves
(`<exe>/../stdlib/`), so no environment variables are needed.

---

## 2. Run from the command line

```bash
# Run a script
dist/eta-linux-x64/bin/etai examples/eta/hello.eta

# Interactive REPL
dist/eta-linux-x64/bin/eta_repl
```

In the REPL, the prelude is loaded automatically:

```
η> (import std.core)
η> (atom? 42)
#t
η> (import std.collections)
η> (filter (lambda (x) (> x 3)) (range 1 7))
(4 5 6)
```

---

## 3. Package the VS Code extension

```bash
cd editors/vscode
npm ci
```

To bundle native binaries inside the extension (optional but
recommended for distribution):

```bash
# Copy the installed binaries + stdlib into the extension tree
cp -r ../../dist/eta-linux-x64/bin ./bin
cp -r ../../dist/eta-linux-x64/stdlib ./stdlib
```

Then package:

```bash
npx @vscode/vsce package
```

This produces `eta-lang-0.1.0.vsix`.

---

## 4. Install the extension in VS Code

```bash
code --install-extension eta-lang-0.1.0.vsix
```

Or in VS Code: **Extensions → ⋯ → Install from VSIX…**

---

## 5. Using the extension

1. Open or create any `.eta` file — syntax highlighting works
   immediately.

2. The extension starts the `eta_lsp` server automatically. It looks
   for the binary in this order:
   - `eta.lsp.serverPath` setting (if set)
   - `<extension>/bin/eta_lsp` (bundled in the `.vsix`)
   - Workspace build output directories
   - `PATH`

3. Diagnostics (parse errors, undefined names, etc.) appear in the
   Problems panel as you type.

---

## 6. Standard Library Modules

| Module               | Description                              |
|----------------------|------------------------------------------|
| `std.core`           | `atom?`, `compose`, `flip`, `iota`, … |
| `std.math`           | `pi`, `e`, `even?`, `gcd`, `expt`, …  |
| `std.io`             | `println`, `read-line`, port helpers     |
| `std.collections`    | `filter`, `foldl`, `sort`, `range`, …  |
| `std.test`           | `make-test`, `assert-equal`, `run`, …  |
| `std.prelude`        | Re-exports everything from the above     |

Example user program:

```scheme
(module hello
  (import std.io)
  (begin
    (println "Hello, world!")))
```

---

## 7. Manual stdlib path (if not using installed layout)

If you run `etai` from a build directory (not the install tree), point
it at the stdlib:

```bash
# Via CLI flag
./build/eta/interpreter/etai --path /path/to/eta/stdlib examples/eta/hello.eta

# Or via environment variable
export ETA_MODULE_PATH=/path/to/eta/stdlib
./build/eta/interpreter/etai examples/eta/hello.eta
```

