# Reference

[← Back to Language Guide](../../language_guide.md)

This folder contains the per-module / per-tool reference documentation.
The [Language Guide](../../language_guide.md) is the place to start; each
chapter there links into the relevant pages here.
Use [Guide chapters](../README.md) for tutorial chapter deep dives.
This folder is for module, tool, and runtime/internal reference only.

---

## Language & runtime

| Page                                       | Topic                                            |
| :----------------------------------------- | :----------------------------------------------- |
| [Modules](./modules.md)               | Module system, `import` / `export` clause forms  |
| [Runtime & GC](./runtime.md)               | VM data model, GC, intern table                  |
| [Eval](./eval.md)                     | Runtime expression evaluation with lexical scope  |
| [Bytecode VM](./bytecode-vm.md)       | Bytecode opcode catalogue                        |
| [Compiler](./compiler.md)             | `etac` AOT compiler                              |
| [NaN-Boxing](./nanboxing.md)           | NaN-boxed value layout                           |
| [Optimisations](./optimisations.md)   | Optimisation pass list                           |
| [Eigen Backend](./eigen-backend.md)   | Eigen backend internals used by numeric modules  |
| [Finalizers](./finalizers.md)         | Object-lifetime hooks                            |

## Standard-library modules

| Page                                                 | Module                |
| :--------------------------------------------------- | :-------------------- |
| [Logic](./logic.md)                             | `std.logic`           |
| [CLP(FD)](./clp.md)                                 | `std.clp` (CLP(FD))   |
| [CLP(R)](./clpr.md)                               | `std.clpr` (CLP(R))   |
| [CLP(B)](./clpb.md)                               | `std.clpb` (CLP(B))   |
| [Freeze](./freeze.md)                           | `std.freeze`          |
| [Datalog](./db.md)                                   | `std.db` (Datalog)    |
| [Fact Table](./fact-table.md)                   | `std.fact_table`      |
| [Hash Map](./hashmap.md)                         | `std.hashmap`         |
| [Atom](./atom.md)                               | `std.atom`            |
| [Regex](./regex.md)                             | `std.regex`           |
| [CSV](./csv.md)                                 | `std.csv`             |
| [JSON](./json.md)                               | `std.json`            |
| [Filesystem](./fs.md)                                   | `std.fs`              |
| [OS](./os.md)                                   | `std.os`              |
| [Logging](./log.md)                                 | `std.log`             |
| [Time](./time.md)                               | `std.time`            |
| [Stats](./stats.md)                             | `std.stats`           |
| [AAD](./aad.md)                                 | `std.aad`             |
| [Torch](./torch.md)                             | `std.torch`           |
| [Causal](./causal.md)                           | `std.causal` (+ `.admg`, `.identify`, `.adjustment`, `.mediation`, `.transport`, `.estimate`, `.learn`, `.render`) |
| [Causal Counterfactual](./causal-counterfactual.md) | `std.causal.counterfactual` |
| [Message Passing](./message-passing.md)         | Actor model           |
| [Networking](./networking.md)                   | nng primitives        |
| [Network Message Passing](./network-message-passing.md) | Cross-machine actors |
| [Supervisor](./supervisor.md)                   | `std.supervisor`      |


## Tooling

| Page                       | Tool                                |
| :------------------------- | :---------------------------------- |
| [REPL](./repl.md)     | `eta_repl` interactive prompt       |
| [VS Code](./vscode.md) | VS Code extension                   |
| [Jupyter](./jupyter.md) | `eta_jupyter` kernel              |
| [Profiling](../profiling.md) | Profiler usage (`eta prof`, `std.prof`, `%%prof`, `:prof`) |

## Quantitative finance walkthroughs

| Page                       | Worked example                      |
| :------------------------- | :---------------------------------- |
| [European options](./european.md) | Black–Scholes Greeks via AAD     |
| [SABR](./sabr.md)     | SABR vol surface, Hagan formula     |
| [XVA](./xva.md)       | CVA / FVA via AAD                   |

For the featured end-to-end engines see
[Portfolio](../../featured/portfolio.md)
and
[XVA with WWR](../../featured/xva-wwr.md).
