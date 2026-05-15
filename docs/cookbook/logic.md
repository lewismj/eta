---
title: "Cookbook — Logic"
---

# Logic

Logic programming with Eta's built-in unification engine, constraint logic
programming (CLP), and symbolic rewriting.

**Run any example with:**
```bash
etai cookbook/logic/<file>.eta
```

---

## Contents

- [Unification Primitives](#unification-primitives)
- [Relational Logic Programming](#relational-logic-programming)
- [N-Queens](#n-queens)
- [SEND + MORE = MONEY](#send--more--money)
- [Boolean Simplifier](#boolean-simplifier)
- [Symbolic Differentiation](#symbolic-differentiation)

---

## Unification Primitives

`cookbook/logic/unification.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/logic/unification.eta)

Eta provides six built-in unification primitives: `logic-var`, `unify`,
`deref-lvar`, `logic-var?`, `trail-mark`, and `unwind-trail`. Together they
give you the core machinery of a Prolog engine.

```scheme
(module unification
  (import std.io std.collections std.logic)
  (begin

    ;; 1. Basic unification
    (define x (logic-var))
    (unify x 42)
    (println (deref-lvar x))                ; => 42

    (println (unify 'hello 'hello))         ; => #t
    (println (unify 'hello 'world))         ; => #f

    ;; 2. Structural unification
    ;; Unify (f ?a 2) with (f 1 ?b)
    (define a (logic-var))
    (define b (logic-var))
    (unify (cons 'f (cons a (cons 2 '())))
           (cons 'f (cons 1 (cons b '()))))
    (println (deref-lvar a))                ; => 1
    (println (deref-lvar b))                ; => 2

    ;; Variable chains
    (define v1 (logic-var))
    (define v2 (logic-var))
    (unify v1 v2)    ; v1 and v2 share the same binding
    (unify v2 99)
    (println (deref-lvar v1))               ; => 99

    ;; 3. Backtracking with trail marks
    (define r    (logic-var))
    (define mark (trail-mark))
    (unify r 100)
    (println (deref-lvar r))                ; => 100
    (unwind-trail mark)
    (println (logic-var? (deref-lvar r)))   ; => #t  (unbound again)

    ;; 4. Occurs check -- prevents cyclic terms
    (define z   (logic-var))
    (define z-m (trail-mark))
    (println (unify z (cons z '())))        ; => #f  (would be infinite)
    (unwind-trail z-m)

    ;; 5. Fact database queries
    (define parent-db
      '((tom bob) (tom liz) (bob ann) (bob pat) (pat jim)))

    (defun solve-parent (pvar cvar db)
      (if (null? db) '()
          (let* ((fact (car db))
                 (m    (trail-mark))
                 (ok   (and (unify pvar (car fact))
                            (unify cvar (cadr fact)))))
            (if ok
                (let* ((sol  (cons (deref-lvar pvar) (deref-lvar cvar)))
                       (rest (begin (unwind-trail m)
                                    (solve-parent pvar cvar (cdr db)))))
                  (cons sol rest))
                (begin (unwind-trail m)
                       (solve-parent pvar cvar (cdr db)))))))

    ;; Children of tom
    (let* ((pv (logic-var)) (cv (logic-var))
           (_ (unify pv 'tom))
           (sols (solve-parent pv cv parent-db)))
      (println (map* cdr sols)))            ; => (bob liz)

    ;; 6. std.logic higher-level API
    ;; findall -- collect all solutions
    (let* ((x    (logic-var))
           (sols (findall
                   (lambda () (deref-lvar x))
                   (map* (lambda (v) (lambda () (== x v)))
                         '(a b c)))))
      (println sols))                       ; => (a b c)

    ;; run1 -- first solution
    (let* ((x   (logic-var))
           (sol (run1
                  (lambda () (deref-lvar x))
                  (map* (lambda (v) (lambda () (and (== x v) (> v 3))))
                        '(1 2 3 4 5)))))
      (println sol))                        ; => 4

    ;; succeeds? -- non-destructive probe
    (let* ((sv (logic-var))
           (ok (succeeds? (lambda () (== sv 77)))))
      (println ok)                          ; => #t
      (println (logic-var? sv)))            ; => #t  (binding not kept)
  ))
```

---

## Relational Logic Programming

`cookbook/logic/logic.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/logic/logic.eta)

Wraps the fact database as a reusable *relation function* that returns a list
of goal branches. The same relation works in all query directions, mirroring
miniKanren / Prolog.

```scheme
(module logic
  (import std.io std.core std.collections std.logic)
  (begin

    (define parent-db
      '((tom bob) (tom liz) (bob ann) (bob pat) (pat jim)))

    ;; parento -- the core relation.
    ;; Each branch is a thunk; findall unwinds the trail between them.
    ;; Either argument can be a free logic variable (wildcard) or a
    ;; concrete value (filter).
    (defun parento (p c)
      (map* (lambda (fact)
               (lambda ()
                 (and (== p (car fact))
                      (== c (cadr fact)))))
             parent-db))

    ;; Forward: children of tom
    (let* ((cv (logic-var))
           (sols (findall (lambda () (deref-lvar cv))
                          (parento 'tom cv))))
      (println sols))                          ; => (bob liz)

    ;; Backward: parents of ann
    (let* ((pv (logic-var))
           (sols (findall (lambda () (deref-lvar pv))
                          (parento pv 'ann))))
      (println sols))                          ; => (bob)

    ;; Unconstrained: all parent->child pairs
    (let* ((pv (logic-var)) (cv (logic-var))
           (sols (findall (lambda () (cons (deref-lvar pv) (deref-lvar cv)))
                          (parento pv cv))))
      (println sols))
    ;; => ((tom . bob) (tom . liz) (bob . ann) (bob . pat) (pat . jim))

    ;; grandparento -- composing two relations
    ;; Prolog: grandparent(GP, GC) :- parent(GP, Mid), parent(Mid, GC).
    (defun grandparento (gp gc)
      (let* ((mid  (logic-var))
             (mids (findall (lambda () (deref-lvar mid))
                            (parento gp mid))))
        (flatten
          (map* (lambda (mid-val) (parento mid-val gc))
                mids))))

    ;; Grandchildren of tom
    (let* ((gc (logic-var))
           (sols (findall (lambda () (deref-lvar gc))
                          (grandparento 'tom gc))))
      (println sols))                          ; => (ann pat)
  ))
```

---

## N-Queens

`cookbook/logic/nqueens.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/logic/nqueens.eta)

The classic N-queens problem solved with CLP(FD) finite-domain constraints.
Each queen's column is a logic variable with domain `[1..N]`; `all-different`
and diagonal constraints are posted; labelling uses first-fail strategy.

```scheme
(module nqueens
  (import std.core std.collections std.logic std.clp)
  (begin

    ;; Build N fresh logic variables with domain [lo..hi]
    (defun make-fd-vars (n lo hi)
      (let loop ((i 0) (acc '()))
        (if (>= i n) (reverse acc)
            (let ((v (logic-var)))
              (clp:domain v lo hi)
              (loop (+ i 1) (cons v acc))))))

    ;; Post qi + offset = ai (fresh auxiliary variable)
    (defun post-offsets (qs offs)
      (let loop ((qs qs) (offs offs) (acc '()))
        (cond
          ((null? qs) (reverse acc))
          (else
            (let ((a (logic-var)))
              (clp:domain a -1000 1000)
              (clp:plus-offset a (car qs) (car offs))
              (loop (cdr qs) (cdr offs) (cons a acc)))))))

    ;; Solve N-queens, return column assignment or #f
    (defun solve-nqueens (n)
      (let* ((qs    (make-fd-vars n 1 n))
             (rows  (let loop ((i 1) (acc '()))
                      (if (> i n) (reverse acc)
                          (loop (+ i 1) (cons i acc)))))
             (nrows (map (lambda (i) (- 0 i)) rows))
             (as    (post-offsets qs rows))    ; Qi + i  (anti-diagonal)
             (bs    (post-offsets qs nrows)))  ; Qi - i  (main diagonal)
        (clp:all-different qs)
        (clp:all-different as)
        (clp:all-different bs)
        (if (clp:labeling qs 'strategy 'ff)
            (map (lambda (v) (deref-lvar v)) qs)
            #f)))

    ;; Pretty-print board
    (defun print-board (cols)
      (let ((n (length cols)))
        (for-each
          (lambda (c)
            (let loop ((i 1))
              (cond
                ((> i n) (newline))
                ((= i c) (display "Q ") (loop (+ i 1)))
                (else    (display ". ") (loop (+ i 1))))))
          cols)))

    (let ((sol (solve-nqueens 8)))
      (display "8-queens: ") (display sol) (newline)
      (print-board sol))
  ))
```

---

## SEND + MORE = MONEY

`cookbook/logic/send-more-money.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/logic/send-more-money.eta)

The classic cryptarithmetic puzzle solved with CLP(FD). Each letter is a
distinct digit `[0..9]` with leading digits non-zero. A single
`clp:scalar-product` encodes the column-wise arithmetic constraint.
Unique solution: 9567 + 1085 = 10652.

```scheme
(module send-more-money
  (import std.core std.collections std.logic std.clp)
  (begin

    (defun solve ()
      (let ((s (logic-var)) (e (logic-var)) (n (logic-var)) (d (logic-var))
            (m (logic-var)) (o (logic-var)) (r (logic-var)) (y (logic-var)))
        ;; Leading digits non-zero
        (clp:domain s 1 9)
        (clp:domain m 1 9)
        ;; Remaining letters
        (clp:domain e 0 9) (clp:domain n 0 9) (clp:domain d 0 9)
        (clp:domain o 0 9) (clp:domain r 0 9) (clp:domain y 0 9)
        ;; All eight digits distinct
        (clp:all-different (list s e n d m o r y))
        ;; SEND + MORE - MONEY = 0 linearised:
        ;;   S:+1000  E:+91  N:-90  D:+1  M:-9000  O:-900  R:+10  Y:-1
        (clp:scalar-product
          '(1000 91 -90 1 -9000 -900 10 -1)
          (list s e n d m o r y)
          0)
        (if (clp:labeling (list s e n d m o r y) 'strategy 'ff)
            (list (cons 'S (deref-lvar s)) (cons 'E (deref-lvar e))
                  (cons 'N (deref-lvar n)) (cons 'D (deref-lvar d))
                  (cons 'M (deref-lvar m)) (cons 'O (deref-lvar o))
                  (cons 'R (deref-lvar r)) (cons 'Y (deref-lvar y)))
            #f)))

    (let ((sol (solve)))
      (for-each
        (lambda (kv)
          (display (car kv)) (display " = ") (display (cdr kv)) (newline))
        sol))
    ;; S = 9, E = 5, N = 6, D = 7, M = 1, O = 0, R = 8, Y = 2
  ))
```

---

## Boolean Simplifier

`cookbook/logic/boolean-simplifier.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/logic/boolean-simplifier.eta)

Symbolic Boolean expression simplifier using recursive tree rewriting.
Applies De Morgan's laws, double-negation elimination, identity laws, and
idempotency. A fixed-point wrapper runs until the expression stabilises.

```scheme
(module boolean-simplifier
  (import std.io)
  (begin

    (defun non-pair? (x) (not (pair? x)))
    (defun op (e) (car e))
    (defun a1 (e) (car (cdr e)))
    (defun a2 (e) (car (cdr (cdr e))))

    (defun simplify-bool (e)
      (cond
        ((non-pair? e) e)

        ;; not
        ((eq? (op e) 'not)
          (let ((u (simplify-bool (a1 e))))
            (cond
              ((eq? u #t) #f)
              ((eq? u #f) #t)
              ;; Double negation: (not (not x)) = x
              ((and (pair? u) (eq? (op u) 'not)) (a1 u))
              ;; De Morgan: (not (and a b)) = (or (not a) (not b))
              ((and (pair? u) (eq? (op u) 'and))
                (list 'or  (list 'not (a1 u)) (list 'not (a2 u))))
              ;; De Morgan: (not (or a b)) = (and (not a) (not b))
              ((and (pair? u) (eq? (op u) 'or))
                (list 'and (list 'not (a1 u)) (list 'not (a2 u))))
              (#t (list 'not u)))))

        ;; and
        ((eq? (op e) 'and)
          (let ((sa (simplify-bool (a1 e)))
                (sb (simplify-bool (a2 e))))
            (cond
              ((eq? sa #f) #f)      ; x AND F = F
              ((eq? sb #f) #f)      ; F AND x = F
              ((eq? sa #t) sb)      ; x AND T = x
              ((eq? sb #t) sa)      ; T AND x = x
              ((equal? sa sb) sa)   ; x AND x = x
              (#t (list 'and sa sb)))))

        ;; or
        ((eq? (op e) 'or)
          (let ((sa (simplify-bool (a1 e)))
                (sb (simplify-bool (a2 e))))
            (cond
              ((eq? sa #t) #t)      ; x OR T = T
              ((eq? sb #t) #t)      ; T OR x = T
              ((eq? sa #f) sb)      ; x OR F = x
              ((eq? sb #f) sa)      ; F OR x = x
              ((equal? sa sb) sa)   ; x OR x = x
              (#t (list 'or sa sb)))))

        (#t e)))

    ;; Fixed-point: keep simplifying until stable
    (defun simplify-bool* (e)
      (let ((s (simplify-bool e)))
        (if (equal? s e) s (simplify-bool* s))))

    ;; Examples
    (println (simplify-bool* '(and x #t)))            ; => x
    (println (simplify-bool* '(and x #f)))            ; => #f
    (println (simplify-bool* '(or (and #t x) #f)))    ; => x
    (println (simplify-bool* '(not (not y))))          ; => y
    (println (simplify-bool* '(not (and a b))))        ; => (or (not a) (not b))
    (println (simplify-bool* '(not (or a b))))         ; => (and (not a) (not b))
    (println (simplify-bool* '(or x x)))               ; => x
  ))
```

---

## Symbolic Differentiation

`cookbook/logic/symbolic-diff.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/logic/symbolic-diff.eta)

Symbolic differentiation and algebraic simplification of expression trees.
Supports: constants, variables, `+`, `*`, `^` (power), `sin`, `cos`, `exp`,
`log`. A fixed-point simplifier cleans up the result.

```scheme
(module symbolic-diff
  (import std.io)
  (begin

    (defun non-pair? (x) (not (pair? x)))
    (defun op (e) (car e))
    (defun a1 (e) (car (cdr e)))
    (defun a2 (e) (car (cdr (cdr e))))

    ;; Algebraic simplifier
    (defun simplify (e)
      (cond
        ((non-pair? e) e)
        ((eq? (op e) '+)
          (let ((sa (simplify (a1 e))) (sb (simplify (a2 e))))
            (cond
              ((and (number? sa) (= sa 0)) sb)
              ((and (number? sb) (= sb 0)) sa)
              ((and (number? sa) (number? sb)) (+ sa sb))
              ((equal? sa sb) (list '* 2 sa))
              (#t (list '+ sa sb)))))
        ((eq? (op e) '*)
          (let ((sa (simplify (a1 e))) (sb (simplify (a2 e))))
            (cond
              ((or (and (number? sa) (= sa 0))
                   (and (number? sb) (= sb 0))) 0)
              ((and (number? sa) (= sa 1)) sb)
              ((and (number? sb) (= sb 1)) sa)
              ((and (number? sa) (number? sb)) (* sa sb))
              (#t (list '* sa sb)))))
        (#t e)))

    (defun simplify* (e)
      (let ((s (simplify e)))
        (if (equal? s e) s (simplify* s))))

    ;; Symbolic differentiation rules
    (defun diff (e v)
      (cond
        ((number? e) 0)
        ((symbol? e) (if (eq? e v) 1 0))
        ;; Sum rule: d(a+b) = da + db
        ((eq? (op e) '+)
          (list '+ (diff (a1 e) v) (diff (a2 e) v)))
        ;; Product rule: d(a*b) = da*b + a*db
        ((eq? (op e) '*)
          (list '+ (list '* (diff (a1 e) v) (a2 e))
                   (list '* (a1 e) (diff (a2 e) v))))
        ;; Power rule: d(u^n) = n*u^(n-1)*du
        ((eq? (op e) '^)
          (let ((u (a1 e)) (n (a2 e)))
            (if (number? n)
                (list '* n (list '* (list '^ u (- n 1)) (diff u v)))
                0)))
        ;; Chain rules
        ((eq? (op e) 'sin)
          (list '* (list 'cos (a1 e)) (diff (a1 e) v)))
        ((eq? (op e) 'cos)
          (list '* -1 (list '* (list 'sin (a1 e)) (diff (a1 e) v))))
        ((eq? (op e) 'exp)
          (list '* (list 'exp (a1 e)) (diff (a1 e) v)))
        ((eq? (op e) 'log)
          (list '* (list '/ 1 (a1 e)) (diff (a1 e) v)))
        (#t 0)))

    ;; Differentiate and simplify
    (defun D (expr var) (simplify* (diff expr var)))

    ;; Examples
    (println (D '(+ (* x x) 3) 'x))     ; => (* 2 x)
    (println (D '(* x (+ x 3)) 'x))     ; => (+ (+ x 3) x)
    (println (D '(sin (* x x)) 'x))      ; => (* (cos (* x x)) (* 2 x))
    (println (D '(^ (+ x 1) 3) 'x))     ; => (* 3 (^ (+ x 1) 2))
    (println (D '(exp (* 2 x)) 'x))      ; => (* (exp (* 2 x)) 2)
    (println (D '(log x) 'x))            ; => (/ 1 x)
  ))
```

