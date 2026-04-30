## std.causal — Comprehensive Implementation Plan

**Goal.** Take Eta's current back-door identification prototype to a
research-grade causal-inference engine: ADMGs with bidirected edges,
linear-time d-separation, the three rules of do-calculus as first-class
predicates, the **ID** and **IDC** algorithms (Shpitser & Pearl, 2006),
**hedge** detection, mediation (NDE/NIE), transportability, modern
estimators (IPW, g-formula, AIPW, TMLE), structure learning (PC/FCI),
DOT/Mermaid rendering, and a TAP-style test pack covering every
canonical DAG (Front-door, IV, Napkin, Bow, M-bias, Verma, …).

The plan is expressed as **Eta code** organized into 12 milestones
(M0 … M11). Each milestone lists: (a) module surface, (b) reference
implementation in Eta, (c) tests. Anything marked `;; STUB` is a
signature-only placeholder so milestones can be merged independently.

---

### Module layout

```text
stdlib/std/
  causal.eta              ;; M0–M2  graph + d-sep + rules
  causal/admg.eta         ;; M1     mixed graphs, latent projection
  causal/identify.eta     ;; M3–M4  ID, IDC, hedge witness
  causal/adjustment.eta   ;; M5     generalized adjustment, IV, front-door
  causal/mediation.eta    ;; M6     NDE / NIE / CDE
  causal/transport.eta    ;; M7     s-nodes, sBD, sID
  causal/counterfactual.eta ;; M8   ID*, IDC*, twin networks
  causal/estimate.eta     ;; M9     IPW / g-form / AIPW / TMLE / bootstrap
  causal/learn.eta        ;; M10    PC / FCI / NOTEARS hooks
  causal/render.eta       ;; M11    DOT, Mermaid, LaTeX

stdlib/tests/
  causal.test.eta             ;; existing
  causal-dsep.test.eta        ;; M2
  causal-id.test.eta          ;; M3
  causal-idc.test.eta         ;; M4
  causal-adjustment.test.eta  ;; M5
  causal-mediation.test.eta   ;; M6
  causal-transport.test.eta   ;; M7
  causal-counterfactual.test.eta ;; M8
  causal-estimate.test.eta    ;; M9
  causal-learn.test.eta       ;; M10
  causal-render.test.eta      ;; M11
  causal-canon.test.eta       ;; cross-milestone canonical-graph regression
```

---

## M0 — Foundational hygiene (P0, 1–2 days)

Fix correctness bugs and surface an honest API before adding features.

### Bugs to fix in `stdlib/std/causal.eta`

```scheme
;; (1) do:estimate-effect must marginalize over Z, not pick a single z-val.
(defun do:estimate-effect (y-var x-var x-val z-set data)
  (let ((z-values (%distinct-tuples data z-set)))
    (foldl (lambda (acc z-val)
             (let* ((stratum (filter (lambda (o)
                                       (and (equal? (%obs-get o x-var) x-val)
                                            (%obs-matches-z o z-set z-val)))
                                     data))
                    (p-z     (do:marginal-prob data z-set z-val)))
               (if (null? stratum)
                   acc
                   (+ acc (* (do:conditional-mean stratum y-var) p-z)))))
           0
           z-values)))

;; (2) Force float division so counts don't truncate.
(defun do:marginal-prob (data z-set z-val)
  (if (null? data) 0.0
      (exact->inexact (/ (count (lambda (o)
                                  (%obs-matches-z o z-set z-val))
                                data)
                         (length data)))))

;; (3) Validate inputs: cycle check + var-membership.
(defun dag:valid? (dag)
  (and (every? (lambda (e)
                 (and (= (length e) 3) (eq? (cadr e) '->)))
               dag)
       (not (dag:cyclic? dag))))

(defun dag:cyclic? (dag)
  (any? (lambda (n) (%member? n (dag:descendants dag n)))
        (dag:nodes dag)))

(defun dag:topo-sort (dag)
  ;; Kahn's algorithm; raises 'cyclic-graph if dag:cyclic?.
  ...)

(defun %assert-var (dag v)
  (if (%member? v (dag:nodes dag)) v
      (error 'unknown-variable v)))
```

### Tests (`stdlib/tests/causal.test.eta`, additions)

```scheme
(test "do:estimate-effect marginalises over Z"
  (let ((d '(((x . 1) (z . 0) (y . 4))
             ((x . 1) (z . 1) (y . 8))
             ((x . 0) (z . 0) (y . 1))
             ((x . 0) (z . 1) (y . 2)))))
    (assert-near (do:estimate-effect 'y 'x 1 '(z) d) 6.0 1e-9)))

(test "do:marginal-prob returns a float"
  (assert (inexact? (do:marginal-prob
                      '(((z . 0)) ((z . 1)) ((z . 1))) '(z) '((z . 1))))))

(test "dag:cyclic? detects 2-cycle"
  (assert (dag:cyclic? '((a -> b) (b -> a)))))

(test "dag:topo-sort yields a valid linear extension"
  (let ((order (dag:topo-sort '((a -> b) (b -> c) (a -> c)))))
    (assert (< (index-of 'a order) (index-of 'b order)))
    (assert (< (index-of 'b order) (index-of 'c order)))))
```

---

## M1 — ADMGs & latent projection (P1, 3–5 days)

Extend the edge syntax with bidirected edges (`(u <-> v)`) representing
unobserved confounders.  Provide the **latent projection** that converts
a DAG with hidden variables into an equivalent ADMG over observed
variables.

### `stdlib/std/causal/admg.eta`

```scheme
(module std.causal.admg
  (import std.core) (import std.collections)
  (export
    admg:directed admg:bidirected admg:nodes
    admg:district admg:districts
    admg:project              ;; latent projection of (DAG, latents) -> ADMG
    admg:moralize             ;; ancestral moralization
    admg:ancestors            ;; same as DAG ancestors but tolerates <->
    admg:c-component admg:c-components)
  (begin

    ;; Edge predicates
    (defun %edge-kind (e) (cadr e))         ;; '->  or  '<->
    (defun admg:directed (g)
      (filter (lambda (e) (eq? (%edge-kind e) '->)) g))
    (defun admg:bidirected (g)
      (filter (lambda (e) (eq? (%edge-kind e) '<->)) g))

    (defun admg:nodes (g)
      (foldr (lambda (e acc)
               (%adjoin (car e) (%adjoin (caddr e) acc)))
             '() g))

    ;; Bidirected-connected component containing v.
    (defun admg:district (g v)
      (let* ((bi (admg:bidirected g))
             (neigh (lambda (u)
                      (foldr (lambda (e acc)
                               (cond ((eq? (car e)   u) (cons (caddr e) acc))
                                     ((eq? (caddr e) u) (cons (car e)   acc))
                                     (else acc)))
                             '() bi))))
        (%bfs-closure neigh (list v))))

    ;; Districts = equivalence classes of <-> reachability (a.k.a. C-components).
    (defun admg:districts (g)
      (%partition (admg:nodes g) (lambda (v) (admg:district g v))))

    (defun admg:c-component  (g v) (admg:district g v))
    (defun admg:c-components (g)   (admg:districts g))

    ;; Latent projection (Verma & Pearl 1990):
    ;;   For each pair (u,v) of observed nodes:
    ;;     - draw u -> v if exists directed path through latents only.
    ;;     - draw u <-> v if exists divergent path L <- ... -> u, ... -> v
    ;;       through latents only (a "bidirected-connecting" path).
    (defun admg:project (dag latents)
      (let* ((obs (filter (lambda (n) (not (%member? n latents)))
                          (dag:nodes dag)))
             (dir (admg:%project-directed   dag latents obs))
             (bid (admg:%project-bidirected dag latents obs)))
        (append dir bid)))

    ;; STUB helpers — see implementation note in this section.
    (defun admg:%project-directed   (dag latents obs) ...)
    (defun admg:%project-bidirected (dag latents obs) ...)

    ;; Ancestral moralization: take ancestors of S, moralize, drop directions.
    (defun admg:moralize (g s)
      (let* ((anc (foldr %adjoin s
                         (foldr (lambda (v acc)
                                  (foldr %adjoin acc (admg:ancestors g v)))
                                '() s)))
             (subg (filter (lambda (e)
                             (and (%member? (car e)   anc)
                                  (%member? (caddr e) anc)))
                           g))
             (moral (admg:%add-marriages subg)))
        (admg:%undirected moral)))

    ))
```

### Tests (`causal-admg.test.eta`)

```scheme
(test "bidirected district groups latently confounded vars"
  (let ((g '((a -> b) (b -> c) (a <-> c))))
    (assert (set= (admg:district g 'a) '(a c)))
    (assert (set= (admg:district g 'b) '(b)))))

(test "latent projection on Bow graph yields X<->Y, X->Y"
  ;;     U is latent;  U->X, U->Y, X->Y
  (let* ((dag    '((u -> x) (u -> y) (x -> y)))
         (g      (admg:project dag '(u))))
    (assert (member '(x -> y)  g))
    (assert (member '(x <-> y) g))))
```

---

## M2 — Linear-time d-separation (P0, 2 days)

Replace the exponential simple-path enumerator with **Bayes-ball**
(Shachter 1998), which traverses the moralized ancestral subgraph in
*O(|V|+|E|)*.

```scheme
(defun dag:d-separated? (dag x-set y-set z-set)
  (not (dag:d-connected? dag x-set y-set z-set)))

(defun dag:d-connected? (dag x-set y-set z-set)
  ;; Bayes-ball: BFS on (node, direction) pairs.  See Shachter 1998 Fig. 3.
  (letrec
      ((reachable (%bayes-ball dag x-set z-set)))
    (any? (lambda (y) (%member? y reachable)) y-set)))

(defun %bayes-ball (dag start z-set)
  (let* ((visited (make-hashtable))                    ; (node, dir) -> #t
         (queue   (foldr (lambda (s acc) (cons (cons s 'up) acc))
                         '() start)))
    (let loop ((q queue) (acc '()))
      (cond
        ((null? q) acc)
        (else
         (let* ((pair (car q))
                (v    (car pair))
                (dir  (cdr pair)))
           (if (hashtable-ref visited pair #f)
               (loop (cdr q) acc)
               (begin
                 (hashtable-set! visited pair #t)
                 (let* ((acc2 (if (%member? v z-set) acc (cons v acc)))
                        (nbrs (%bayes-ball-step dag v dir z-set)))
                   (loop (append (cdr q) nbrs) acc2))))))))))

(defun %bayes-ball-step (dag v dir z-set)
  ;; Implements the visit-rules table:
  ;;   non-collider passes ball through if v ∉ Z
  ;;   collider passes ball through if v ∈ Z or any descendant of v ∈ Z
  ...)
```

### Tests (`causal-dsep.test.eta`)

```scheme
;; Pearl's 5-node test suite
(define p '((a -> b) (b -> c) (c -> d) (e -> b) (e -> d)))
(test "chain a-b-c blocked by b"      (assert (dag:d-separated? p '(a) '(c) '(b))))
(test "fork e-b/d open"               (assert (dag:d-connected? p '(b) '(d) '())))
(test "collider b activated by cond"  (assert (dag:d-connected? p '(a) '(e) '(b))))
(test "Bayes-ball O(V+E) on 1000-node chain"
  (let ((big (%make-chain 1000)))
    (assert (dag:d-separated? big '(n0) '(n999) '(n500)))))
```

Cross-validate against `pgmpy` ground truth using a generator script in
`scripts/check_dsep_against_pgmpy.py` (committed as fixture data).

---

## M3 — The three rules + ID algorithm (P1, 1–2 weeks)

Promote the rules out of the example file into `std.causal`, then build
the **ID algorithm** of Shpitser & Pearl (2006) on top.

### Rules

```scheme
(defun dag:mutilate-do  (dag x-set)
  (filter (lambda (e) (not (%member? (caddr e) x-set))) dag))   ; remove parents->x
(defun dag:mutilate-see (dag x-set)
  (filter (lambda (e) (not (%member? (car e)   x-set))) dag))   ; remove x->children

(defun do-rule1-applies? (dag y-set x-set z-set w-set)
  ;; Insert/delete observation:
  ;;   P(y | do(x), z, w) = P(y | do(x), w)  iff
  ;;   (Y ⟂ Z | X, W) in G_{X̄}
  (dag:d-separated? (dag:mutilate-do dag x-set) y-set z-set
                    (append x-set w-set)))

(defun do-rule2-applies? (dag y-set x-set z-set w-set)
  ;; Action/observation exchange:
  ;;   P(y | do(x), do(z), w) = P(y | do(x), z, w)  iff
  ;;   (Y ⟂ Z | X, W) in G_{X̄, Z_}                 (Z arrows out removed)
  (let ((g (dag:mutilate-see (dag:mutilate-do dag x-set) z-set)))
    (dag:d-separated? g y-set z-set (append x-set w-set))))

(defun do-rule3-applies? (dag y-set x-set z-set w-set)
  ;; Insert/delete action; let Z(W) = Z \ An(W)_{G_X̄}.
  (let* ((gx (dag:mutilate-do dag x-set))
         (zw (filter (lambda (z)
                       (not (any? (lambda (w)
                                    (%member? z (dag:ancestors gx w)))
                                  w-set)))
                     z-set))
         (g  (dag:mutilate-do gx zw)))
    (dag:d-separated? g y-set z-set (append x-set w-set))))
```

### ID algorithm (operates on an ADMG)

```scheme
;; Estimand AST:
;;   (P set)                       — joint marginal P(set)
;;   (sum vars expr)               — Σ_vars expr
;;   (prod e1 e2 …)                — ∏ factors
;;   (cond-on expr conditioning)   — division by another estimand
;;   (do expr interventions)       — interventional (only at the root)
;;   (fail hedge)                  — non-identifiable; carries witness

(defun id (g y x)
  ;; (id G Y X)  ⇒  estimand for P(Y | do(X))
  (id-rec g (admg:nodes g) y x))

(defun id-rec (g v y x)
  ;; Lines numbered as in Shpitser & Pearl 2006, Fig. 3.
  (let* ((anc-y (admg:ancestors g y)))
    (cond
      ;; Line 1:  if X = ∅
      ((null? x)
       (list 'sum (set-diff v y) (list 'P v)))

      ;; Line 2:  if V \ An(Y)_G ≠ ∅
      ((not (set= v anc-y))
       (let* ((g2 (admg:induced-subgraph g anc-y)))
         (id-rec g2 anc-y y (set-intersect x anc-y))))

      ;; Line 3:  let W = (V \ X) \ An(Y)_{G_X̄}
      ((let* ((gx (admg:mutilate-do g x))
              (w  (set-diff (set-diff v x) (admg:ancestors gx y))))
         (and (not (null? w))
              (id-rec g v y (set-union x w)))))

      ;; Line 4:  if C(G \ X) has > 1 c-component
      ((let ((cs (admg:c-components (admg:remove-nodes g x))))
         (and (> (length cs) 1)
              (let ((factors (map* (lambda (s)
                                     (id-rec g v s (set-diff v s)))
                                   cs)))
                (list 'sum (set-diff v (set-union y x))
                      (cons 'prod factors))))))

      ;; Line 5:  hedge check — c-component of G equals V?
      ((let ((cs (admg:c-components g)))
         (and (= (length cs) 1)
              (set= (car cs) v)
              (list 'fail (list 'hedge g (admg:c-components
                                          (admg:remove-nodes g x)))))))

      ;; Line 6:  S ∈ C(G), S ⊆ V \ X  →  factor over topological order
      ((let* ((cs    (admg:c-components (admg:remove-nodes g x)))
              (s     (find (lambda (c) (set-subset? c (set-diff v x))) cs))
              (order (dag:topo-sort (admg:directed g))))
         (and s
              (list 'sum (set-diff s y)
                    (cons 'prod
                          (map* (lambda (vi)
                                  (let ((pre (%before vi order)))
                                    (list 'cond-on (list 'P (list vi))
                                          pre)))
                                s))))))

      ;; Line 7:  S′ ⊋ S where S ⊆ S′ ∈ C(G)
      (else
       (let* ((cs   (admg:c-components (admg:remove-nodes g x)))
              (s    (find (lambda (c) (set-subset? c (set-diff v x))) cs))
              (s*   (find (lambda (c) (set-subset? s c)) (admg:c-components g)))
              (g*   (admg:induced-subgraph g s*)))
         (id-rec g* s* y (set-intersect x s*)))))))
```

### Tests (`causal-id.test.eta`)

```scheme
(test "ID: trivial direct effect"
  (assert (id-equiv? (id '((x -> y)) '(y) '(x))
                     '(P (y x)))))

(test "ID: front-door"
  ;; X -> M -> Y, X <-> Y
  (let ((g '((x -> m) (m -> y) (x <-> y))))
    (assert (id-equiv? (id g '(y) '(x))
                       '(sum (m)
                          (prod (P (m x))
                                (sum (x*) (prod (P (y x* m))
                                                (P (x*))))))))))

(test "ID: bow graph is a hedge"
  (let* ((g '((x -> y) (x <-> y)))
         (r (id g '(y) '(x))))
    (assert (eq? (car r) 'fail))
    (assert (eq? (car (cdr r)) 'hedge))))

(test "ID: napkin graph is identifiable"
  (let ((g '((w1 -> w2) (w2 -> x) (x -> y) (w1 <-> x) (w1 <-> y))))
    (assert (not (eq? (car (id g '(y) '(x))) 'fail)))))

(test "ID: instrumental variable is NOT identifiable non-parametrically"
  (let ((g '((z -> x) (x -> y) (x <-> y))))
    (assert (eq? (car (id g '(y) '(x))) 'fail))))
```

---

## M4 — IDC algorithm + estimand simplifier (P1, 3–4 days)

```scheme
(defun idc (g y x z)
  ;; (idc G Y X Z) ⇒ estimand for P(Y | do(X), Z) per Shpitser-Pearl 2006.
  ;; Repeatedly try Rule 2 to convert observations in Z to interventions.
  (let loop ((z z))
    (let ((moveable (find (lambda (zi)
                            (do-rule2-applies? g y (set-union x (set-diff z (list zi)))
                                                 (list zi) '()))
                          z)))
      (if moveable
          (idc g y (set-union x (list moveable))
                   (set-diff z (list moveable)))
          (let ((joint (id g (set-union y z) x)))
            (if (eq? (car joint) 'fail) joint
                (list 'cond-on joint (list 'P z))))))))

(defun do:simplify (estimand)
  ;; Algebraic simplifier: removes (sum ∅ e) → e, (prod e) → e,
  ;; collapses nested sums, eliminates conditioning when independent,
  ;; uses dag:d-separated? to drop irrelevant conditioning variables.
  ...)
```

### Tests (`causal-idc.test.eta`)

```scheme
(test "IDC: conditional direct effect"
  (let ((g '((x -> y) (z -> y))))
    (assert (id-equiv? (idc g '(y) '(x) '(z)) '(P (y x z))))))

(test "IDC: bow with conditioning still a hedge"
  (let ((g '((x -> y) (x <-> y))))
    (assert (eq? (car (idc g '(y) '(x) '())) 'fail))))
```

---

## M5 — Generalized adjustment, Front-door, IV (P1, 4–5 days)

### Adjustment

```scheme
;; Shpitser–VanderWeele–Robins generalized adjustment criterion.
(defun dag:gac? (g x y z)
  (and (every? (lambda (zi)
                 (not (%member? zi (admg:proper-forbidden-nodes g x y))))
               z)
       (every? (lambda (p) (not (%path-active? g p z)))
               (admg:proper-causal-paths g x y))
       (every? (lambda (p) (not (%path-active? g p z)))
               (admg:proper-non-causal-paths g x y))))

;; Polynomial-delay enumeration of *minimal* adjustment sets
;; (Van der Zander & Liśkiewicz 2014).
(defun dag:minimal-adjustments (g x y observed)
  (vdz-enumerate g x y observed 'minimal))
```

### Front-door & IV

```scheme
(defun front-door? (g x y m)
  (and (every? (lambda (p) (%member-any? m p))
               (admg:directed-paths g x y))
       (dag:d-separated? (dag:mutilate-do g (list x))
                         (list x) m '())
       (dag:satisfies-backdoor? g m y (list x))))

(defun do:front-door-formula (y x m)
  `(sum ,m
        (prod (P ,m ,x)
              (sum (,x)
                   (prod (P ,y ,x ,m) (P ,x))))))

(defun iv? (g z x y)
  (and (admg:has-directed-edge? g z x)
       (dag:d-separated? (dag:mutilate-do g (list x)) (list z) (list y) '())
       (not (dag:d-separated? g (list z) (list x) '()))))
```

### Tests (`causal-adjustment.test.eta`)

```scheme
(test "front-door criterion holds on smoking→tar→cancer"
  (assert (front-door? '((s -> t) (t -> c) (s <-> c)) 's 'c '(t))))

(test "M-bias: empty Z is the only valid adjustment"
  (let* ((g '((u1 -> x) (u2 -> y) (u1 -> z) (u2 -> z))))
    (assert (member '() (dag:minimal-adjustments g 'x 'y '(z))))
    (assert (not (member '(z) (dag:minimal-adjustments g 'x 'y '(z)))))))

(test "Verma constraint: id reproduces the dormant independence"
  ...)

(test "IV: instrumental variable detected"
  (assert (iv? '((z -> x) (x -> y) (x <-> y)) 'z 'x 'y)))
```

---

## M6 — Mediation analysis (P1, 2–3 days)

```scheme
;; Natural direct, natural indirect, controlled direct effects.
;; For binary X with reference x* and contrast x:
;;   NDE = E[Y(x, M(x*))] - E[Y(x*, M(x*))]
;;   NIE = E[Y(x, M(x))]  - E[Y(x, M(x*))]
;;   TE  = NDE + NIE
(defun do:nde (g y x m data x* x)
  (- (mediation-formula data y x m x*  x)        ; cross-world mean
     (mediation-formula data y x m x*  x*)))

(defun do:nie (g y x m data x* x)
  (- (mediation-formula data y x m x   x)
     (mediation-formula data y x m x*  x)))

(defun mediation-formula (data y x m treatment-for-y treatment-for-m)
  ;; Σ_m  E[Y | x=treatment-for-y, M=m] · P(M=m | x=treatment-for-m)
  ...)
```

### Tests (`causal-mediation.test.eta`)

```scheme
(test "Linear SEM: NDE+NIE = TE on simulated data"
  (let* ((data (%simulate-linear-mediation 5000 0.7 0.4 0.3))
         (te   (do:ate data 'y 'x))
         (nde  (do:nde nil 'y 'x '(m) data 0 1))
         (nie  (do:nie nil 'y 'x '(m) data 0 1)))
    (assert-near te (+ nde nie) 0.05)))
```

---

## M7 — Transportability & selection bias (P2, 1 week)

```scheme
;; Selection diagrams: edges (s -> v) where s is an "S-node" mark.
(defun s-node? (n) (eq? (string-ref (symbol->string n) 0) #\S))

(defun do:transport (g* g y x)
  ;; sID algorithm (Bareinboim & Pearl 2014).
  ...)

(defun do:sBD? (g x y z s-nodes)
  (and (dag:satisfies-backdoor? g x y z)
       (every? (lambda (s)
                 (dag:d-separated? g (list y) (list s) (append (list x) z)))
               s-nodes)))
```

Tests cover Bareinboim–Pearl's transportability examples
(Figs. 1–6 of the 2014 paper).

---

## M8 — Counterfactuals (P2, 1–2 weeks)

```scheme
(defun twin-network (g intervened-vars)
  ;; Build the parallel-worlds graph: duplicate every variable into a
  ;; counterfactual copy with primed names; share unobserved exogenous parents.
  ...)

(defun id* (g gamma)
  ;; Shpitser & Pearl 2007 Algorithm ID*.
  ;; gamma is a conjunction of counterfactual events Y_x = y.
  ...)

(defun idc* (g gamma delta) ...)

;; Effect of treatment on the treated.
(defun do:ett (g y x x*)
  (idc* g `((,y ,x)) `((,x ,x*))))
```

### Tests (`causal-counterfactual.test.eta`)

```scheme
(test "ETT identifiable in front-door graph"
  (let ((g '((x -> m) (m -> y) (x <-> y))))
    (assert (not (eq? (car (do:ett g 'y 'x 0)) 'fail)))))

(test "Probability of necessity bounded for binary treatment"
  ...)
```

---

## M9 — Estimation backends (P1, 1–2 weeks)

Plug into existing `std.stats`, `std.torch`, and AAD modules.

```scheme
(module std.causal.estimate
  (import std.core) (import std.stats) (import std.torch)
  (export
    do:ate do:ate-ipw do:ate-gformula do:ate-aipw do:ate-tmle
    do:bootstrap-ci do:propensity-score
    do:e-value do:rosenbaum-bound)

  (begin

    ;; 1. Plug-in (G-formula): regression-based outcome model.
    (defun do:ate-gformula (data y x z model-fit model-predict)
      (let* ((mu  (model-fit data y (cons x z)))
             (mu1 (mean (map* (lambda (o) (model-predict mu (set-x o x 1))) data)))
             (mu0 (mean (map* (lambda (o) (model-predict mu (set-x o x 0))) data))))
        (- mu1 mu0)))

    ;; 2. IPW: stabilised inverse propensity weights.
    (defun do:ate-ipw (data y x z)
      (let* ((p   (do:propensity-score data x z))
             (w   (map* (lambda (o pi)
                          (if (= (%obs-get o x) 1)
                              (/ 1.0 pi)
                              (/ 1.0 (- 1.0 pi))))
                        data p))
             (yw1 (%weighted-mean data y w (lambda (o) (= (%obs-get o x) 1))))
             (yw0 (%weighted-mean data y w (lambda (o) (= (%obs-get o x) 0)))))
        (- yw1 yw0)))

    ;; 3. AIPW (doubly robust): combines (1) and (2).
    (defun do:ate-aipw (data y x z model-fit model-predict)
      ...)

    ;; 4. TMLE: targeted update via fluctuation submodel; uses std.torch
    ;;    autograd to fit ε in one pass.
    (defun do:ate-tmle (data y x z model-fit model-predict)
      ...)

    ;; 5. Bootstrap CIs.
    (defun do:bootstrap-ci (estimator data n-boot alpha)
      (let* ((thetas (map* (lambda (_)
                             (estimator (%bootstrap-sample data)))
                           (iota n-boot))))
        (list (quantile thetas (/ alpha 2))
              (quantile thetas (- 1 (/ alpha 2))))))

    ;; 6. Propensity score via logistic regression (uses std.torch).
    (defun do:propensity-score (data x z)
      (let* ((xs (%matrix-of data z))
             (ys (%vector-of data x))
             (m  (sequential (linear (length z) 1) (sigmoid-layer))))
        (do:%fit-logistic m xs ys 200 0.05)
        (map* (lambda (xi) (item (forward m xi))) (%row-tensors xs))))

    ;; 7. Sensitivity: VanderWeele's E-value.
    (defun do:e-value (rr)
      (+ rr (sqrt (* rr (- rr 1)))))
    ))
```

### Tests (`causal-estimate.test.eta`)

```scheme
(test "AIPW unbiased when either model is correct (DR)"
  (let* ((data    (%simulate-confounded 2000))
         (correct (do:ate-aipw data 'y 'x '(z) ols-fit ols-predict)))
    (assert-near correct 1.0 0.10)))

(test "TMLE matches AIPW within MC error"
  (let* ((d   (%simulate-confounded 5000))
         (a   (do:ate-aipw d 'y 'x '(z) ols-fit ols-predict))
         (t   (do:ate-tmle d 'y 'x '(z) ols-fit ols-predict)))
    (assert-near a t 0.05)))

(test "Bootstrap 95% CI covers truth on synthetic"
  (let* ((d      (%simulate-confounded 1000))
         (ci     (do:bootstrap-ci
                   (lambda (b) (do:ate-aipw b 'y 'x '(z) ols-fit ols-predict))
                   d 500 0.05)))
    (assert (and (<= (car ci) 1.0) (>= (cadr ci) 1.0)))))

(test "E-value: RR=2 ⇒ E≈3.41"
  (assert-near (do:e-value 2.0) 3.4142 1e-3))
```

---

## M10 — Structure learning (P2, 1–2 weeks)

```scheme
(module std.causal.learn
  (import std.core) (import std.stats) (import std.torch)
  (export
    learn:pc learn:fci learn:ges learn:notears
    learn:ci-test:fisher-z
    learn:ci-test:chi2)
  (begin

    (defun learn:ci-test:fisher-z (data x y z alpha)
      ;; Partial correlation Fisher-z test; returns (independent? . p-value)
      ...)

    (defun learn:pc (data alpha ci-test)
      ;; PC algorithm: skeleton via CI tests, then collider orientation,
      ;; then Meek's propagation rules. Returns a CPDAG.
      (let* ((skeleton (%pc-skeleton data alpha ci-test))
             (oriented (%pc-orient-colliders skeleton))
             (cpdag    (%meek-rules oriented)))
        cpdag))

    (defun learn:fci (data alpha ci-test)
      ;; FCI for latent confounders: returns a PAG.
      ...)

    (defun learn:notears (data lambda1 max-iter)
      ;; Continuous structure learning via std.torch:
      ;;   minimize  ½‖X - XW‖² + λ‖W‖₁  s.t.  trace(eᴹᵒᵈ⁽ᵂ°ᵂ⁾) - d = 0
      ...)
    ))
```

### Tests (`causal-learn.test.eta`)

```scheme
(test "PC recovers chain a->b->c on linear-Gaussian sim"
  (let* ((d  (%simulate-chain 1000))
         (g  (learn:pc d 0.05 learn:ci-test:fisher-z)))
    (assert (member '(a -> b) g))
    (assert (member '(b -> c) g))))

(test "NOTEARS recovers sparse W on 5-node random DAG"
  ...)
```

---

## M11 — Rendering & ergonomics (P0, 1 day)

```scheme
(module std.causal.render
  (import std.core) (import std.io)
  (export dag:->dot dag:->mermaid dag:->latex)
  (begin

    (defun dag:->dot (g . opts)
      (let ((title (or (assoc-ref 'title opts) "Causal DAG"))
            (lines (map* (lambda (e)
                           (case (cadr e)
                             ((->)  (string-append "  " (sym (car e))
                                                   " -> " (sym (caddr e)) ";"))
                             ((<->) (string-append "  " (sym (car e))
                                                   " -> " (sym (caddr e))
                                                   " [dir=both, style=dashed];"))))
                         g)))
        (string-join (append (list (string-append "digraph \"" title "\" {")
                                   "  rankdir=LR;")
                             lines
                             (list "}"))
                     "\n")))

    (defun dag:->mermaid (g) ...)
    (defun dag:->latex   (g) ...)
    ))
```

`define-dag` macro (validates at expansion time, supports `<->`):

```scheme
(define-syntax define-dag
  (syntax-rules ()
    ((_ name edge ...)
     (define name
       (let ((g '(edge ...)))
         (if (dag:valid? g) g
             (error 'invalid-dag g)))))))
```

---

## Canonical-graph regression suite (`causal-canon.test.eta`)

A single file that locks in correct identification status across the
canonical literature:

```scheme
(define canon
  ;; (name dag latents y x expected-status)
  '((direct       ((x -> y))                                    () y x ident)
    (chain        ((x -> m) (m -> y))                            () y x ident)
    (back-door    ((z -> x) (z -> y) (x -> y))                   () y x ident)
    (front-door   ((x -> m) (m -> y))                            (u) y x ident)   ; with U->X,Y
    (napkin       ((w1 -> w2) (w2 -> x) (x -> y))                (u) y x ident)
    (bow          ((x -> y))                                     (u) y x hedge)
    (iv           ((z -> x) (x -> y))                            (u) y x hedge)
    (m-bias       ((u1 -> x) (u2 -> y) (u1 -> z) (u2 -> z))      () y x ident)
    (verma        ;; 4-node Verma graph
                  ((a -> b) (b -> c) (c -> d) (a <-> c) (b <-> d)) () d a ident)))

(for-each
  (lambda (row)
    (let* ((name (car row)) (dag* (cadr row)) (lat (caddr row))
           (y (cadddr row)) (x (cadr (cdddr row)))
           (expected (caddr (cdddr row)))
           (g  (admg:project (%add-latent-edges dag* lat) lat))
           (r  (id g (list y) (list x))))
      (test (string-append "canon: " (symbol->string name))
        (case expected
          ((ident) (assert (not (eq? (car r) 'fail))))
          ((hedge) (assert (eq? (car r) 'fail)))))))
  canon)
```

---

## Documentation deliverables

| File | Update |
|---|---|
| `docs/guide/reference/causal.md` | Move "rules live in example file" wording; add **Limitations** section pre-M3, then refresh after each milestone |
| `docs/guide/reference/causal-id.md` | New: ID/IDC walkthrough w/ Mermaid diagrams (Front-door, Napkin, Bow, IV) |
| `docs/guide/reference/causal-estimation.md` | New: IPW/AIPW/TMLE recipes, integration with `std.torch` |
| `docs/guide/reference/causal-counterfactual.md` | New: ID*, ETT, twin networks |
| `examples/do-calculus/canon/` | One `.eta` per canonical graph, runnable via `etai` |
| `examples/notebooks/causal-tour.ipynb` | Interactive Jupyter tour using `dag:->mermaid` |

---

## Milestone summary

| ID | Scope | LoC est. | Tests | Risk |
|---:|---|---:|---:|---|
| M0 | Bug-fix, validators, topo-sort | 150 | 12 | Low |
| M1 | ADMG + latent projection | 350 | 18 | Med |
| M2 | Bayes-ball d-separation | 200 | 25 | Low |
| M3 | 3 rules + ID + hedge | 600 | 30 | High |
| M4 | IDC + simplifier | 250 | 12 | Med |
| M5 | GAC, front-door, IV | 400 | 20 | Med |
| M6 | Mediation (NDE/NIE/CDE) | 200 | 10 | Low |
| M7 | Transportability (sID) | 500 | 15 | High |
| M8 | Counterfactuals (ID*, IDC*) | 700 | 18 | High |
| M9 | Estimation (IPW/AIPW/TMLE) | 600 | 25 | Med |
| M10 | Structure learning (PC/FCI/NOTEARS) | 900 | 20 | High |
| M11 | DOT/Mermaid render + macro | 150 | 8 | Low |
|    | **Totals** | **~5000** | **~213** | |

---

## Acceptance criteria (definition of "world class")

1. **Every canonical DAG** in the Shpitser–Pearl/Bareinboim–Pearl
   literature passes the regression suite with the documented status.
2. **D-separation** runs in linear time and matches `pgmpy` on
   ≥1000 random graphs (fuzz harness).
3. **ID algorithm** returns either a closed-form estimand or a hedge
   witness for every input — no silent `#f`.
4. **Estimators** are double-robust (AIPW) and targeted (TMLE), produce
   bootstrap CIs, and integrate with `std.torch` propensity / outcome
   models.
5. **Documentation**: each module has a reference page, a worked
   example, and a Mermaid diagram of its key DAG.
6. **Coverage**: TAP test count ≥ 200, line coverage on `std.causal.*`
   modules ≥ 90 %.
7. **Performance**: identification on a 50-node ADMG completes in ≤ 50 ms
   on commodity hardware; PC on 20 nodes / 5000 samples in ≤ 5 s.

When all seven criteria are met, Eta's causal stack will be on par with
the union of DoWhy + Ananke + Y0, with the unique advantages of native
AAD, libtorch integration, and CLP-based bound reasoning that no other
toolkit currently combines.

<!-- Implementation note (2026-04-30): M0, M1, M2, M3, M4, M5, M6, M7, and M8 are implemented in-tree. -->
