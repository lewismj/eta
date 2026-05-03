# How to Build Your First App

This guide covers two flows:

1. Build and run a single Eta app package.
2. Build an app that depends on a local library package (from the end-to-end packaging appendix in [`docs/plan/eta_packaging_plan.md`](../plan/eta_packaging_plan.md)).

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

### Create the library package

```console
eta new mathx --lib
```

Edit `mathx/src/mathx.eta`:

```scheme
(module mathx
  (export square cube)
  (import std.math)
  (begin
    (defun square (x) (* x x))
    (defun cube (x) (* x x x))))
```

Run the library tests:

```console
cd mathx
eta test
cd ..
```

### Create the app package and add the library dependency

```console
eta new myapp --bin
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

Build and run:

```console
eta build
eta run
```

Expected output:

```text
49
```

Inspect the resolved dependency graph:

```console
eta tree
```

## Related

- [Packaging System overview](../packaging.md)
- [Cookbook end-to-end packaging example](../../cookbook/packaging/end-to-end/README.md)
