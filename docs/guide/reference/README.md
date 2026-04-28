# Reference

[← Back to Language Guide](../language_guide.md)

This folder contains the per-module / per-tool reference documentation.
The [Language Guide](../language_guide.md) is the place to start; each
chapter there links into the relevant pages here.

---

## Language & runtime

| Page                                       | Topic                                            |
| :----------------------------------------- | :----------------------------------------------- |
| [`modules.md`](./modules.md)               | Module system, `import` / `export` clause forms  |
| [`runtime.md`](./runtime.md)               | VM data model, GC, intern table                  |
| [`bytecode-vm.md`](./bytecode-vm.md)       | Bytecode opcode catalogue                        |
| [`compiler.md`](./compiler.md)             | `etac` AOT compiler                              |
| [`nanboxing.md`](./nanboxing.md)           | NaN-boxed value layout                           |
| [`optimisations.md`](./optimisations.md)   | Optimisation pass list                           |
| [`optimization.md`](./optimization.md)     | Heuristics & cost models                         |
| [`finalizers.md`](./finalizers.md)         | Object-lifetime hooks                            |

## Standard-library modules

| Page                                                 | Module                |
| :--------------------------------------------------- | :-------------------- |
| [`logic.md`](./logic.md)                             | `std.logic`           |
| [`clp.md`](./clp.md)                                 | `std.clp` (CLP(FD))   |
| [`clpb.md`](./clpb.md)                               | `std.clpb` (CLP(B))   |
| [`freeze.md`](./freeze.md)                           | `std.freeze`          |
| [`db.md`](./db.md)                                   | `std.db` (Datalog)    |
| [`fact-table.md`](./fact-table.md)                   | `std.fact_table`      |
| [`hashmap.md`](./hashmap.md)                         | `std.hashmap`         |
| [`regex.md`](./regex.md)                             | `std.regex`           |
| [`csv.md`](./csv.md)                                 | `std.csv`             |
| [`time.md`](./time.md)                               | `std.time`            |
| [`stats.md`](./stats.md)                             | `std.stats`           |
| [`aad.md`](./aad.md)                                 | `std.aad`             |
| [`torch.md`](./torch.md)                             | `std.torch`           |
| [`causal.md`](./causal.md)                           | `std.causal`          |
| [`causal-factor.md`](./causal-factor.md)             | Causal factor primer  |
| [`message-passing.md`](./message-passing.md)         | Actor model           |
| [`networking.md`](./networking.md)                   | nng primitives        |
| [`network-message-passing.md`](./network-message-passing.md) | Cross-machine actors |
| [`supervisor.md`](./supervisor.md)                   | `std.supervisor`      |


## Tooling

| Page                       | Tool                                |
| :------------------------- | :---------------------------------- |
| [`repl.md`](./repl.md)     | `eta_repl` interactive prompt       |
| [`vscode.md`](./vscode.md) | VS Code extension                   |
| [`jupyter.md`](./jupyter.md) | `eta_jupyter` kernel              |

## Quantitative finance walkthroughs

| Page                       | Worked example                      |
| :------------------------- | :---------------------------------- |
| [`european.md`](./european.md) | Black–Scholes Greeks via AAD     |
| [`sabr.md`](./sabr.md)     | SABR vol surface, Hagan formula     |
| [`xva.md`](./xva.md)       | CVA / FVA via AAD                   |

For the featured end-to-end engines see
[`featured_examples/portfolio.md`](../../featured_examples/portfolio.md)
and
[`featured_examples/xva-wwr.md`](../../featured_examples/xva-wwr.md).

