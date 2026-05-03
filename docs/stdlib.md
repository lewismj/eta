# Eta Standard Library Reference

Reference documentation for the modules under `stdlib/std/`. Each page lists
the module's exported symbols with short descriptions and call shapes. For
tutorials and longer examples see the [Language Guide](language_guide.md).

## Conventions

- Most non-pure modules expose a colon-prefixed namespace
  (`csv:read-row`, `log:info`, `clp:domain`, ...). The colon is part of the
  symbol name; it is not a Scheme operator.
- A few modules also export bare aliases (for example `std.atom` exports both
  `atom:deref` and `deref`).
- Names ending in `!` mutate state. Names ending in `?` return a boolean.
- Functions documented with `. opts` accept a trailing key/value list.
- Modules wrap runtime built-ins prefixed `%` (for example `%csv-*`,
  `%regex-*`). Those built-ins are implementation detail and are not
  documented here.
- Some modules require optional build flags:
  - `std.net`, `std.supervisor` need `-DETA_BUILD_NNG=ON`.
  - `std.torch` needs `-DETA_BUILD_TORCH=ON`.

## Modules

### Core / language

- [std.core](stdlib/core.md) - Combinators, accessors, platform predicate.
- [std.collections](stdlib/collections.md) - Higher-order list and vector ops.
- [std.math](stdlib/math.md) - Math constants and numeric helpers.
- [std.io](stdlib/io.md) - I/O conveniences and port redirection.

### Data structures

- [std.atom](stdlib/atom.md) - Atomic mutable references with CAS.
- [std.hashmap](stdlib/hashmap.md) - Hash-map helpers.
- [std.hashset](stdlib/hashset.md) - Hash-set helpers.
- [std.fact_table](stdlib/fact_table.md) - Columnar fact tables.

### Logic and constraints

- [std.logic](stdlib/logic.md) - miniKanren-style goal combinators.
- [std.freeze](stdlib/freeze.md) - Attributed-variable combinators.
- [std.clp](stdlib/clp.md) - CLP(Z) and CLP(FD) over integers.
- [std.clpb](stdlib/clpb.md) - CLP(B) Boolean propagation.
- [std.clpr](stdlib/clpr.md) - CLP(R) over real intervals with LP/QP.
- [std.db](stdlib/db.md) - Datalog-style relations on fact tables.

### Causal inference

- [std.causal](stdlib/causal.md) - DAG operations and back-door identification.
- [std.causal.adjustment](stdlib/causal/adjustment.md) - GAC, front-door, IV.
- [std.causal.admg](stdlib/causal/admg.md) - ADMG helpers and latent projection.
- [std.causal.identify](stdlib/causal/identify.md) - ID/IDC algorithms.
- [std.causal.counterfactual](stdlib/causal/counterfactual.md) - Twin networks, ETT.
- [std.causal.mediation](stdlib/causal/mediation.md) - NDE, NIE, CDE.
- [std.causal.estimate](stdlib/causal/estimate.md) - ATE, IPW, AIPW, TMLE.
- [std.causal.cate](stdlib/causal/cate.md) - Meta-learners (S/T/X/R/DR).
- [std.causal.crossfit](stdlib/causal/crossfit.md) - Cross-fitting and DML.
- [std.causal.forest](stdlib/causal/forest.md) - Causal forests via DR scores.
- [std.causal.learn](stdlib/causal/learn.md) - Structure learning (PC, NOTEARS).
- [std.causal.policy](stdlib/causal/policy.md) - Uplift and policy values.
- [std.causal.render](stdlib/causal/render.md) - DOT, Mermaid, LaTeX rendering.
- [std.causal.transport](stdlib/causal/transport.md) - Transportability.

### Statistics and machine learning

- [std.stats](stdlib/stats.md) - Descriptive stats, t-tests, OLS.
- [std.aad](stdlib/aad.md) - Reverse-mode AD helpers.
- [std.torch](stdlib/torch.md) - libtorch tensors, NN, optimizers.
- [std.ml.tree](stdlib/ml/tree.md) - Regression CART.
- [std.ml.forest](stdlib/ml/forest.md) - Bagged regression forests.

### Systems and I/O

- [std.fs](stdlib/fs.md) - Filesystem and path helpers.
- [std.os](stdlib/os.md) - Environment, argv, working directory.
- [std.process](stdlib/process.md) - Subprocess management.
- [std.time](stdlib/time.md) - Clocks, sleep, ISO-8601 formatting.
- [std.regex](stdlib/regex.md) - Regular expressions.
- [std.csv](stdlib/csv.md) - CSV reader/writer.
- [std.json](stdlib/json.md) - JSON read/write.
- [std.log](stdlib/log.md) - Structured logging.
- [std.args](stdlib/args.md) - Command-line argument parsing.

### Concurrency and networking

- [std.net](stdlib/net.md) - Networking patterns over NNG.
- [std.supervisor](stdlib/supervisor.md) - Actor supervision strategies.

### Tooling

- [std.test](stdlib/test.md) - Unit tests with TAP/JUnit reporters.
- [std.jupyter](stdlib/jupyter.md) - Notebook MIME display markers.

