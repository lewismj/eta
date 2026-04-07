# Eta — Testing & Release Bundle

## Prerequisites

| Tool            | Version      |
|-----------------|-------------|
| CMake           | ≥ 3.28      |
| C++ compiler    | C++23 (Clang 17+, GCC 13+, or MSVC 17.8+) |
| Boost           | ≥ 1.88 (unit_test_framework) |
| Node.js / npm   | ≥ 18        |
| vsce (optional) | `npm install -g @vscode/vsce` |

---

## Quick Start — One-Script Build

### Linux / macOS / WSL

```bash
chmod +x scripts/build-release.sh
./scripts/build-release.sh ./dist/eta-linux-x86_64
```

Produces `dist/eta-linux-x86_64/` and `dist/eta-linux-x86_64.tar.gz`.

### Windows (PowerShell)

```powershell
.\scripts\build-release.ps1 .\dist\eta-win-x64
```

Produces `dist\eta-win-x64\` and `dist\eta-win-x64.zip`.

Both scripts take a single **required** argument — the directory to
install the bundle into.

---

## Installing from a Release Archive

Download the archive for your platform from
[GitHub Releases](../../releases), then:

### Linux / macOS

```bash
tar xzf eta-linux-x86_64.tar.gz
cd eta-linux-x86_64
./install.sh                  # adds bin/ to PATH, sets ETA_MODULE_PATH
                              # installs VS Code extension if 'code' is on PATH
```

To install into a custom prefix instead of using the bundle in-place:

```bash
./install.sh /usr/local       # copies bin/, stdlib/ → /usr/local/
```

### Windows

```powershell
Expand-Archive eta-win-x64.zip -DestinationPath .
cd eta-win-x64
.\install.cmd                 # adds bin\ to user PATH, sets ETA_MODULE_PATH
                              # installs VS Code extension if 'code' is on PATH
```

To install into a custom prefix:

```powershell
.\install.cmd "C:\Program Files\Eta"
```

> [!NOTE]
> Always use `install.cmd` rather than calling `install.ps1`
> directly — the `.cmd` wrapper launches PowerShell with
> `-ExecutionPolicy Bypass` so it works regardless of the system
> execution policy.

---

## Bundle Layout

After build or extraction:

```
eta-<platform>/
  bin/
    etai(.exe)                # file interpreter
    eta_repl(.exe)            # interactive REPL
    eta_lsp(.exe)             # language server
  stdlib/
    prelude.eta               # auto-loaded standard library
    std/
      core.eta math.eta io.eta collections.eta test.eta
  editors/
    vscode/                   # bundled VS Code extension
  install.sh / install.cmd     # post-extract installer (install.ps1 is called by install.cmd)
  TESTING.md
```

The binaries automatically locate `stdlib/` relative to themselves
(`<exe>/../stdlib/`), so no environment variables are needed when
using the installed layout.

---

## Run from the Command Line

### Running a script

```bash
etai examples/hello.eta
```

### Interactive REPL

```bash
eta_repl
```

The REPL auto-loads `std.prelude`, which re-exports every name from
`std.core`, `std.math`, `std.io`, and `std.collections`.  All standard
library functions are available immediately — no explicit `import`
needed:

```
η> (atom? 42)
#t
η> (even? 6)
#t
η> (filter (lambda (x) (> x 3)) (range 1 7))
(4 5 6)
```

You can define and use your own functions interactively:

```
η> (defun square (x) (* x x))
η> (square 7)
=> 49
```

You can import user-defined modules too.  Use `--path` to tell the REPL
where to find your `.eta` files:

```bash
eta_repl --path ./mylibs
```

```
η> (import greeting)
η> (say-hello "REPL")
Hello, REPL!
```

---

## Modules & Imports

### Standard Library Modules

| Module               | Description                              |
|----------------------|------------------------------------------|
| `std.core`           | `atom?`, `compose`, `flip`, `iota`, …    |
| `std.math`           | `pi`, `e`, `even?`, `gcd`, `expt`, …     |
| `std.io`             | `println`, `read-line`, port helpers      |
| `std.collections`    | `filter`, `foldl`, `sort`, `range`, …     |
| `std.test`           | `make-test`, `assert-equal`, `run`, …     |
| `std.prelude`        | Re-exports everything from the above      |

### Writing a module

A module groups definitions behind an explicit `import`/`export`
interface.  Save this as **`greeting.eta`**:

```scheme
(module greeting
  (import std.io)
  (export say-hello)
  (begin
    (defun say-hello (name)
      (println (string-append "Hello, " name "!")))))
```

Run it directly:

```bash
etai greeting.eta
```

### Importing your module from another file

Create **`app.eta`** in the same directory:

```scheme
(module app
  (import greeting)
  (begin
    (say-hello "world")))
```

`etai` auto-adds the input file's directory to the module search path,
so sibling modules are found automatically:

```bash
etai app.eta          # prints: Hello, world!
```

For modules in different directories, use `--path` or `ETA_MODULE_PATH`
(`;`-separated on Windows, `:`-separated on Linux):

```bash
etai --path ./libs app.eta

# or
export ETA_MODULE_PATH=./libs        # Linux
set ETA_MODULE_PATH=.\libs           # Windows
etai app.eta
```

### Import clause variants

Eta supports several ways to control which names are imported:

```scheme
;; Import all exported names
(import greeting)

;; Import only specific names
(import (only std.math pi e))

;; Import everything except certain names
(import (except std.collections sort))

;; Rename on import
(import (rename std.math (pi PI) (e E)))

;; Prefix — all imported names gain a prefix (namespace-style)
(import (prefix std.math math:))
;; now use math:pi, math:even?, math:gcd, etc.
```

The `prefix` clause is particularly useful when two modules export the
same name:

```scheme
(module app
  (import (prefix mod-a a:))
  (import (prefix mod-b b:))
  (begin
    ;; No conflict — each name is qualified
    (a:process data)
    (b:process data)))
```

---

## Using the VS Code Extension

1. Open or create any `.eta` file — syntax highlighting works
   immediately.

2. The extension starts the `eta_lsp` server automatically. It looks
   for the binary in this order:
   - `eta.lsp.serverPath` setting (if configured)
   - `<extension>/bin/eta_lsp` (bundled in the release)
   - Workspace build output directories
   - `PATH`

3. Diagnostics (parse errors, undefined names, etc.) appear in the
   Problems panel as you type.

---


## GitHub Actions CI

The repository includes `.github/workflows/release.yml` which:

1. Builds release bundles for **Linux x86_64** and **Windows x64**
2. Uploads them as workflow artifacts (downloadable from the Actions tab)
3. On tag push (`v*`), creates a **GitHub Release** with both archives

To trigger a release:

```bash
git tag v0.1.0
git push origin v0.1.0
```

To test the workflow without tagging, use **Actions → Release Bundle →
Run workflow** (manual dispatch).

---

## Manual Build Steps

If you prefer not to use the scripts:

### Linux / WSL

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
cmake --install build --prefix dist/eta-linux-x86_64
```

### Windows (MSVC)

```powershell
cmake -B build
cmake --build build --config Release
cmake --install build --prefix dist\eta-win-x64 --config Release
```

---

## Manual stdlib path (development)

When running from a build directory (not the install tree):

```bash
# Via CLI flag
./build/eta/interpreter/etai --path ./stdlib examples/eta/hello.eta

# Or via environment variable
export ETA_MODULE_PATH=./stdlib
./build/eta/interpreter/etai examples/eta/hello.eta
```
