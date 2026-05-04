# Blackjack Demo Plan — Multi-Paradigm Showcase (Library + App)

[Back to README](../../README.md) ·
[Packaging System](../packaging.md) ·
[Package Commands](../guide/packages.md) ·
[How to Build Your First App](../app/first_app.md) ·
[End-to-End Packaging Example](../../cookbook/packaging/end-to-end/README.md) ·
[Workspace Plan](./workspace_plan.md)

---

## 0) Introduction

> **One-line summary.** This demo shows how four paradigms — logic, causal
> inference, statistical learning, and constraint solving — can be
> *combined* to **learn, validate, and compress decision policies from
> data**, with blackjack as a small but non-trivial test bed.

Pipeline at a glance:

```
                  ┌──────────────┐
   Game traces ──►│   §5.3 ILP   │──► Rules of the game
                  └──────────────┘
                  ┌──────────────┐
   Shoe model ───►│ §5.4 DAG +do │──► Causal EV(action | count)
                  └──────────────┘
                  ┌──────────────┐
   MC rollouts ──►│ §5.6 Torch   │──► Representation w  +  EV head f_θ
                  └──────────────┘        (sufficient statistic)
                  ┌──────────────┐
   f_θ , w  ────►│ §5.7 CLP     │──► Optimal policy chart π*(s, c)
                  └──────────────┘
                  ┌──────────────┐
   π* chart ────►│ §5.8 ILP     │──► Human-readable maxims (cheat sheet)
                  └──────────────┘
```

Blackjack is the canonical "small game with deep structure" — a finite rule
set, a clean reward signal, and a well-known optimal strategy that depends
not only on the player's hand but on *what cards remain in the shoe*. That
last point is what makes it interesting for a multi-paradigm language: the
same game can be studied as **rules**, as a **causal system**, and as a
**statistical learning** problem, and each paradigm answers a different
question.

This demo asks four progressively harder questions, each handled by the
paradigm best suited to it:

1. *Given only game traces, can we recover the rules?*
   → **Logic / ILP**: search a tiny predicate DSL for the smallest
   hypothesis consistent with the traces. Output: induced clauses for
   `bust`, `blackjack`, `dealer-hits`, win condition.

2. *Does the count actually cause a change in expected value, or is it just
   correlated with one?*
   → **Causal / do-calculus**: build a shoe DAG and evaluate
   `E[Outcome | do(Action=a), RunningCount=c]`. Output: a count threshold
   at which the optimal action for `(player=16, dealer-up=10)` flips from
   `hit` to `stand`.

3. *What is the optimal weight to assign each rank when summarising the
   shoe into a single scalar "count"?*
   → **ML / Torch + Monte Carlo**: jointly optimize a per-rank weight
   vector `w ∈ ℝ¹³` and a small EV head against millions of simulated
   hands. **This is representation learning of an approximately
   sufficient statistic for decision-making** — the model discovers a
   compressed state summary that retains everything needed to choose the
   optimal action. Output: `w` converging — without being told — to a
   positive multiple of the canonical Hi-Lo vector
   `(+1,+1,+1,+1,+1, 0,0,0, −1,−1,−1,−1,−1)`.

4. *What action should I take in every situation?*
   → **CLP enumeration**: project the learned EV function over
   `(player_total × dealer_up × count_bucket)` and emit the basic-strategy
   chart. Output: a printed table per count bucket, snapshot-tested for
   stability.

5. *Can we compress that chart back into the human maxims people actually
   memorise?*
   → **Logic / ILP again, applied to the chart itself**: induce
   short, readable rules such as *"always split aces and 8s"*,
   *"never split 10s"*, *"double 11 vs dealer 2–10"*, *"stand on hard
   17+"*, *"hit soft 17 or less"*, **and the soft-double rules like
   *"A,5 doubles vs 4–6, otherwise hit"***. Output: a ranked list of
   clauses with coverage/error stats. This closes the loop — the
   rules-of-the-game induced in (1) and the rules-of-play induced here
   use the *same* machinery on different inputs.

> **Important: the entire basic-strategy chart is *learned*, not given.**
> The cheat sheet in §5.8.1 (hard totals, soft totals, pairs — all of
> it, including the soft-double rules) is the **acceptance target**, not
> an input to the system. The demo:
>
> 1. computes EV for every `(player_hand_class, dealer_up, count)`
>    triple from first principles in §5.7 (Monte Carlo + the §5.6
>    learned EV head),
> 2. takes `argmax_a` to produce a chart of optimal actions,
> 3. runs ILP over that chart in §5.8 to compress it into the maxims.
>
> Nowhere is "soft 16 doubles vs 4–6" or "stand on 12 vs 4–6" coded as
> a rule. Each emerges as the `argmax` action for the relevant cells
> and is then recovered as a clause by induction. The §5.8.1 cheat
> sheet is the gold standard the system is *checked against*, the same
> way §5.2's hand-written rules are the oracle for §5.3 induction.

### 0.1 The EV math the demo is built on

All four phases reduce to one quantity. Let:

- `S` = visible player state `(player_total, dealer_up_card, hand_shape)`
  where `hand_shape` records pair-ness / soft-ness so split and double
  eligibility are well-defined,
- `A ∈ A(s)` = action drawn from the **state-dependent legal set**
  `A(s) ⊆ {hit, stand, double, split}`. The eligibility predicates from
  §5.2.1 define `A(s)`:
  - `hit, stand ∈ A(s)` always,
  - `double ∈ A(s)` iff `can-double(s)` (typically `|hand|=2`),
  - `split  ∈ A(s)` iff `can-split(s)`  (hand is a pair).
  v1 core restricts `A(s) = {hit, stand}` for every `s`; v1.5 uses the
  full set above.
- `C` = running count summary of the unseen shoe,
- `R ∈ ℝ` = terminal reward in **stake units**, so doubles and splits
  are commensurable with hit/stand:
  - `hit/stand`: `R ∈ {−1, 0, +1}` (loss / push / win; naturals pay 3:2),
  - `double`:    `R ∈ {−2, 0, +2}` (one card, doubled stake),
  - `split`:     `R = R_left + R_right` over the two sub-hands, each
    itself recursively in stake units (sub-hands may double).
- `π : (S, C) → A(s)` = policy, constrained so `π(s, c) ∈ A(s)`.

The action-value under a policy is:

```
Q^π(s, a, c) = E[ R | S=s, A=a, C=c, follow π thereafter ]
              with a ∈ A(s)
```

The optimal action at `(s, c)` is `argmax_{a ∈ A(s)} Q*(s, a, c)`, and
the basic strategy table is exactly the function
`(s, c) ↦ argmax_{a ∈ A(s)} Q*(s, a, c)` flattened to 2-D per
`c`-bucket.

Because `R` is in stake units, the `argmax` is directly comparable
across `{hit, stand, double, split}` — doubling is preferred over
hitting iff the doubled-stake EV exceeds the hit-then-play-on EV; a
split is preferred iff the sum of the two sub-hand EVs exceeds the
best single-hand EV. This is what lets the §5.7 chart and §5.8 maxims
treat all four actions uniformly in v1.5.

Why the count matters: by the law of total probability,

```
P(NextCard = r | C=c) ≠ P(NextCard = r)        when c ≠ 0,
```

so `Q*(s, a, c)` is genuinely a function of `c`, not just `s`. The Hi-Lo
weights `w*` are (proportional to) the **effect of removal (EOR)** of each
rank on overall EV — i.e. `w*[r] ∝ −∂E[R]/∂P(rank r remains)`. The demo
recovers `w*` two ways:

1. *empirically*, by gradient descent against MC reward (§5.6),
2. *causally*, as the slope of `E[R | do(Action=π(s,c)), C=c]` in `c`
   (§5.4).

Agreement between (1) and (2) is the demo's central confirmation that
**the count carries decision-relevant information under intervention**,
not just under observation. Strict identifiability of the per-rank EOR
from a scalar count is *not* claimed (see §5.4 caveat) — what is shown
is that two independent estimation paths land in the same cone, which
is the operationally useful statement.

### 0.2 What runs end-to-end

`eta run -- all --seed 42` executes, in order:

1. **Generate** a deterministic trace under a baseline (rule-following but
   non-counting) dealer/player.
2. **Induce** rule predicates from the trace and print them.
3. **Build** the causal DAG and print the EV-vs-count sweep for the
   canonical `(16, 10)` decision, marking the flip point.
4. **Train** the joint `(w, f_θ)` model and print the recovered count
   weights, normalized.
5. **Enumerate** the strategy chart for `count ∈ {−2, 0, +2}` buckets and
   print it.
6. **Distill** the chart into human-readable basic-strategy maxims (e.g.
   *"never split 10s"*, *"always split 8s"*) and print them ranked by
   coverage.

The whole pipeline is reproducible (`--seed`) and deterministic under
fixed inputs, so it doubles as a smoke test for the packaging story.

### 0.3 What this demo deliberately is *not*

It is not a casino-grade card counter. v1 of the *core spine* limits the
action space to `hit/stand` so the four-paradigm story is the focus; v1.5
extends the action set to `{hit, stand, double, split}` specifically so
the basic-strategy maxim phase (§5.8) can produce the famous splitting and
doubling rules. Insurance and surrender remain follow-ups, as does
multi-deck composition-dependent play. These boundaries are listed in
§3.2.

### 0.4 Why this maps to real systems

Blackjack is a vehicle. The *capabilities* the demo exercises are
directly transferable:

| Demo component (blackjack)              | Real-world equivalent                                        |
|----------------------------------------|--------------------------------------------------------------|
| Shoe state                              | Market / portfolio / system state                             |
| Running count `C` (learned)             | Risk-factor compression, sufficient statistic, latent regime  |
| ILP-induced rules (§5.3)                | Compliance / regulatory logic, trade classification, alerting |
| Causal DAG + `do()` (§5.4)              | Risk dependency graph; intervention vs observation studies    |
| EV head `f_θ` (§5.6)                    | Pricing / PnL / utility model                                 |
| Strategy chart (§5.7)                   | Trading / hedging / routing policy                            |
| Maxim induction (§5.8)                  | Distilling black-box policies into human-auditable rules      |

The ILP layers in particular are a stand-in for **interpretable model
discovery**: in domains where rules are *not* known a priori (novel
trade structures, evolving regulation, anomaly triggers), the same
machinery learns short, human-checkable clauses from observations.

### 0.5 Why not just train one end-to-end RL policy?

Because the goal is not just "win at blackjack" — it is to **learn,
validate, and explain** the resulting policy. Splitting the problem
across paradigms buys properties an end-to-end policy net cannot offer:

1. **Cross-paradigm validation.** The Hi-Lo weights are recovered
   *empirically* (gradient descent, §5.6) and *causally* (slope of
   `do()`-EV in count, §5.4). Two independent derivations agreeing is
   evidence; one neural net's output is not.
2. **Interpretability by construction.** The representation (`w`), the
   value (`f_θ`), and the policy (CLP enumeration → maxims) are
   separable artefacts. Each can be inspected, snapshot-tested, and
   audited.
3. **Sample efficiency.** A 13-parameter linear summary plus a small EV
   head converges with modest data; an end-to-end policy net over the
   same state would need orders of magnitude more rollouts to match —
   and would still not yield a cheat sheet at the end.
4. **Testability.** Each phase has a sharp acceptance criterion
   (rule equivalence, EV flip, cosine-to-Hi-Lo, chart agreement,
   maxim coverage). An end-to-end agent has only one (win-rate),
   which is high-variance.

End-to-end RL is the natural baseline; this demo is deliberately the
*opposite* design choice.

---

## 1) Objective

Ship a runnable cookbook demo that uses Eta's four flagship paradigms — **Logic
/ CLP**, **Causal / do-calculus**, **ML / Torch**, and **Concurrency /
Monte Carlo** — to:

1. **induce** the basic rules of blackjack from observed game traces,
2. **model** the shoe causally and evaluate `do(action)` queries,
3. **rediscover** the Hi-Lo running-count weights from EV residuals,
4. **emit** a basic-strategy chart conditioned on the count.

Primary outcome: a packaged `library + app` pair under
`cookbook/blackjack/` that builds and runs end-to-end with `eta build && eta run`,
and is referenced from the README/TLDR as a flagship example.

---

## 2) Why this demo

1. exercises four cookbook areas in one cohesive narrative,
2. each paradigm is used where it is genuinely best (not contrived),
3. produces a *measurable* punchline: the learned weight vector should
   converge to a positive multiple of `(+1,+1,+1,+1,+1, 0,0,0, -1,-1,-1,-1,-1)`,
4. validates the packaging story end-to-end with a non-trivial workload.

---

## 3) Scope and non-goals

### 3.1 In scope

1. core game model: shoe, hand, dealer policy (driven by induced rules),
2. ILP-style rule induction over `(state, action, outcome)` traces,
3. causal DAG with `do()` queries via the existing do-calculus primitives,
4. parallel Monte Carlo rollout harness,
5. Torch-based EV / residual model and joint optimization of count weights,
6. CLP enumeration to produce the strategy chart,
7. **basic-strategy maxim induction** (ILP over the chart, §5.8),
8. CLI app `blackjack-demo` that runs each phase and prints results,
9. unit tests per phase + an integration smoke test.

### 3.2 Out of scope (v1 core; included in v1.5)

1. v1 core action set is `{hit, stand}`; v1.5 adds `{double, split}` so
   maxim induction can recover splitting/doubling rules,
2. insurance and surrender remain out of scope,
3. multi-deck composition-dependent strategy,
4. betting strategy / Kelly sizing,
5. GUI / notebook polish (a follow-up notebook can wrap the library),
6. registry publication.

---

## 4) Package layout

Two packages under `cookbook/blackjack/`, mirroring
[end-to-end packaging](../../cookbook/packaging/end-to-end/README.md):

```
cookbook/blackjack/
  README.md
  blackjack/                 # library package
    eta.toml
    src/
      blackjack.eta          # public re-exports
      shoe.eta               # cards, deck, deterministic shuffle
      rules.eta              # rule predicates (sum, bust, dealer_hits, ...)
      induction.eta          # ILP-lite hypothesis search
      causal.eta             # DAG + do() queries
      mc.eta                 # parallel rollout harness
      learn.eta              # torch model + weight optimization
      strategy.eta           # CLP enumeration -> chart
      maxims.eta             # ILP over chart -> human-readable rules
    tests/
      shoe_test.eta
      rules_test.eta
      induction_test.eta
      causal_test.eta
      learn_test.eta
      strategy_test.eta
      maxims_test.eta
  blackjack-demo/            # app package
    eta.toml
    src/
      blackjack_demo.eta     # CLI: subcommands induce|causal|learn|chart|all
    tests/
      smoke_test.eta
```

### 4.1 Library `eta.toml`

```toml
[package]
name = "blackjack"
version = "0.1.0"
license = "MIT"

[compatibility]
eta = ">=0.6, <0.8"

[dependencies]
```

### 4.2 App `eta.toml`

```toml
[package]
name = "blackjack_demo"
version = "0.1.0"
license = "MIT"

[compatibility]
eta = ">=0.6, <0.8"

[dependencies]
blackjack = { path = "../blackjack" }
```

Created with the standard flow:

```console
cd cookbook/blackjack
eta new blackjack --lib
eta new blackjack-demo --bin
cd blackjack-demo
eta add blackjack --path ../blackjack
```

---

## 5) Module-by-module design

### 5.1 `shoe.eta` — card model

1. Ranks `1..13` with value map `A=(1|11)`, `2..10` face value, `J/Q/K=10`.
2. `make-shoe : (decks seed) -> shoe` deterministic via seeded RNG.
3. `deal : shoe -> (card, shoe')` pure functional.
4. `hand-totals : hand -> list[int]` (handles soft aces).
5. `running-count : seen -> int` parameterized by a weight vector
   (Hi-Lo is just one instantiation).

Reference patterns: [stats.eta](../../cookbook/quant/stats.eta).

### 5.2 `rules.eta` — relational rule predicates

Pure logic predicates re-using patterns from
[logic.eta](../../cookbook/logic/logic.eta) and
[unification.eta](../../cookbook/logic/unification.eta):

```
bust(H)            :- min(totals(H)) > 21.
blackjack(H)       :- |H|=2, 21 ∈ totals(H).
dealer-hits(H)     :- best(H) < 17.                        -- S17 ruleset
dealer-hits(H)     :- best(H) = 17, soft(H), <H17 variant>.-- H17 (disabled v1)
dealer-stands(H)   :- best(H) >= 17, ¬(<H17 case above>).
player-wins(P,D)   :- ¬bust(P), (bust(D) ∨ best(P) > best(D)).
push(P,D)          :- ¬bust(P), ¬bust(D), best(P) = best(D).
```

**Dealer ruleset for v1/v1.5: S17 — dealer stands on soft 17.** This is
fixed so the §5.7 chart and §5.8 maxim acceptance match the canonical
cheat sheet in §5.8.1. An H17 toggle is reserved as a follow-up.

Two roles: ground-truth oracle for trace generation, and target predicates
for induction in §5.3.

#### 5.2.1 Mechanical rules vs strategic rules — what is *learned* and what is *given*

Splits and doubles raise an honest question: *how do we learn the rules
for them?* The answer is that "rules" splits into two very different
things, and only one of them is learnable from outcome traces:

1. **Mechanical rules** — *what an action does*. E.g. *"a split takes a
   pair and starts two independent hands, each receiving one new card,
   stake doubled"*; *"a double takes exactly one card and doubles the
   stake"*; *"a hit draws one card and may bust"*. These are
   **transition semantics** of the game's state machine. They are not
   learnable from `(state, action, reward)` traces alone — the trace
   only shows *consequences* of these mechanics, and many distinct
   mechanics could explain the same outcomes. The honest treatment is
   to encode them as **game-spec axioms** in `rules.eta` and
   `mc.eta`, the same way we encode "dealer deals two cards" or "Aces
   count as 1 or 11". They are *given*, not induced.

2. **Eligibility predicates** — *when an action is legal*. E.g.
   *"split is legal iff the hand is a pair"*; *"double is legal iff the
   hand has exactly two cards"*. These **are** learnable from traces
   that include the legal-action set per state, because they are pure
   classifiers over state. §5.3 induces them as additional targets in
   v1.5, alongside `bust` / `dealer-hits` / etc.

3. **Strategic rules** — *when an action is optimal*. E.g.
   *"never split 10s"*, *"always split aces"*, *"double 11 vs 2–10"*.
   These are the §5.8 maxims, induced from the §5.7 chart. This is the
   genuinely interesting learning problem, and the one the cheat sheet
   in §5.8.1 catalogues.

Concretely, in v1.5:

| Layer                | Splits / doubles handled by |
|----------------------|-----------------------------|
| Mechanics (transitions) | **Given** — extended `rules.eta` + `mc.eta` |
| Eligibility (`can-split`, `can-double`) | **Induced** by §5.3 ILP from richer traces |
| Strategy (when to split/double) | **Induced** by §5.8 ILP from §5.7 chart |

This split also matches the real-world analogy: in trading, the
*mechanics* of an order type (limit, stop, IOC) are exchange spec; the
*eligibility* (does this account/instrument permit the type?) is
classifier logic; the *strategy* (when to use it) is the decision
policy. The demo exercises the same separation.

### 5.3 `induction.eta` — ILP-lite rule discovery

Goal: given a labeled trace `[(state, label)]`, search a small predicate
DSL (`sum`, `count`, `member`, `<`, `<=`, `=`) for the smallest hypothesis
consistent with the labels.

Approach (re-using [boolean-simplifier.eta](../../cookbook/logic/boolean-simplifier.eta)):

1. enumerate candidate clauses up to bounded length,
2. score by coverage − complexity (MDL),
3. simplify boolean combinations with the existing simplifier,
4. return canonical rule per target predicate.

Targets: `bust`, `blackjack`, `dealer-hits`, `dealer-stands`, win condition.
In **v1.5**, the target set extends with the eligibility predicates
`can-split` and `can-double` (induced from richer traces that record the
legal-action set per state). Mechanical transition semantics for `split`
and `double` are *not* induced — they are game-spec axioms in §5.2.1.

Acceptance: induced rules are *equivalent* (after simplification) to the
hand-written rules in §5.2 over a 50k-hand trace.

> **Why bother inducing rules we already know?** Because the engine is the
> point, not the rules. The same predicate-search machinery applies
> directly to domains where ground truth is *not* available — trade
> classification, compliance triggers, anomaly rules, regulatory logic —
> i.e. wherever short, human-auditable structure must be discovered from
> observations. Blackjack is the only setting where we *can* check the
> induced rules against a known oracle, which is exactly what makes it a
> good test bed.

### 5.4 `causal.eta` — DAG + do-calculus

Build a shoe DAG using [dag.eta](../../cookbook/do-calculus/dag.eta) and
[do-rules.eta](../../cookbook/do-calculus/do-rules.eta):

```
Shoe ──► NextCard ──► Outcome
  │                     ▲
  └──► RunningCount ────┘
Action ──────────────────► Outcome
```

API:

1. `ev-obs : (state, action, count) -> float`
   evaluates the **observational** `E[Outcome | A=a, C=c]` from logged play,
2. `ev-do : (state, action, count) -> float`
   evaluates the **interventional** `E[Outcome | do(A=a), C=c]`,
3. `policy-do : (state, count) -> action` picks `argmax_a ev-do(...)`.

The pair `(ev-obs, ev-do)` is the point. Observational EV mixes the
behaviour policy of whoever generated the trace into the answer;
interventional EV does not. The demo prints both side-by-side and
flags the gap, so the claim "the count *causes* a change in optimal
action" is supported by an actual `do()` query rather than a
correlation. In this fully-known shoe model the two will frequently
agree — that is itself worth showing, because it makes the back-door
adjustment explicit rather than implicit.

Acceptance:

1. optimal action for `(player=16, dealer-up=10)` flips from `hit` to
   `stand` as `true-count` increases past a learned threshold under
   `ev-do`,
2. for at least one `(state, count)` pair, `ev-obs ≠ ev-do` under a
   biased behaviour policy, demonstrating the engine actually performs
   the intervention rather than reporting conditional expectation,
3. the slope `∂ ev-do / ∂ c` evaluated over count buckets recovers a
   weight vector consistent (cosine ≥ 0.9) **with the §5.6 learned `w`,
   not necessarily with the per-rank EOR ground truth** (see caveat
   below).

#### 5.4.1 Identifiability caveat — what this layer does and does not show

The running count `C` is a *deterministic, lossy function* of the shoe
composition. Two shoes with different rank distributions can produce
the same `C`. Consequently:

1. `do(A=a)` is a clean intervention on the action node; the demo
   genuinely performs and prints it.
2. `∂ ev-do / ∂ c` is **not** an identification of per-rank
   effect-of-removal (EOR). It is the slope of EV with respect to a
   *summary statistic* of the unseen shoe under the modelling
   constraint that policy depends on `(s, c)` only. Per-rank EOR is
   only identifiable if either (a) the unseen-rank distribution is
   inverted from `c` (it is not, in general), or (b) finer-grained
   conditioning is allowed (which would defeat the point of compressing
   to a scalar count).
3. What §5.6 and §5.4 jointly demonstrate is therefore weaker — but
   still operationally interesting — than "Hi-Lo recovered from
   first principles": both routes converge on **a direction in
   weight-space consistent with EOR under count aggregation**, i.e.
   the best linear summary that retains decision relevance. That is
   exactly what Hi-Lo *is* in practice; the demo just refuses to
   over-claim about identifiability.

In real-system terms (cf. §0.4): this is the difference between
*"intervening on a control given a learned risk-factor summary"* and
*"identifying causal effects of every underlying market state
component"*. The first is what production systems actually do; the
second is rarely identifiable. The demo models the honest version.

### 5.5 `mc.eta` — parallel rollout harness

Re-uses [monte-carlo.eta](../../cookbook/concurrency/monte-carlo.eta),
[parallel-map.eta](../../cookbook/concurrency/parallel-map.eta), and the
worker-pool patterns:

1. `simulate : (policy, shoe, n) -> traces` produces
   `(state, action, count, reward)` tuples,
2. shards across workers; deterministic seeds per shard,
3. deterministic replay checks ensure stable trace/stat outputs for a
   fixed seed and configuration.

#### 5.5.1 Frozen shoe parameters

To make count behaviour and acceptance reproducible, the demo fixes:

1. **Decks**: `6` (a standard six-deck shoe),
2. **Penetration**: `75%` (re-shuffle when 25% of cards remain),
3. **Reshuffle policy**: cut card at the penetration point; no
   continuous-shuffle variant in v1/v1.5,
4. **Blackjack payout**: `3:2` on naturals (consistent with the §5.8.1
   S17 cheat sheet); the §5.6 reward signal **must** apply this payout
   on the same code path as the §5.4 `ev-do` evaluator and the §5.7
   chart enumeration — a single reward function reused everywhere.
5. **Hit/stand on naturals**: dealer checks for blackjack; player
   blackjack against dealer non-blackjack pays `3:2` and the round ends.

These five values are constants in `shoe.eta` and surfaced in the app
header so every printed result is fully specified.

#### 5.5.2 Rollout representation

To keep rollout execution simple, deterministic, and allocation-light:

1. represent the shoe as a **rank-count vector** `[u8; 13]` (a 13-byte
   composition), not a list of cards — sampling a card is one
   `multinomial`-style step on the count vector,
2. represent a hand as a tuple `(soft_total, hard_total, n_cards,
   pair_rank?)` — fixed-size, no allocation per hit,
3. trace rows are POD structs in a pre-sized arena per shard,
4. workers reuse arenas across rounds; only the trace summary crosses
   thread boundaries.

The representation choice is correctness-first: preserve pure functional
semantics and deterministic behavior, then optimize implementation details
only if they do not change outputs.

### 5.6 `learn.eta` — Torch EV model and weight discovery

> **Framing.** This phase is **representation learning of an approximately
> sufficient statistic**. The shoe state is high-dimensional; the model
> compresses it to a single scalar `count = w · seen` such that the EV
> head `f_θ(player_total, dealer_up, count)` recovers the optimal action.
> Recovering Hi-Lo is the *measurable* outcome; the *transferable*
> outcome is the technique — discovering a low-dimensional summary of a
> high-dimensional state that preserves decision-relevant information.

Parameterize:

1. `w ∈ ℝ¹³` — per-rank count weight (init = small random),
2. `f_θ : (player_total, dealer_up, count) -> ℝ` — EV head (small MLP),
3. shared loss: MSE between `f_θ` and observed reward.

Training loop (using [torch.eta](../../cookbook/ml/torch.eta)):

1. generate batch of MC traces under current `policy-from(f_θ, w)`,
2. compute `count` per state via `w`,
3. backprop through `f_θ` and `w` jointly,
4. optionally re-fit `policy` every K epochs (policy iteration).

Acceptance:

1. `w` converges (after sign + scale normalization) to within
   cosine-similarity ≥ 0.95 of canonical Hi-Lo
   `(+1×5, 0×3, −1×5)`,
2. EV monotone-increasing in true count for `bet=1` policy.

#### 5.6.1 Headline path: joint policy iteration

The headline result uses the full joint loop above — `w`, `f_θ`, and
the policy are co-trained. This is the design that demonstrates the
representation-learning claim and is the path the §0 narrative is
built around.

#### 5.6.2 Fallback path: supervised regression (reproducibility + debugging baseline)

A second mode, `--learn=supervised`, **fixes the policy to canonical
basic strategy** (the §5.8.1 chart) and reduces the problem to a
supervised regression of EV residuals against shoe composition. This
is *not* the headline — it is shipped as:

1. **CI baseline** — deterministic, fast, no policy-iteration
   variance, suitable for stable test execution.
2. **Debugging anchor** — if the joint loop fails to recover Hi-Lo,
   the supervised path tells you whether the problem is in the
   representation/data (supervised also fails) or in the policy
   iteration (supervised succeeds).
3. **Headline cross-check** — both modes should produce `w` vectors
   within cosine ≥ 0.95 of each other; disagreement is a bug signal.

The joint mode remains the demo's headline because it is the path
that makes the representation-learning claim non-trivial — the policy
itself emerges from the same parameters that summarise the shoe.

#### 5.6.3 Debug trace

Both modes can emit `--debug-trace <path>`, which writes a CSV of
`(round, state, count, action, ev_hit, ev_stand, ev_double, ev_split,
chosen, reward)` per decision point. This is the single most useful
artefact when an acceptance test fails and is documented as the first
thing to inspect in §10.

### 5.7 `strategy.eta` — CLP basic-strategy chart

Enumerate `player_total ∈ {5..20} × dealer_up ∈ {2..A} × count_bucket`,
solve `argmax_a ev-do(...)`, project to a 2D table per count bucket. Same
enumeration shape as [nqueens.eta](../../cookbook/logic/nqueens.eta) and
[send-more-money.eta](../../cookbook/logic/send-more-money.eta).

Output: pretty-printed chart(s) and a stable text snapshot for tests.

#### 5.7.1 Worked example — how a soft-double rule emerges

To make concrete that the chart (and hence the §5.8 maxims) is *not*
seeded with cheat-sheet knowledge, consider the cell
`(hand=A,5, dealer_up=4, count=0)`:

1. §5.5 generates rollouts under each action in `A(s) =
   {hit, stand, double}` (split is illegal — not a pair). Stake-unit
   reward (§0.1) makes the three values directly comparable.
2. §5.6's EV head (or §5.5 raw MC, in `--learn=supervised` mode)
   produces approximately:

   ```
   Q(A,5 vs 4, hit)    ≈ +0.05
   Q(A,5 vs 4, stand)  ≈ −0.16
   Q(A,5 vs 4, double) ≈ +0.13
   ```

3. `argmax = double`. The §5.7 chart cell becomes `D`.
4. The same is true for `dealer_up ∈ {4, 5, 6}` and false for
   `dealer_up ∈ {2, 3, 7..A}` (where `Q(hit) > Q(double/2)` once you
   account for the doubled stake risk).
5. §5.8 ILP scans the chart row for `(hand=A,5)` and finds the
   minimal-complexity clause covering exactly the `D` cells:
   `soft-pair(5), up-in({4..6}) → double` — i.e.
   ***"A,5 doubles vs 4–6, otherwise hit"***, recovered without
   ever telling the system that this rule exists.

The same process recovers every other soft-total row, every hard-total
boundary (`13–16 → S vs 2–6, else H`), every pair rule, and the
count-conditional deviations (`16 vs 10 → S` at high count). The
cheat sheet in §5.8.1 lists the targets; §5.7 + §5.8 produce them.

### 5.8 `maxims.eta` — basic-strategy maxim induction

Goal: turn the chart from §5.7 into the short, human-memorable rules that
players actually learn — *"always split aces and 8s"*, *"never split 10s"*,
*"double 11 vs 2–10"*, *"stand on hard 17+"*, *"hit soft 17 or less"*,
*"stand on 12 vs dealer 4–6"*.

This is the **same induction engine as §5.3**, applied to a different
relation: instead of `(hand_state → label)` from a game trace, the input
is `(player_hand_class, dealer_up, count_bucket → optimal_action)` from
the §5.7 chart. That symmetry — one ILP engine, two very different
artefacts — is a deliberate teaching point of the demo.

Predicate DSL (kept tiny, like §5.3):

1. **hand classifiers**: `pair(r)`, `hard(n)`, `soft(n)`, `total-in(lo,hi)`,
   `soft-pair(r)` (for A,r soft hands),
2. **dealer classifiers**: `up-in(set)`, `up=r`,
3. **count classifiers**: `count-bucket=b`, `count>=b`,
4. **action heads**: `action ∈ {hit, stand, double, split, double-else-stand}`
   — the `double-else-stand` head encodes the cheat-sheet `Ds` cell
   (e.g. A,7 vs 3–6) so soft-hand rows are expressible as single clauses.

Search and scoring:

1. enumerate clauses up to a small literal cap (≤ 3 antecedents),
2. score by **coverage** (cells matched) minus a complexity penalty,
3. **bias toward canonical predicates** — give a complexity discount
   to literals that appear in §5.8.1 row schemas (e.g. `total=11`,
   `pair(8)`, `up-in({2..6})`, `up-in({7..A})`, `hard`, `soft`,
   `pair`). This pulls the search toward human-shaped clauses
   instead of equally-scoring ad-hoc combinations like
   `total-in(12,16) ∧ up-in({2,3,4,5,6}) ∧ ¬soft ∧ ¬pair`.
4. greedy set-cover so the printed list is short and non-redundant,
5. report each surviving clause with `(coverage %, error %, examples)`.

Post-processing normalisation (applied to the surviving clause set):

1. **interval merge** — merge adjacent `total=k` literals into
   `total-in(lo,hi)`,
2. **dealer-range canonicalisation** — collapse `up-in({...})` to
   the canonical ranges `{2..6}`, `{7..A}`, `{2..10}`, `{2..9}`,
   `{2..7}`, `{3..6}`, `{4..6}`, `{5..6}` whenever the matching set
   is a subset of one of these (these are exactly the dealer-range
   shapes used by the cheat sheet),
3. **negation pruning** — drop redundant `¬soft`, `¬pair` literals
   when implied by the head (e.g. `pair(r) → split` does not need
   `¬soft`),
4. **head canonicalisation** — collapse `double` heads on cells where
   `double` is illegal (`|hand| ≠ 2`) into the underlying `hit`/`stand`.

Without these passes the raw ILP output, even with sensible scoring,
will not look like the cheat sheet; with them it does.

Reuses [boolean-simplifier.eta](../../cookbook/logic/boolean-simplifier.eta)
for clause normalisation and the same simplification path as §5.3.

#### 5.8.1 Canonical ground-truth chart (S17, dealer stands on soft 17)

The acceptance target for v1.5 maxim induction at `count_bucket = 0` is
the standard S17 cheat sheet below. Dealer up-card columns are
`{2,3,4,5,6,7,8,9,10,A}`; `D` = double if allowed else hit, `Ds` = double
if allowed else stand, `H` = hit, `S` = stand, `P` = split.

**Hard totals** (no usable Ace):

| Hand | Action |
|---|---|
| ≤ 8 | `H` |
| 9   | `D` vs 3–6, else `H` |
| 10  | `D` vs 2–9, else `H` |
| 11  | `D` vs 2–10, `H` vs A |
| 12  | `S` vs 4–6, else `H` |
| 13–16 | `S` vs 2–6, else `H` |
| 17+ | `S` |

**Soft totals** (Ace = 11):

| Hand | Action |
|---|---|
| A,2 / A,3 | `D` vs 5–6, else `H` |
| A,4 / A,5 | `D` vs 4–6, else `H` |
| A,6       | `D` vs 3–6, else `H` |
| A,7       | `S` vs 2,7,8 · `Ds` vs 3–6 · `H` vs 9,10,A |
| A,8 / A,9 | `S` |

**Pairs** (splitting):

| Pair | Action |
|---|---|
| A,A   | `P` |
| 8,8   | `P` |
| 10,10 | `S` |
| 5,5   | `D` vs 2–9, else `H` |
| 2,2 / 3,3 | `P` vs 2–7, else `H` |
| 4,4   | `P` vs 5–6, else `H` |
| 6,6   | `P` vs 2–6, else `H` |
| 7,7   | `P` vs 2–7, else `H` |
| 9,9   | `P` vs 2–6, 8–9 · `S` vs 7,10,A |

This table is checked into `tests/fixtures/basic_strategy_s17.txt` and is
the single source of truth for both §5.7 chart snapshot and §5.8 maxim
acceptance.

#### 5.8.2 Acceptance contract

Let **C** be the §5.7 chart at `count_bucket = 0`, **G** be the
canonical chart in §5.8.1, and `gap(s) = |Q*(s, C(s)) − Q*(s, G(s))|`
the EV gap between the demo's choice and the cheat sheet's choice at
state `s`. Define **tolerant agreement**:

```
tol-agree(C, G; ε) =
  |{ s : C(s) = G(s)  ∨  gap(s) ≤ ε }| / |G|
```

i.e. cells that disagree but are *near-indifferent* (EV gap below `ε`)
do not count as failures. The MC noise floor on `Q*` is
`O(1 / sqrt(N))` per cell; with N≈10⁵ rollouts the noise floor is
≈ 3 × 10⁻³, so we set `ε = 0.01` (about 3σ).

v1.5 acceptance requires:

1. **Chart fidelity (tolerant)**: `tol-agree(C, G; 0.01) ≥ 0.98`.
   Strict agreement `agree(C, G) ≥ 0.95` is also reported but is
   **informational, not gating** — near-indifferent cells legitimately
   flip under MC noise and rule-set variants.
2. **Maxim coverage**: the induced rule list must reproduce, with
   `error % ≤ 2`, **every** row of §5.8.1 — i.e. each row is recoverable
   as either a single clause or a small disjunction of clauses from the
   §5.8 DSL. Concretely, the test asserts presence of at least one rule
   per row (e.g. `pair(A) → split`, `pair(8) → split`,
   `pair(10) → stand`, `total-in(17,21), hard → stand`,
   `total-in(13,16), hard, up-in({2..6}) → stand`,
   `total=11, up-in({2..10}) → double`, `pair(9), up-in({7,10,A}) → stand`,
   etc.).
3. **Count-conditional deviation**: at `count_bucket = +2` at least one
   well-known deviation flips relative to `count_bucket = 0` (canonical
   target: `hard 16 vs 10 → stand` instead of `hit`).
4. **Compactness**: the printed rule list is ≤ 25 clauses after greedy
   set-cover (the cheat sheet itself is 21 rows; some flexibility for
   soft-17 sub-cases).

For v1 (hit/stand only) the acceptance is restricted to the hard-totals
rows that involve only `H` or `S` (i.e. hands ≤ 8, 12, 13–16, 17+) plus
the `+2` deviation in (3); double/split rows are deferred to v1.5.

#### 5.8.3 Fallback: rule templates with fitted thresholds

ILP search can be unstable on the chart — particularly for borderline
cells where two actions are near-EV-indifferent. To protect delivery,
`maxims.eta` ships a fallback path activated by `--maxims=templates`:

1. start from the §5.8.1 row schemas as **rule templates** with free
   threshold variables (e.g. `total-in(13, 16), up-in({2..U}) → stand`
   with `U` free),
2. fit each `U` (and similar threshold parameters) by maximising
   coverage on the chart from §5.7,
3. report the same `(coverage %, error %)` columns as the ILP path so
   downstream snapshot tests are interchangeable.

This guarantees a presentable maxim list even if pure search produces
noisy clauses, while keeping the ILP path as the headline result.

---

## 6) App: `blackjack-demo`

Single binary with subcommands:

```console
eta run -- induce      # §5.3 — print discovered rules
eta run -- causal      # §5.4 — print do() EV sweeps + flip threshold
eta run -- learn [N]   # §5.6 — train, print discovered weight vector
                       #   --learn=joint        (default; headline)
                       #   --learn=supervised   (CI/debug baseline; §5.6.2)
eta run -- chart       # §5.7 — print strategy chart per count bucket
eta run -- maxims      # §5.8 — print induced basic-strategy rules
                       #   --maxims=ilp         (default)
                       #   --maxims=templates   (fallback; §5.8.3)
eta run -- all         # runs the full pipeline in order
```

Common flags: `--seed <n>`, `--debug-trace <path>` (writes the
§5.6.3 per-decision CSV; the first artefact to inspect when an
acceptance test regresses).

---

## 7) Testing strategy

1. **Unit (library)**:
   - `shoe`: deal determinism per seed; soft-ace totals.
   - `rules`: bust/blackjack/dealer-policy edge cases.
   - `induction`: induced rules equivalent to hand-written over fixed trace.
   - `causal`: `do()` sign of effect at known count thresholds.
   - `learn`: with trace from a *fixed* known-good policy, `w` recovers Hi-Lo
     within tolerance on a deterministic training configuration.
   - `strategy`: snapshot of chart at `count=0` matches a checked-in fixture.
   - `maxims`: induced top-N rule list contains the §5.8 acceptance
     clauses; snapshot of the printed list is stable.
2. **App smoke**: `eta run -- all --seed 42` exits 0
   and prints the expected section headers.
3. **CI stability**: default path is deterministic (fixed seeds/config),
   and tolerance thresholds are explicit in tests.

---

## 8) Build, run, and packaging contract

Mirrors [Package Commands](../guide/packages.md):

```console
cd cookbook/blackjack/blackjack
eta test

cd ../blackjack-demo
eta build
eta run -- all --seed 42
eta tree
```

Artifacts land under `.eta/target/<profile>/` per the standard layout.
No registry, no native sidecars. Workspace mode is *not* required for v1
but the layout is workspace-ready (see §11).

---

## 9) Staged roadmap

### B0 — Skeleton and packaging

1. `eta new` both packages, wire `eta add blackjack --path ../blackjack`,
2. `hello world` end-to-end build/run/test passes,
3. README stub linking to this plan.

Gate: `eta build && eta run` works in `blackjack-demo`.

### B1 — Shoe + rules (hand-written)

1. implement `shoe.eta` and `rules.eta`,
2. unit tests for soft aces, dealer policy, bust/blackjack.

Gate: rule unit tests green.

### B2 — Monte Carlo harness (unblocks downstream phases)

1. `mc.eta` with parallel sharding using the §5.5.2 compact
   representation (`[u8; 13]` shoe, fixed-size hand tuple),
2. trace recorder schema frozen,
3. deterministic replay checks run before B3–B6 begin so downstream
   phases build on stable rollout behavior.

Gate: deterministic outputs are reproducible with fixed seed and the
integration phases consume the harness successfully.

### B3 — Rule induction

1. predicate DSL + bounded search,
2. MDL scorer + boolean simplification,
3. equivalence test vs hand-written rules.

Gate: induced rules equivalent on 50k-hand trace.

### B4 — Causal model

1. DAG construction + `do()` queries,
2. EV sweep over count for `(16, 10)` flips at expected threshold.

Gate: causal flip test green.

### B5 — ML weight discovery

1. Torch model + joint optimization,
2. policy iteration loop,
3. weight recovery test (cosine ≥ 0.95) on the deterministic training path.

Gate: weight recovery test green.

### B6 — Strategy chart

1. CLP enumeration,
2. snapshot test for `count=0` chart,
3. printed multi-bucket charts in app.

Gate: snapshot stable across runs.

### B6.5 — Basic-strategy maxim induction

1. reuse §5.3 ILP engine over the §5.7 chart relation,
2. greedy set-cover for short, non-redundant rule list,
3. snapshot test for `count_bucket = 0` rule list,
4. acceptance asserts the §5.8 canonical clauses are present.

Gate: maxim list contains the documented clauses; snapshot stable.

### B7 — App polish + docs

1. subcommand UX, `--seed`,
2. cookbook README with copy-paste run instructions,
3. README/TLDR/next-steps cross-links,
4. optional notebook wrapper under `cookbook/notebooks/`.

Gate: `eta run -- all --seed 42` produces the documented output.

### B8 — v1.5: extend action set to `{double, split}`

Per the mechanics-vs-strategy distinction in §5.2.1:

1. **Given (mechanics):** extend `rules.eta` and `mc.eta` with the
   transition semantics of `double` (one card, doubled stake) and
   `split` (pair → two independent hands, each gets one card, stake
   doubled per hand; aces split typically capped at one card per
   sub-hand). These are encoded, not learned.
2. **Induced (eligibility):** add `can-split` and `can-double` to the
   §5.3 ILP target set; trace generator now emits the legal-action
   set per state. Acceptance: induced eligibility predicates equivalent
   to the hand-written ones (`can-split(H) ↔ pair(H)`,
   `can-double(H) ↔ |H|=2`).
3. **Causal:** extend `causal.eta` action variable domain to
   `{hit, stand, double, split}`; rerun §5.4 acceptance with the
   wider domain.
4. **Learning:** retrain §5.6 EV head with the expanded action head;
   `w` recovery acceptance unchanged.
5. **Chart:** rerun §5.7 enumeration over the full action set and
   verify §5.8.2 chart fidelity ≥ 98% against the §5.8.1 cheat sheet.
6. **Strategy maxims (induced):** rerun §5.8 ILP and verify the full
   v1.5 acceptance — including the canonical splitting/doubling
   clauses (split A/8, never split 10, double 11 vs 2–10, etc.).

Gate: full §5.8.2 v1.5 acceptance contract satisfied; eligibility
predicates induced equivalent to oracle.

---

## 10) Risks and mitigations

1. **ILP search blow-up** — Mitigation: tiny predicate DSL, depth cap,
   MDL pruning; only 4–5 target predicates. For §5.8 specifically,
   canonical-predicate bias and the post-processing normalisation in
   §5.8 keep clauses human-shaped; §5.8.3 template fallback exists if
   pure search is unstable.
2. **Weight non-identifiability (sign/scale)** — Mitigation: normalize to
   unit-norm and fix the sign of rank `5` to `+1` before comparison.
3. **MC variance hides EV flips** — Mitigation: paired-rollout variance
   reduction (same shoe seed for `hit` vs `stand` branch where possible);
   §5.8.2 acceptance uses tolerant agreement with `ε = 0.01`.
4. **Torch availability on CI** — Mitigation: keep a deterministic
   supervised path (§5.6.2), and keep test configs fixed so failures are
   debuggable and reproducible.
5. **Test flakiness from RNG** — Mitigation: every test fixes seeds and
   uses tolerance-based asserts with documented bounds.
6. **Long-running training loops slow feedback** — Mitigation: keep test
   configurations small but fixed, and preserve one deterministic path as
   the default verification route.
7. **Parallel rollout implementation can hide correctness bugs** —
   Mitigation: assert deterministic replay under fixed seeds and compare
   aggregate stats between worker counts in B2.
8. **Causal over-claim** — Mitigation: §5.4.1 caveat explicitly limits
   the identifiability claim; the headline framing is "intervention
   semantics + cross-paradigm direction agreement", not "EOR
   identification from a scalar count".
9. **Debugging blind spots** — Mitigation: `--debug-trace` (§5.6.3) is
   the first thing to inspect on any acceptance regression, and is
   wired into every subcommand.

### 10.1 Quality bar per phase (avoid "80% of everything, 100% of nothing")

Six interlocking phases is a real complexity risk. To keep delivery
focused, the plan explicitly tiers what must be production-grade
versus what may be deliberately minimal:

**Must be rock solid (the demo lives or dies on these):**

1. Monte Carlo harness (§5.5) — determinism, paired-rollout
   variance reduction.
2. Torch EV + weight recovery (§5.6) — the Hi-Lo punchline.
3. Strategy chart (§5.7) — snapshot-stable, ≥ 98% agreement with §5.8.1.

**May be deliberately minimal (correct but small):**

1. ILP engine (§5.3, §5.8) — tiny predicate DSL, depth ≤ 3, MDL pruning;
   the §5.8.3 template fallback exists precisely so this layer cannot
   block release.
2. Causal layer (§5.4) — minimal DAG, two `do()` queries; no
   identifiability machinery beyond what those queries need.
3. App polish (§B7) — single binary, plain text output, no TUI.

This tiering is referenced from each roadmap stage's gate so reviewers
can tell at a glance whether a stage is shipping its production form or
its minimal form.

---

## 11) Workspace-readiness (forward-looking)

Layout already groups two packages under one directory, so adopting
[workspace mode](./workspace_plan.md) later is mechanical:

```toml
# cookbook/blackjack/eta.toml (future)
[workspace]
members = ["blackjack", "blackjack-demo"]
default-members = ["blackjack-demo"]
```

No code changes required; only the root manifest is added when workspaces ship.

---

## 12) Acceptance criteria

The demo is complete when:

1. `cookbook/blackjack/blackjack` builds and `eta test` is green,
2. `cookbook/blackjack/blackjack-demo` builds and
   `eta run -- all --seed 42` exits 0,
3. induced rules are equivalent to hand-written rules on the trace fixture,
4. causal `do()` sweep shows the documented action flip,
5. learned weight vector matches Hi-Lo within cosine ≥ 0.95,
6. strategy chart snapshot is stable,
7. induced basic-strategy maxim list satisfies the §5.8.2 acceptance
   contract against the canonical S17 cheat sheet in §5.8.1
   (v1: hard-totals H/S rows + count-conditional deviation;
   v1.5: full chart fidelity ≥ 98%, every cheat-sheet row covered, ≤ 25
   printed clauses),
8. README, TLDR, and next-steps reference the demo.

