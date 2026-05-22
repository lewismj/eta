# Standard Library Reference

[← Language Guide](../../language_guide.md) ·
[Guide Chapters](../README.md) ·
[Detailed Reference](../reference/README.md)

This is the complete Eta standard library. Every module listed here ships with
the distribution and is importable with `(import <name>)`.

---

## Foundations

| Module | Import | Description |
| :----- | :----- | :---------- |
| [std.core](./core.md) | `(import std.core)` | Core combinators, list accessors, and a platform predicate. |
| [std.math](./math.md) | `(import std.math)` | Mathematical constants and common numeric helpers. |
| [std.collections](./collections.md) | `(import std.collections)` | Higher-order operations on lists and vectors. |
| [std.io](./io.md) | `(import std.io)` | I/O conveniences and dynamic port redirection. |
| [std.atom](./atom.md) | `(import std.atom)` | Atomic mutable references with compare-and-set semantics. |

---

## Strings, Regex & Data Formats

| Module | Import | Description |
| :----- | :----- | :---------- |
| [std.regex](./regex.md) | `(import std.regex)` | Regular-expression helpers and match-payload accessors. |
| [std.csv](./csv.md) | `(import std.csv)` | CSV reader and writer. |
| [std.json](./json.md) | `(import std.json)` | JSON read/write. |

---

## Collections & Data Structures

| Module | Import | Description |
| :----- | :----- | :---------- |
| [std.hashmap](./hashmap.md) | `(import std.hashmap)` | Helpers built over the runtime hash-map primitives. |
| [std.hashset](./hashset.md) | `(import std.hashset)` | Helpers built over the runtime hash-set primitives. |
| [std.fact_table](./fact_table.md) | `(import std.fact_table)` | Columnar fact tables with indexes, grouping, and aggregation. |

---

## Logic & Constraint Programming

| Module | Import | Description |
| :----- | :----- | :---------- |
| [std.logic](./logic.md) | `(import std.logic)` | Prolog/miniKanren-style goal combinators and solvers. |
| [std.freeze](./freeze.md) | `(import std.freeze)` | Attributed-variable combinators (`freeze`, `dif`). |
| [std.clp](./clp.md) | `(import std.clp)` | CLP over integers and finite domains with labelling. |
| [std.clpr](./clpr.md) | `(import std.clpr)` | CLP over real intervals with linear and quadratic optimisation. |
| [std.clpb](./clpb.md) | `(import std.clpb)` | CLP(B) Boolean propagation solver. |
| [std.db](./db.md) | `(import std.db)` | Datalog/Prolog-style relations layered on fact tables. |

---

## Numerics & Machine Learning

| Module | Import | Description |
| :----- | :----- | :---------- |
| [std.stats](./stats.md) | `(import std.stats)` | Descriptive statistics, t-tests, and OLS regression. |
| [std.aad](./aad.md) | `(import std.aad)` | Tape-based reverse-mode automatic differentiation. |
| [std.torch](./torch.md) | `(import std.torch)` | libtorch tensors, modules, optimizers, and device management. *(requires `-DETA_BUILD_TORCH=ON`)* |

---

## Causal Inference

| Module | Import | Description |
| :----- | :----- | :---------- |
| [std.causal](./causal.md) | `(import std.causal)` | DAG queries, do-calculus, back-door identification, and effect estimation. |
| [std.causal.adjustment](./causal/adjustment.md) | `(import std.causal.adjustment)` | GAC, front-door criterion, IV adjustment. |
| [std.causal.identify](./causal/identify.md) | `(import std.causal.identify)` | ID and IDC algorithms over ADMGs. |
| [std.causal.estimate](./causal/estimate.md) | `(import std.causal.estimate)` | Modern ATE estimators (IPW, AIPW, DML). |
| [std.causal.learn](./causal/learn.md) | `(import std.causal.learn)` | Structure learning from data. |
| [std.causal.counterfactual](./causal/counterfactual.md) | `(import std.causal.counterfactual)` | Counterfactual queries under SCMs. |
| [std.causal.mediation](./causal/mediation.md) | `(import std.causal.mediation)` | Natural direct and indirect effects. |
| [std.causal.transport](./causal/transport.md) | `(import std.causal.transport)` | Transportability of causal effects. |
| [std.causal.admg](./causal/admg.md) | `(import std.causal.admg)` | ADMG (acyclic directed mixed graph) operations. |
| [std.causal.render](./causal/render.md) | `(import std.causal.render)` | Graph and DAG rendering helpers. |

---

## OS, Filesystem & Processes

| Module | Import | Description |
| :----- | :----- | :---------- |
| [std.os](./os.md) | `(import std.os)` | Environment variables, working directory. |
| [std.fs](./fs.md) | `(import std.fs)` | Filesystem and path primitives. |
| [std.process](./process.md) | `(import std.process)` | Subprocess spawn, wait, and I/O. |
| [std.args](./args.md) | `(import std.args)` | Argparse-style command-line parser. |

---

## Concurrency & Actors

| Module | Import | Description |
| :----- | :----- | :---------- |
| [std.actor](./actor.md) | `(import std.actor)` | Local PID/mailbox actor runtime wrappers. |
| [std.actor.node](./actor-node.md) | `(import std.actor.node)` | Distributed node handshake/routing for actor PIDs over NNG. |
| [std.actor.gen_server](./actor-gen-server.md) | `(import std.actor.gen_server)` | OTP-style server behaviour (`start`, `call`, `cast`, `stop`). |
| [std.actor.supervisor](./actor-supervisor.md) | `(import std.actor.supervisor)` | Local actor supervisor helpers (strategies, child specs, restart intensity). |
| [std.net](./net.md) | `(import std.net)` | High-level networking patterns over NNG. *(requires `-DETA_BUILD_NNG=ON`)* |
| [std.supervisor](./supervisor.md) | `(import std.supervisor)` | Compatibility shim that re-exports `std.actor.supervisor`. |

---

## Tooling & Development

| Module | Import | Description |
| :----- | :----- | :---------- |
| [std.log](./log.md) | `(import std.log)` | Structured logger and sink construction. |
| [std.test](./test.md) | `(import std.test)` | Unit-testing framework with assertions and TAP/JUnit reporters. |
| [std.prof](./prof.md) | `(import std.prof)` | Runtime profiling helpers. |
| [std.time](./time.md) | `(import std.time)` | Clocks, sleep, and ISO-8601 formatting. |
| [std.jupyter](./jupyter.md) | `(import std.jupyter)` | Notebook cell rendering helpers. *(requires `eta_jupyter`)* |

---

## See also

- [Detailed per-module reference](../reference/README.md) — extended explanations, recipes, and examples for many modules.
- [Language Guide](../../language_guide.md) — start here if you are new to Eta.
- [stdlib source](../../../stdlib/std/) — the Eta source files for every module above.

