# Examples Tour

[← Back to Language Guide](../language_guide.md)

A guided reading order through every program in
[`cookbook/`](../../cookbook/). Each entry gives a one-line summary and
a link to the file (and to the in-depth doc where one exists). 

---

## Reading order

If you are new to Eta, work through the **Beginner** and **Symbolic &
Logic** sections first. The **AAD & Finance** and **Causal & Portfolio**
sections build on each other and culminate in
[`portfolio.eta`](../../cookbook/quant/portfolio.eta).

---

## Beginner

| Example                                                | Topic                                                |
| :----------------------------------------------------- | :--------------------------------------------------- |
| [`hello.eta`](../../cookbook/basics/hello.eta)                | First program: `module`, `import`, `defun`, recursion |
| [`basics.eta`](../../cookbook/basics/basics.eta)              | Values, bindings, `cond`, records, quoting           |
| [`functions.eta`](../../cookbook/basics/functions.eta)        | `defun`, `lambda`, closures, variadics, `letrec`     |
| [`higher-order.eta`](../../cookbook/basics/higher-order.eta)  | `map*`, `filter`, `foldl`, `zip`, `range`            |
| [`composition.eta`](../../cookbook/basics/composition.eta)    | `compose`, `flip`, `negate`, manual currying         |
| [`recursion.eta`](../../cookbook/basics/recursion.eta)        | Fibonacci, deep flatten, Ackermann, Hanoi            |
| [`exceptions.eta`](../../cookbook/basics/exceptions.eta)      | `catch` / `raise`, `dynamic-wind`, structured payloads |

---

## Symbolic & Logic

| Example                                                          | Topic                                              |
| :--------------------------------------------------------------- | :------------------------------------------------- |
| [`boolean-simplifier.eta`](../../cookbook/logic/boolean-simplifier.eta) | Tree rewriting, De Morgan, fixed-point             |
| [`symbolic-diff.eta`](../../cookbook/logic/symbolic-diff.eta)          | Computer algebra: differentiation + simplification |
| [`unification.eta`](../../cookbook/logic/unification.eta)              | Raw `logic-var`, `==`, `findall`                   |
| [`logic.eta`](../../cookbook/logic/logic.eta)                          | Relations: `parento`, `grandparento`, `membero`    |
| [`send-more-money.eta`](../../cookbook/logic/send-more-money.eta)      | Classic CLP(FD) cryptarithm                        |
| [`nqueens.eta`](../../cookbook/logic/nqueens.eta)                      | N-Queens via CLP                                   |

> Reference: [`logic.md`](./reference/logic.md), [`clp.md`](./reference/clp.md),
> [`clpb.md`](./reference/clpb.md).

---

## AAD & Finance

| Example                                       | Topic                                              | Walkthrough |
| :-------------------------------------------- | :------------------------------------------------- | :---------- |
| [`aad.eta`](../../cookbook/quant/aad.eta)           | Reverse-mode AD primer, `grad`                     | [`aad.md`](./reference/aad.md) |
| [`european.eta`](../../cookbook/quant/european.eta) | Black–Scholes Greeks (1st & 2nd order)             | [`european.md`](./reference/european.md) |
| [`sabr.eta`](../../cookbook/quant/sabr.eta)         | SABR vol surface, Hagan approximation              | [`sabr.md`](./reference/sabr.md) |
| [`xva.eta`](../../cookbook/quant/xva.eta)           | CVA / FVA sensitivities via AAD                    | [`xva.md`](./reference/xva.md) |
| [`xva-wwr/`](../../cookbook/xva-wwr/)         | Wrong-Way Risk via do-interventions                | [`featured/xva-wwr.md`](../featured/xva-wwr.md) |

---

## Statistics & ML

| Example                                       | Topic                                  |
| :-------------------------------------------- | :------------------------------------- |
| [`stats.eta`](../../cookbook/quant/stats.eta)       | Descriptive stats, OLS                 |
| [`torch.eta`](../../cookbook/ml/torch.eta)       | libtorch tensor / autograd basics      |
| [`tests/torch/`](../../cookbook/tests/torch/) | Layer / optimiser smoke tests          |

> Reference: [`stats.md`](./reference/stats.md), [`torch.md`](./reference/torch.md).

---

## Concurrency

| Example                                                       | Pattern                              |
| :------------------------------------------------------------ | :----------------------------------- |
| [`message-passing.eta`](../../cookbook/concurrency/message-passing.eta)   | Parent / child via PAIR              |
| [`inproc.eta`](../../cookbook/concurrency/inproc.eta)                     | `spawn-thread` with closure capture  |
| [`worker-pool.eta`](../../cookbook/concurrency/worker-pool.eta)           | Parallel fan-out                     |
| [`parallel-map.eta`](../../cookbook/concurrency/parallel-map.eta)         | Map over a list, one worker per item |
| [`parallel-fib.eta`](../../cookbook/concurrency/parallel-fib.eta)         | Recursive parallel Fibonacci         |
| [`monte-carlo.eta`](../../cookbook/concurrency/monte-carlo.eta)           | Embarrassingly parallel π estimation |
| [`scatter-gather.eta`](../../cookbook/concurrency/scatter-gather.eta)     | SURVEYOR / RESPONDENT                |
| [`pub-sub.eta`](../../cookbook/concurrency/pub-sub.eta)                   | Topic-filtered PUB / SUB             |
| [`echo-server.eta`](../../cookbook/concurrency/echo-server.eta), [`echo-client.eta`](../../cookbook/concurrency/echo-client.eta) | REQ / REP |
| [`distributed-compute.eta`](../../cookbook/concurrency/distributed-compute.eta) | Cross-machine TCP messaging     |

> Reference: [`message-passing.md`](./reference/message-passing.md),
> [`networking.md`](./reference/networking.md),
> [`network-message-passing.md`](./reference/network-message-passing.md),
> [`supervisor.md`](./reference/supervisor.md).

---

## Causal & Portfolio

| Example                                                  | Role                                          | Walkthrough |
| :------------------------------------------------------- | :-------------------------------------------- | :---------- |
| [`causal_demo.eta`](../../cookbook/causal/causal_demo.eta)      | Primer: symbolic + causal + CLP + libtorch    | [`causal.md`](./reference/causal.md) |
| [`do-calculus/`](../../cookbook/do-calculus/)            | Worked do-calculus identification problems    | [`causal.md`](./reference/causal.md) |
| [`fact-table.eta`](../../cookbook/quant/fact-table.eta)        | Columnar fact tables for analytics            | [`fact-table.md`](./reference/fact-table.md) |
| [`portfolio-lp.eta`](../../cookbook/quant/portfolio-lp.eta)    | LP construction via `std.clpr`                | — |
| [`portfolio.eta`](../../cookbook/quant/portfolio.eta)          | End-to-end causal portfolio engine            | [`featured/portfolio.md`](../featured/portfolio.md) |

---

## Notebooks

| Notebook                                                                  | Topic                                  |
| :------------------------------------------------------------------------ | :------------------------------------- |
| [`notebooks/`](../../cookbook/notebooks/)                                 | Jupyter examples (xeus-eta kernel)     |

See [`jupyter.md`](./reference/jupyter.md) for kernel installation.

---

## Related

- [Language Guide](../language_guide.md)
- [Featured: Causal Portfolio Engine](../featured/portfolio.md)
- [Featured: Wrong-Way Risk via do-interventions](../featured/xva-wwr.md)


