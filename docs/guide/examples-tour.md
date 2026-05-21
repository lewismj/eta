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
[`portfolio.eta`](../../cookbook/numerics/portfolio.eta).

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

> Reference: [Logic](./reference/logic.md), [CLP(FD)](./reference/clp.md),
> [CLP(B)](./reference/clpb.md).

---

## AAD & Finance

| Example                                       | Topic                                              | Walkthrough |
| :-------------------------------------------- | :------------------------------------------------- | :---------- |
| [`aad.eta`](../../cookbook/numerics/aad.eta)           | Reverse-mode AD primer, `grad`                     | [AAD](./reference/aad.md) |
| [`european.eta`](../../cookbook/numerics/european.eta) | Black–Scholes Greeks (1st & 2nd order)             | [European options](./reference/european.md) |
| [`sabr.eta`](../../cookbook/numerics/sabr.eta)         | SABR vol surface, Hagan approximation              | [SABR](./reference/sabr.md) |
| [`xva.eta`](../../cookbook/numerics/xva.eta)           | CVA / FVA sensitivities via AAD                    | [XVA](./reference/xva.md) |
| [`xva-wwr/`](../../cookbook/xva-wwr/)         | Wrong-Way Risk via do-interventions                | [XVA with WWR](../featured/xva-wwr.md) |

---

## Statistics & ML

| Example                                       | Topic                                  |
| :-------------------------------------------- | :------------------------------------- |
| [`stats.eta`](../../cookbook/numerics/stats.eta)       | Descriptive stats, OLS                 |
| [`torch.eta`](../../cookbook/ml/torch.eta)       | libtorch tensor / autograd basics      |
| [`tests/torch/`](../../cookbook/tests/torch/) | Layer / optimiser smoke tests          |

> Reference: [Stats](./reference/stats.md), [Torch](./reference/torch.md).

---

## Concurrency

| Example                                                       | Pattern                              |
| :------------------------------------------------------------ | :----------------------------------- |
| [`gen-server-counter.eta`](../../cookbook/concurrency/gen-server-counter.eta) | OTP-style counter service (`std.actor.gen_server`) |
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

> Reference: [Message Passing](./reference/message-passing.md),
> [Networking](./reference/networking.md),
> [Network Message Passing](./reference/network-message-passing.md),
> [Supervisor](./reference/supervisor.md).

---

## Causal & Portfolio

| Example                                                  | Role                                          | Walkthrough |
| :------------------------------------------------------- | :-------------------------------------------- | :---------- |
| [`causal_demo.eta`](../../cookbook/causal/causal_demo.eta)      | Primer: symbolic + causal + CLP + libtorch    | [Causal](./reference/causal.md) |
| [`do-calculus/`](../../cookbook/do-calculus/)            | Worked do-calculus identification problems    | [Causal](./reference/causal.md) |
| [`fact-table.eta`](../../cookbook/numerics/fact-table.eta)        | Columnar fact tables for analytics            | [Fact Table](./reference/fact-table.md) |
| [`portfolio-lp.eta`](../../cookbook/numerics/portfolio-lp.eta)    | LP construction via `std.clpr`                | — |
| [`portfolio.eta`](../../cookbook/numerics/portfolio.eta)          | End-to-end causal portfolio engine            | [Portfolio](../featured/portfolio.md) |

---

## Packaging

| Example | Topic | Walkthrough |
| :------ | :---- | :---------- |
| [`packaging/end-to-end/`](../../cookbook/packaging/end-to-end/) | End-to-end package flow: local library + app path dependency | [First App](../app/first_app.md) |

---

## Notebooks

| Notebook                                                                  | Topic                                  |
| :------------------------------------------------------------------------ | :------------------------------------- |
| [`notebooks/`](../../cookbook/notebooks/)                                 | Jupyter examples (xeus-eta kernel)     |

See [Jupyter](./reference/jupyter.md) for kernel installation.

---

## Related

- [Language Guide](../language_guide.md)
- [Featured: Causal Portfolio Engine](../featured/portfolio.md)
- [Featured: Wrong-Way Risk via do-interventions](../featured/xva-wwr.md)

