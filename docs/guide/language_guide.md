# Eta — Language Guide

[← Back to README](../../README.md) ·
[Quick Start](../quickstart.md) ·
[Architecture](../architecture.md) ·
[Modules & Stdlib](./reference/modules.md) ·
[Release Notes](../release-notes.md)

> [!NOTE]
> This guide is the canonical entry point for learning the Eta language.
> Each section is intentionally short and links to a deep-dive page —
> either a tutorial chapter under [`docs/guide/`](.) or a module / tool
> reference under [`docs/guide/reference/`](./reference/).

---

## Contents

1. [Orientation](#1-orientation)
2. [Syntax & Values](#2-syntax--values)
3. [Bindings & Scope](#3-bindings--scope)
4. [Control Flow](#4-control-flow)
5. [Functions, Closures & Tail Calls](#5-functions-closures--tail-calls)
6. [Records & Compound Data](#6-records--compound-data)
7. [Pattern-Style Dispatch](#7-pattern-style-dispatch)
8. [Macros (`syntax-rules`)](#8-macros-syntax-rules)
9. [Modules & Imports](#9-modules--imports)
10. [Error Handling](#10-error-handling)
11. [Strings, Symbols & Regex](#11-strings-symbols--regex)
12. [Collections](#12-collections)
13. [I/O, Filesystem & OS](#13-io-filesystem--os)
14. [Time, Freeze & Finalizers](#14-time-freeze--finalizers)
15. [Logic Programming & Unification](#15-logic-programming--unification)
16. [Constraint Logic Programming](#16-constraint-logic-programming)
17. [Datalog & Fact Tables](#17-datalog--fact-tables)
18. [Causal Inference](#18-causal-inference)
19. [Automatic Differentiation (AAD)](#19-automatic-differentiation-aad)
20. [Statistics & Linear Algebra](#20-statistics--linear-algebra)
21. [libtorch / Neural Networks](#21-libtorch--neural-networks)
22. [Concurrency & Distribution](#22-concurrency--distribution)
23. [Quantitative Finance Examples](#23-quantitative-finance-examples)
24. [Tooling](#24-tooling)
25. [Runtime Internals (overview)](#25-runtime-internals-overview)
26. [Examples Index](#26-examples-index)
27. [Further Reading](#27-further-reading)

---

## 1. Orientation

Eta is a Lisp-1 with an S-expression surface, a hygienic macro system,
and a stack-based bytecode VM that uses NaN-boxing for value
representation. The same VM hosts the symbolic core, native logic
programming with backtracking, reverse-mode automatic differentiation,
linear algebra (Eigen), neural networks (libtorch), and an actor runtime
(nng).

### Toolchain

| Tool          | Role                                                | Reference |
| :------------ | :-------------------------------------------------- | :-------- |
| `etai`        | Interpreter for `.eta` source or `.etac` bytecode   | [Quick Start](../quickstart.md) |
| `etac`        | Ahead-of-time bytecode compiler                     | [Compiler](./reference/compiler.md), [Bytecode Tools](./bytecode-and-tools.md) |
| `eta_repl`    | Interactive REPL                                    | [REPL](./reference/repl.md) |
| `eta_lsp`     | Language Server (diagnostics, completion, navigation) | [VS Code](./reference/vscode.md), [Debugging](./debugging.md) |
| `eta_dap`     | Debug Adapter (breakpoints, stepping, inspection)   | [Debugging](./debugging.md) |
| `eta_test`    | Test runner with TAP / JUnit output                 | [Testing](./testing.md) |
| `eta_jupyter` | Jupyter kernel                                      | [Jupyter](./reference/jupyter.md) |

### Hello, world

```scheme
(module hello
  (import std.io)
  (begin
    (println "Hello, world!")))
```

```bash
etai hello.eta
```

> [!TIP]
> The interactive notebook at
> [`examples/notebooks/LanguageBasics.ipynb`](../../examples/notebooks/LanguageBasics.ipynb)
> walks through the same material as §§ 2–9 in a runnable form.

### How to read this guide

Each chapter is short, with a small example and a *Deep dive* link. Read
the chapters in order for a tour, or jump straight to the deep dive of
the topic you need.

---

## 2. Syntax & Values

Eta source is S-expressions: lists `( … )`, vectors `#( … )`, strings
`"…"`, characters `#\a`, booleans `#t` / `#f`, symbols, fixnums and
floats. Comments are `;` to end of line, or `#| … |#` for block
comments. Quoting uses `'`, quasi-quotation `` ` ``, unquote `,`, and
splicing `,@`.

```scheme
'(1 2 3)                ; list literal
#(1 2 3)                ; vector literal
`(a ,(+ 1 2) ,@'(c d))  ; => (a 3 c d)
```

Equality has three flavours: `eq?` (identity), `eqv?` (numeric/char
equivalence), `equal?` (structural). The numeric tower is fixnum +
double, with NaN-box tagging — see [`nanboxing.md`](./reference/nanboxing.md).

> **Deep dive:** [`syntax-and-values.md`](./syntax-and-values.md).

---

## 3. Bindings & Scope

Eta is lexically scoped. The binding forms are `define`, `defun` (a
`define` + `lambda` shorthand), `let`, `let*`, `letrec`, named-`let`,
and `set!`. Internal `define`s inside a `lambda`/`let` are mutually
recursive.

```scheme
(define greeting "hello")

(let* ((a 1)
       (b (+ a 1))
       (c (+ a b)))
  c)                          ; => 3

(let loop ((n 10) (acc 1))    ; named let
  (if (= n 0) acc (loop (- n 1) (* acc n))))
```

> [!NOTE]
> The REPL allows shadowing (re-`define` of an existing name); modules
> do not. See [`repl.md`](./reference/repl.md) for the REPL-specific rules.

> **Deep dive:** [`bindings-and-scope.md`](./bindings-and-scope.md).

---

## 4. Control Flow

| Form               | Purpose                                               |
| :----------------- | :---------------------------------------------------- |
| `if`               | Two-branch conditional                                |
| `cond`             | Multi-branch with optional `else`                     |
| `when` / `unless`  | One-armed conditional with implicit `begin`           |
| `case`             | Dispatch on `eqv?` of a key                           |
| `and` / `or`       | Short-circuit, return the deciding value              |
| `begin`            | Sequence expressions, return the last                 |

Loops are recursive: use named `let` for tight loops and `letrec` for
mutual recursion. All such forms are tail-call optimised — see §5.

```scheme
(cond
  ((null? xs)        'empty)
  ((= (length xs) 1) 'singleton)
  (else              'many))
```

> **Deep dive:** [`control-flow.md`](./control-flow.md).

---

## 5. Functions, Closures & Tail Calls

`lambda` constructs a closure; `defun` is shorthand for
`(define name (lambda …))`. Parameter lists support fixed, optional and
dotted-rest arguments, and `apply` calls a function with a list of
arguments.

```scheme
(defun adder (n) (lambda (x) (+ n x)))   ; closure over n
(define add5 (adder 5))
(add5 3)                                 ; => 8

(defun sum (first . rest) (foldl + first rest))
(apply sum '(1 2 3 4))                   ; => 10
```

Eta guarantees tail-call optimisation in tail position — including the
last expression of `if`, `cond`, `when`, `unless`, `case`, `let*`,
`letrec`, and `begin`. Mutual recursion via `letrec` runs in constant
stack.

> **Deep dives:** [`functions-and-closures.md`](./functions-and-closures.md),
> [`tail-calls.md`](./tail-calls.md).

---

## 6. Records & Compound Data

`define-record-type` produces a constructor, a predicate, accessors and
optional setters. Records are not pairs; `equal?` performs structural
comparison.

```scheme
(define-record-type <point>
  (make-point x y)
  point?
  (x point-x)
  (y point-y set-point-y!))

(define p (make-point 3 4))
(point-x p)                ; => 3
(set-point-y! p 7)
```

Other compound types: pairs / lists, vectors (`#( … )`, fixed-length,
mutable, O(1) indexed), hash maps and hash sets — see §12.

---

## 7. Pattern-Style Dispatch

Eta has no built-in `match` form. Idiomatic dispatch uses `cond` with
predicate guards or, for symbolic data, structural unification from
`std.logic` (§15).

```scheme
(defun shape-area (s)
  (cond
    ((and (pair? s) (eq? (car s) 'circle))
       (let ((r (cadr s))) (* 3.14159 r r)))
    ((and (pair? s) (eq? (car s) 'rect))
       (* (cadr s) (caddr s)))
    (else (raise 'unknown-shape s))))
```

For destructuring on relational data, use `(== pat term)` with logic
variables — see [`logic.md`](./reference/logic.md).

---

## 8. Macros (`syntax-rules`)

Macros are hygienic and pattern-based. `define-syntax` binds an
expander; `syntax-rules` lists `(pattern → template)` cases with
ellipsis (`...`) for variadic patterns.

```scheme
(define-syntax swap!
  (syntax-rules ()
    ((_ a b)
     (let ((tmp a))
       (set! a b)
       (set! b tmp)))))
```

> [!IMPORTANT]
> Eta's macro system is `syntax-rules` only — there are no procedural
> macros. This keeps expansion deterministic and serialisable into
> bytecode. See [`macros.md`](./macros.md) for ellipses, literal
> keywords, and worked examples from the standard library.

> **Deep dive:** [`macros.md`](./macros.md).

---

## 9. Modules & Imports

Every source file declares one or more `(module name … )` forms with
explicit `(import …)` and `(export …)` clauses. The module search path
is the input file's directory plus `--path` arguments and
`ETA_MODULE_PATH`.

| Clause                                | Effect                                  |
| :------------------------------------ | :-------------------------------------- |
| `(import std.math)`                   | All exports of a module                 |
| `(import (only std.math pi e))`       | Only listed names                       |
| `(import (except std.collections sort))` | All except listed names              |
| `(import (rename std.math (pi PI)))`  | Rename on import                        |
| `(import (prefix std.math math:))`    | Namespace-style qualified access        |

> **Reference:** [`modules.md`](./reference/modules.md).

---

## 10. Error Handling

`raise` and `catch` compile to the `Throw` and `SetupCatch` VM opcodes.
Tags are symbols; the raised payload can be any value. A tag-less
`(catch body)` is a catch-all that also intercepts `runtime.*` errors.

```scheme
(defun safe-div (a b)
  (if (= b 0)
      (raise 'division-by-zero (list a b))
      (/ a b)))

(catch 'division-by-zero (safe-div 10 0))
;; => (10 0)
```

`dynamic-wind` runs its *after* thunk on every exit (normal or
exceptional), enabling reliable cleanup.

> **Deep dive:** [`error-handling.md`](./error-handling.md).

---

## 11. Strings, Symbols & Regex

Strings are immutable byte sequences with the standard Scheme operations
(`string-append`, `string-length`, `substring`, `string-ref`,
`string->list`, `string->symbol`, …). Symbols are interned. Regular
expressions live in `std.regex`.

```scheme
(import std.regex)
(define re (regex:compile "(\\d+)-(\\d+)"))
(regex:find-all re "10-20 and 30-40")
;; => (("10-20" "10" "20") ("30-40" "30" "40"))
```

> **Deep dive:** [`strings.md`](./strings.md). **Reference:** [`regex.md`](./reference/regex.md).

---

## 12. Collections

| Container  | Mutability  | Indexing | Module                        |
| :--------- | :---------- | :------- | :---------------------------- |
| List       | immutable   | O(n)     | builtin                       |
| Vector     | mutable     | O(1)     | builtin                       |
| Hash map   | mutable     | O(1) avg | [`std.hashmap`](./reference/hashmap.md) |
| Hash set   | mutable     | O(1) avg | `std.hashset`                 |
| Fact table | columnar    | indexed  | [`std.fact_table`](./reference/fact-table.md) |

`std.collections` provides the higher-order suite (`map*`, `filter`,
`foldl` / `foldr`, `reduce`, `zip`, `range`, `take` / `drop`,
`flatten`, `sort`, `any?`, `every?`, …).

```scheme
(import std.prelude)
(foldl + 0 (map* (lambda (x) (* x x)) (filter even? (range 1 11))))
;; => 220
```

> **Deep dive:** [`collections.md`](./collections.md).

---

## 13. I/O, Filesystem & OS

Built-ins: `display`, `write`, `newline`, `write-string`, `read-char`,
`current-{input,output,error}-port`, `open-input-file`,
`open-output-file`, `open-input-string`, `open-output-string`,
`get-output-string`. `std.io` adds `println`, `eprintln`, `read-line`,
`display->string`, and the `with-…-port` redirection helpers.

```scheme
(import std.io)
(with-output-to-port (open-output-string)
  (lambda () (println "captured")))
```

CSV via [`std.csv`](./reference/csv.md), Datalog via [`std.db`](./reference/db.md).

### Filesystem (`std.fs`)

`std.fs` wraps the native `std::filesystem`-backed builtins for path
manipulation, directory enumeration, and file metadata. Paths are
plain strings; results round-trip through the platform-preferred
separator.

```scheme
(import std.fs std.io)
(when (fs:directory? "examples")
  (for-each println (fs:list-directory "examples")))

(define cfg (fs:path-join (fs:temp-directory) "eta" "config.json"))
(println (fs:path-normalize cfg))
```

| Function                     | Purpose                                              |
| :--------------------------- | :--------------------------------------------------- |
| `fs:file-exists?`            | `#t` iff the path resolves on disk                   |
| `fs:directory?`              | `#t` iff the path is a directory                     |
| `fs:delete-file`             | Remove a regular file                                |
| `fs:make-directory`          | Create a directory (idempotent)                     |
| `fs:list-directory`          | Sorted list of entry names (no `.`/`..`)            |
| `fs:path-join`               | Variadic; joins with the platform separator          |
| `fs:path-split`              | Inverse of `fs:path-join` (root + components)        |
| `fs:path-normalize`          | Lexical canonicalisation                             |
| `fs:temp-file`               | Allocate a fresh temp-file path                      |
| `fs:temp-directory`          | Allocate a fresh temp-directory path                 |
| `fs:file-modification-time`  | mtime in epoch milliseconds                          |
| `fs:file-size`               | Size in bytes                                        |

### Operating system (`std.os`)

`std.os` exposes process-level concerns: environment variables, the
script's own command line, current working directory, and a clean
`exit`.

```scheme
(import std.os std.io)
(println (os:command-line-arguments))            ; e.g. ("--verbose" "in.csv")
(println (or (os:getenv "ETA_HOME") "(unset)"))
(os:change-directory! (os:current-directory))
```

| Function                         | Purpose                                            |
| :------------------------------- | :------------------------------------------------- |
| `os:getenv`                      | Lookup; `#f` if unset                              |
| `os:setenv!` / `os:unsetenv!`    | Mutate the process environment                    |
| `os:environment-variables`       | Sorted alist of `("KEY" . "value")`               |
| `os:command-line-arguments`      | List of strings passed to `etai` / `etac`         |
| `os:current-directory`           | Working directory as a string                      |
| `os:change-directory!`           | `chdir` equivalent                                |
| `os:exit`                        | Terminate the process with an optional status code |

> **Deep dive:** [`io.md`](./io.md). **References:**
> [`fs.md`](./reference/fs.md), [`os.md`](./reference/os.md).

---

## 14. Time, Freeze & Finalizers

`std.time` exposes `time:now-ms`, `time:monotonic-ms`, `time:sleep-ms`,
`time:utc-parts`, `time:format-iso8601-utc`, `time:elapsed-ms` — see
[`time.md`](./reference/time.md).

`std.freeze` provides two attributed-variable combinators that compose
with the logic engine:

| Form                | Meaning                                                       |
| :------------------ | :------------------------------------------------------------ |
| `(freeze v thunk)`  | Run `thunk` when logic var `v` becomes ground                 |
| `(dif x y)`         | Structural disequality; succeeds iff `x` and `y` cannot unify |

See [`freeze.md`](./reference/freeze.md) and [`finalizers.md`](./reference/finalizers.md)
for object-lifetime hooks.

---

## 15. Logic Programming & Unification

Logic variables (`logic-var`), structural unification (`==`), and the
search combinators (`findall`, `run1`, `succeeds?`, `naf`) are first
class. Backtracking is implemented by a trail managed at the VM level —
exception handling and CLP propagation compose with it cleanly.

```scheme
(import std.logic)

(define parent-db
  '((tom bob) (tom liz) (bob ann) (bob pat) (pat jim)))

(defun parento (p c)
  (map* (lambda (f) (lambda () (and (== p (car f)) (== c (cadr f)))))
        parent-db))

(let ((c (logic-var)))
  (findall (lambda () (deref-lvar c)) (parento 'tom c)))
;; => (bob liz)
```

> **Reference:** [`logic.md`](./reference/logic.md).

---

## 16. Constraint Logic Programming

Three CLP domains are bundled:

| Domain  | Module        | Reference |
| :------ | :------------ | :-------- |
| CLP(FD) | `std.clp`     | [`clp.md`](./reference/clp.md) |
| CLP(B)  | `std.clpb`    | [`clpb.md`](./reference/clpb.md) |
| CLP(R)  | `std.clpr`    | (interval API: `clp:real`, `clp:r<=`, `clp:r-minimize`, `clp:rq-minimize` …) |

```scheme
(import std.clp)
(let ((vars (list (clp:var) (clp:var) (clp:var))))
  (for-each (lambda (v) (clp:domain v 1 9)) vars)
  (clp:all-different vars)
  (clp:solve vars))
```

`std.clpr` exposes interval domains, linear and quadratic
minimise/maximise routines backed by the Fourier–Motzkin oracle. See
[`examples/portfolio-lp.eta`](../../examples/portfolio-lp.eta) for a
worked LP.

---

## 17. Datalog & Fact Tables

`std.db` provides Datalog-style relations with `defrel`, `assert!`, and
tabled evaluation — see [`db.md`](./reference/db.md). `std.fact_table` is a
columnar store with hash-indexed lookups for analytics workloads — see
[`fact-table.md`](./reference/fact-table.md) and
[`examples/fact-table.eta`](../../examples/fact-table.eta).

---

## 18. Causal Inference

`std.causal` implements Pearl's do-calculus on user-defined DAGs:
back-door / front-door identification, instrumental variables, and
conditional ATE estimation. Identification is symbolic; estimation
plugs into any conditional-expectation oracle (closed form, regression,
or a libtorch model).

```scheme
(import std.causal)
(define dag (causal:dag '((Z X) (X Y) (Z Y))))
(do:identify dag 'X 'Y)        ; => back-door adjustment formula
```

> **References:** [`causal.md`](./reference/causal.md),
> [`causal-factor.md`](./reference/causal-factor.md);
> example: [`examples/causal_demo.eta`](../../examples/causal_demo.eta).

---

## 19. Automatic Differentiation (AAD)

Reverse-mode AD with a tape recorded directly by the VM — no closure
allocation per arithmetic op. `grad` returns `(value gradient-vector)`
in a single backward sweep over the tape.

```scheme
(import std.aad)
(grad (lambda (x y) (+ (* x y) (sin x))) '(2 3))
;; => (8.909... #(2.583... 2))
```

Helpers for AD-safe primitives (`ad-abs`, `softplus`, `relu`,
`check-grad`) and tape introspection live in `std.aad`.

> **Reference:** [`aad.md`](./reference/aad.md).

---

## 20. Statistics & Linear Algebra

`std.stats` provides descriptive statistics, OLS multi-regression, PCA,
and distribution functions, backed by Eigen for dense linear algebra.
The Eigen layer is currently exposed only through `std.stats` and
`std.torch` — there is no separate user-facing module.

> **Reference:** [`stats.md`](./reference/stats.md).

---

## 21. libtorch / Neural Networks

`std.torch` wraps libtorch tensors, autograd, the `nn` module suite,
optimisers, and (when built with CUDA) device transfer.

```scheme
(import std.torch)
(define x (torch:tensor '((1.0 2.0) (3.0 4.0)) '(:requires-grad #t)))
(define y (torch:matmul x (torch:transpose x 0 1)))
(torch:backward (torch:sum y))
(torch:grad x)
```

> **Reference:** [`torch.md`](./reference/torch.md);
> tests: [`examples/torch_tests/`](../../examples/torch_tests/).

---

## 22. Concurrency & Distribution

Eta's actor model is built on **nng**: every actor owns a mailbox
socket; messages are arbitrary Eta values serialised by the runtime.
The same `send!` / `recv!` API works for in-process threads, OS
processes, and remote TCP peers.

| Primitive             | Use                                                   |
| :-------------------- | :---------------------------------------------------- |
| `(spawn module-path)` | Fork a child process running the named module         |
| `(spawn-thread thunk)`| Run a closure in a fresh in-process VM thread         |
| `(current-mailbox)`   | Child-side handle to the parent / spawner             |
| `(send! sock v 'wait)`| Block until message is sent                           |
| `(recv! sock 'wait)`  | Block until a message arrives                         |
| `(monitor sock)`      | Receive a `(down …)` message when the peer dies       |

High-level patterns provided by `std.net`: `worker-pool`,
`request-reply`, `survey`, PUB/SUB. Supervision trees (`one-for-one`,
`one-for-all`) live in `std.supervisor`.

> **References:** [`message-passing.md`](./reference/message-passing.md),
> [`networking.md`](./reference/networking.md),
> [`network-message-passing.md`](./reference/network-message-passing.md),
> [`supervisor.md`](./reference/supervisor.md).

---

## 23. Quantitative Finance Examples

| Example                                                        | Topic                                  | Walkthrough |
| :------------------------------------------------------------- | :------------------------------------- | :---------- |
| [`european.eta`](../../examples/european.eta)                  | Black–Scholes Greeks via AAD           | [`european.md`](./reference/european.md) |
| [`sabr.eta`](../../examples/sabr.eta)                          | SABR vol surface, Hagan approximation  | [`sabr.md`](./reference/sabr.md) |
| [`xva.eta`](../../examples/xva.eta)                            | CVA / FVA sensitivities via AAD        | [`xva.md`](./reference/xva.md) |
| [`xva-wwr/`](../../examples/xva-wwr/)                          | Wrong-Way Risk via do-interventions    | [`featured_examples/xva-wwr.md`](../featured_examples/xva-wwr.md) |
| [`portfolio.eta`](../../examples/portfolio.eta)                | Causal portfolio engine (full pipeline)| [`featured_examples/portfolio.md`](../featured_examples/portfolio.md) |
| [`portfolio-lp.eta`](../../examples/portfolio-lp.eta)          | LP variant via `std.clpr`              | [CLP(R)](./clpr.md) |
| [`fact-table.eta`](../../examples/fact-table.eta)              | Columnar fact tables                   | [`fact-table.md`](./reference/fact-table.md) |

---

## 24. Tooling

| Topic                        | Reference                                              |
| :--------------------------- | :----------------------------------------------------- |
| REPL                         | [`repl.md`](./reference/repl.md)                                |
| Compiler (`etac`)            | [`compiler.md`](./reference/compiler.md), [`bytecode-and-tools.md`](./bytecode-and-tools.md) |
| Bytecode VM                  | [`bytecode-vm.md`](./reference/bytecode-vm.md)                  |
| LSP / DAP / VS Code          | [`vscode.md`](./reference/vscode.md), [`debugging.md`](./debugging.md) |
| Jupyter kernel               | [`jupyter.md`](./reference/jupyter.md)                          |
| Testing (`std.test`, `eta_test`) | [`testing.md`](./testing.md), [`std_lib_tests.md`](../std_lib_tests.md) |

---

## 25. Runtime Internals (overview)

<details>
<summary><b>NaN-boxing</b></summary>

All Eta values fit in a 64-bit double-NaN payload: fixnums and small
immediates are encoded directly; heap objects (pairs, vectors,
strings, closures, records, …) use tagged pointers. See
[`nanboxing.md`](./reference/nanboxing.md).
</details>

<details>
<summary><b>Bytecode VM</b></summary>

A stack-based VM with explicit `Call` / `TailCall`, `SetupCatch` /
`Throw`, and unification opcodes. See [`bytecode-vm.md`](./reference/bytecode-vm.md)
and [`runtime.md`](./reference/runtime.md).
</details>

<details>
<summary><b>Garbage collector</b></summary>

Mark-and-sweep over the VM heap with explicit GC roots from the value
stack, frame stack, intern table, and registered finalizer set. See
[`runtime.md`](./reference/runtime.md), [`finalizers.md`](./reference/finalizers.md).
</details>

<details>
<summary><b>Optimisations</b></summary>

`etac -O` runs constant folding, dead-code elimination, peephole
opcode fusion, and known-call inlining. See
[`optimisations.md`](./reference/optimisations.md) and
[`optimization.md`](./reference/optimization.md).
</details>

For the architectural overview, read [`architecture.md`](../architecture.md).

---

## 26. Examples Index

A curated walk through everything in [`examples/`](../../examples/):
beginner programs, symbolic & logic, AAD & finance, concurrency, causal
& portfolio engines, plus the notebook collection.

> **Tour:** [`examples-tour.md`](./examples-tour.md).

---

## 27. Further Reading

- [`architecture.md`](../architecture.md) — pipeline overview
- [`eta_plan.md`](../eta_plan.md) — language roadmap
- [`next-steps.md`](../next-steps.md) — short-term work items
- [`release-notes.md`](../release-notes.md) — version history
- [`std_lib_tests.md`](../std_lib_tests.md) — stdlib test layout
- [`hash_map_plan.md`](../hash_map_plan.md) — hash-map design notes

