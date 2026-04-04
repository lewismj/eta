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
.\install.ps1                 # adds bin\ to user PATH, sets ETA_MODULE_PATH
                              # installs VS Code extension if 'code' is on PATH
```

To install into a custom prefix:

```powershell
.\install.ps1 -Prefix "C:\Program Files\Eta"
```

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
  install.sh / install.ps1    # post-extract installer
  TESTING.md
```

The binaries automatically locate `stdlib/` relative to themselves
(`<exe>/../stdlib/`), so no environment variables are needed when
using the installed layout.

---

## Run from the Command Line

```bash
# Run a script
etai examples/eta/hello.eta

# Interactive REPL
eta_repl
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

## Standard Library Modules

| Module               | Description                              |
|----------------------|------------------------------------------|
| `std.core`           | `atom?`, `compose`, `flip`, `iota`, …    |
| `std.math`           | `pi`, `e`, `even?`, `gcd`, `expt`, …     |
| `std.io`             | `println`, `read-line`, port helpers      |
| `std.collections`    | `filter`, `foldl`, `sort`, `range`, …     |
| `std.test`           | `make-test`, `assert-equal`, `run`, …     |
| `std.prelude`        | Re-exports everything from the above      |

Example user program:

```scheme
(module hello
  (import std.io)
  (begin
    (println "Hello, world!")))
```

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
