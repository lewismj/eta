# How to Build Your First App

This guide covers two flows:

1. Build and run a single Eta app package.
2. Build an app + library in one workspace (end-to-end).

## Prerequisites

- Eta installed and on `PATH`.
- Tooling check:

```console
eta --version
etai --version
```

## 1) Build your first app

Create a new app package:

```console
eta new hello_app --bin
cd hello_app
```

Edit `src/hello_app.eta`:

```scheme
(module hello_app
  (import std.io)
  (begin
    (defun main (args)
      (println "hello from my first eta app"))))
```

Build and run:

```console
eta build
eta run
```

Expected output:

```text
hello from my first eta app
```

## 2) Build an app with a library (end-to-end)

### Create a workspace with two member packages

```console
mkdir first_workspace
cd first_workspace
eta new mathx --lib
eta new myapp --bin
```

Create a workspace root manifest at `first_workspace/eta.toml`:

```toml
[workspace]
members = ["mathx", "myapp"]
default-members = ["myapp"]
```

### Implement the library package

Edit `mathx/src/mathx.eta`:

```scheme
(module mathx
  (export square cube)
  (import std.math)
  (begin
    (defun square (x) (* x x))
    (defun cube (x) (* x x x))))
```

### Add the library dependency to the app package

```console
cd myapp
eta add mathx --path ../mathx
```

Edit `src/myapp.eta`:

```scheme
(module myapp
  (import std.io)
  (import mathx)
  (begin
    (defun main (args)
      (println (square 7)))))
```

### Build, test, and run from the workspace root

```console
cd ..
eta test --workspace
eta build --workspace
eta run -p myapp
```

Expected output:

```text
49
```

Inspect the resolved dependency graph:

```console
eta tree --workspace
```

In workspace mode, build artifacts are written under:
`.eta/target/<profile>/<member-name>/...` at the workspace root.

## Related

- [Packaging System overview](../packaging.md)
- [Package and Workspace guide](../guide/packages.md)
- [Cookbook end-to-end packaging example](../../cookbook/packaging/end-to-end/README.md)
