# Examples Tour

[← Back to Language Guide](./language_guide.md)

A guided reading order through every program in
[`examples/`](../../examples/). Each entry gives a one-line summary and
a link to the file (and to the in-depth doc where one exists). 

---

## Reading order

If you are new to Eta, work through the **Beginner** and **Symbolic &
Logic** sections first. The **AAD & Finance** and **Causal & Portfolio**
sections build on each other and culminate in
[`portfolio.eta`](../../examples/portfolio.eta).

---

## Beginner

| Example                                                | Topic                                                |
| :----------------------------------------------------- | :--------------------------------------------------- |
| [`hello.eta`](../../examples/hello.eta)                | First program: `module`, `import`, `defun`, recursion |
| [`basics.eta`](../../examples/basics.eta)              | Values, bindings, `cond`, records, quoting           |
| [`functions.eta`](../../examples/functions.eta)        | `defun`, `lambda`, closures, variadics, `letrec`     |
| [`higher-order.eta`](../../examples/higher-order.eta)  | `map*`, `filter`, `foldl`, `zip`, `range`            |
| [`composition.eta`](../../examples/composition.eta)    | `compose`, `flip`, `negate`, manual currying         |
| [`recursion.eta`](../../examples/recursion.eta)        | Fibonacci, deep flatten, Ackermann, Hanoi            |
| [`exceptions.eta`](../../examples/exceptions.eta)      | `catch` / `raise`, `dynamic-wind`, structured payloads |

---

## Symbolic & Logic

| Example                                                          | Topic                                              |
| :--------------------------------------------------------------- | :------------------------------------------------- |
| [`boolean-simplifier.eta`](../../examples/boolean-simplifier.eta) | Tree rewriting, De Morgan, fixed-point             |
| [`symbolic-diff.eta`](../../examples/symbolic-diff.eta)          | Computer algebra: differentiation + simplification |
| [`unification.eta`](../../examples/unification.eta)              | Raw `logic-var`, `==`, `findall`                   |
| [`logic.eta`](../../examples/logic.eta)                          | Relations: `parento`, `grandparento`, `membero`    |
| [`send-more-money.eta`](../../examples/send-more-money.eta)      | Classic CLP(FD) cryptarithm                        |
| [`nqueens.eta`](../../examples/nqueens.eta)                      | N-Queens via CLP                                   |

> Reference: [`logic.md`](./reference/logic.md), [`clp.md`](./reference/clp.md),
> [`clpb.md`](./reference/clpb.md).

---

## AAD & Finance

| Example                                       | Topic                                              | Walkthrough |
| :-------------------------------------------- | :------------------------------------------------- | :---------- |
| [`aad.eta`](../../examples/aad.eta)           | Reverse-mode AD primer, `grad`                     | [`aad.md`](./reference/aad.md) |
| [`european.eta`](../../examples/european.eta) | Black–Scholes Greeks (1st & 2nd order)             | [`european.md`](./reference/european.md) |
| [`sabr.eta`](../../examples/sabr.eta)         | SABR vol surface, Hagan approximation              | [`sabr.md`](./reference/sabr.md) |
| [`xva.eta`](../../examples/xva.eta)           | CVA / FVA sensitivities via AAD                    | [`xva.md`](./reference/xva.md) |
| [`xva-wwr/`](../../examples/xva-wwr/)         | Wrong-Way Risk via do-interventions                | [`featured_examples/xva-wwr.md`](../featured_examples/xva-wwr.md) |

---

## Statistics & ML

| Example                                       | Topic                                  |
| :-------------------------------------------- | :------------------------------------- |
| [`stats.eta`](../../examples/stats.eta)       | Descriptive stats, OLS                 |
| [`torch.eta`](../../examples/torch.eta)       | libtorch tensor / autograd basics      |
| [`torch_tests/`](../../examples/torch_tests/) | Layer / optimiser smoke tests          |

> Reference: [`stats.md`](./reference/stats.md), [`torch.md`](./reference/torch.md).

---

## Concurrency

| Example                                                       | Pattern                              |
| :------------------------------------------------------------ | :----------------------------------- |
| [`message-passing.eta`](../../examples/message-passing.eta)   | Parent / child via PAIR              |
| [`inproc.eta`](../../examples/inproc.eta)                     | `spawn-thread` with closure capture  |
| [`worker-pool.eta`](../../examples/worker-pool.eta)           | Parallel fan-out                     |
| [`parallel-map.eta`](../../examples/parallel-map.eta)         | Map over a list, one worker per item |
| [`parallel-fib.eta`](../../examples/parallel-fib.eta)         | Recursive parallel Fibonacci         |
| [`monte-carlo.eta`](../../examples/monte-carlo.eta)           | Embarrassingly parallel π estimation |
| [`scatter-gather.eta`](../../examples/scatter-gather.eta)     | SURVEYOR / RESPONDENT                |
| [`pub-sub.eta`](../../examples/pub-sub.eta)                   | Topic-filtered PUB / SUB             |
| [`echo-server.eta`](../../examples/echo-server.eta), [`echo-client.eta`](../../examples/echo-client.eta) | REQ / REP |
| [`distributed-compute.eta`](../../examples/distributed-compute.eta) | Cross-machine TCP messaging     |

> Reference: [`message-passing.md`](./reference/message-passing.md),
> [`networking.md`](./reference/networking.md),
> [`network-message-passing.md`](./reference/network-message-passing.md),
> [`supervisor.md`](./reference/supervisor.md).

---

## Causal & Portfolio

| Example                                                  | Role                                          | Walkthrough |
| :------------------------------------------------------- | :-------------------------------------------- | :---------- |
| [`causal_demo.eta`](../../examples/causal_demo.eta)      | Primer: symbolic + causal + CLP + libtorch    | [`causal.md`](./reference/causal.md) |
| [`do-calculus/`](../../examples/do-calculus/)            | Worked do-calculus identification problems    | [`causal.md`](./reference/causal.md) |
| [`fact-table.eta`](../../examples/fact-table.eta)        | Columnar fact tables for analytics            | [`fact-table.md`](./reference/fact-table.md) |
| [`portfolio-lp.eta`](../../examples/portfolio-lp.eta)    | LP construction via `std.clpr`                | — |
| [`portfolio.eta`](../../examples/portfolio.eta)          | End-to-end causal portfolio engine            | [`featured_examples/portfolio.md`](../featured_examples/portfolio.md) |

---

## Notebooks

| Notebook                                                                  | Topic                                  |
| :------------------------------------------------------------------------ | :------------------------------------- |
| [`notebooks/`](../../examples/notebooks/)                                 | Jupyter examples (xeus-eta kernel)     |

See [`jupyter.md`](./reference/jupyter.md) for kernel installation.

---

## Related

- [Language Guide](./language_guide.md)
- [Featured: Causal Portfolio Engine](../featured_examples/portfolio.md)
- [Featured: Wrong-Way Risk via do-interventions](../featured_examples/xva-wwr.md)

