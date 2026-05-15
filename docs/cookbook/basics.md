---
title: "Cookbook — Basics"
---

# Basics

Core language features: values, bindings, functions, recursion, higher-order
programming, closures, composition, argument parsing, and exception handling.

**Run any example with:**
```bash
etai cookbook/basics/<file>.eta
```

---

## Contents

- [Hello, World!](#hello-world)
- [Language Fundamentals](#language-fundamentals)
- [Functions & Closures](#functions--closures)
- [Higher-Order Functions](#higher-order-functions)
- [Recursion](#recursion)
- [Composition & Currying](#composition--currying)
- [Argument Parsing](#argument-parsing)
- [Exception Handling](#exception-handling)

---

## Hello, World!

`cookbook/basics/hello.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/basics/hello.eta)

A minimal program that prints a greeting and demonstrates a simple recursive
factorial.

```scheme
(module hello
  (import std.io)
  (begin
    (println "Hello, world!")

    (defun factorial (n)
      (if (= n 0) 1
          (* n (factorial (- n 1)))))

    (println (factorial 20))))
```

---

## Language Fundamentals

`cookbook/basics/basics.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/basics/basics.eta)

Arithmetic, booleans, conditionals, variables, `let` / `let*`, strings, pairs,
lists, and record types.

```scheme
(module basics
  (import std.io)
  (begin

    ;; -- Arithmetic --
    (println (+ 1 2 3))       ; => 6
    (println (* 2 3 4))       ; => 24
    (println (- 10 3))        ; => 7

    ;; -- Booleans & Conditionals --
    (println (and #t #f))     ; => #f
    (println (or  #f #t))     ; => #t

    (println (if (> 3 2) "yes" "no"))  ; => "yes"

    (println (cond
      ((= 1 2) "nope")
      ((= 1 1) "match!")
      (#t      "default")))   ; => "match!"

    ;; -- Variables & Let --
    (define x 42)
    (println x)               ; => 42

    (let ((a 10) (b 20))
      (println (+ a b)))      ; => 30

    ;; let* -- sequential bindings
    (let* ((a 1)
           (b (+ a 1))
           (c (+ a b)))
      (println c))            ; => 4

    ;; -- Strings --
    (println (string-append "hello" ", " "world!"))
    (println (string-length "eta"))   ; => 3

    ;; -- Pairs & Lists --
    (define xs (list 1 2 3 4 5))
    (println xs)              ; => (1 2 3 4 5)
    (println (length xs))     ; => 5
    (println (append '(a b) '(c d)))  ; => (a b c d)

    ;; -- Records --
    (define-record-type <point>
      (make-point x y)
      point?
      (x point-x)
      (y point-y))

    (define p (make-point 3 4))
    (println (point? p))      ; => #t
    (println (point-x p))     ; => 3

    ;; Mutable field
    (define-record-type <counter>
      (make-counter value)
      counter?
      (value counter-value set-counter-value!))

    (define c (make-counter 0))
    (set-counter-value! c (+ (counter-value c) 1))
    (println (counter-value c))  ; => 1

    ;; -- Symbols & Quoting --
    (println 'hello)              ; => hello
    (println '(one two three))    ; => (one two three)
  ))
```

---

## Functions & Closures

`cookbook/basics/functions.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/basics/functions.eta)

Named functions, tail recursion, lambda expressions, closures, variadic
arguments, and mutual recursion via `letrec`.

```scheme
(module functions
  (import std.io std.math)
  (begin

    ;; Named function
    (defun sq (x) (* x x))
    (println (sq 7))              ; => 49

    ;; Recursive factorial
    (defun factorial (n)
      (if (= n 0) 1
          (* n (factorial (- n 1)))))
    (println (factorial 10))      ; => 3628800

    ;; Tail-recursive with accumulator
    (defun factorial-tr (n)
      (letrec ((go (lambda (n acc)
                     (if (= n 0) acc
                         (go (- n 1) (* acc n))))))
        (go n 1)))
    (println (factorial-tr 20))   ; => 2432902008176640000

    ;; Lambda
    (define double (lambda (x) (* 2 x)))
    (println (double 21))         ; => 42

    ;; Closure: captures its environment
    (defun make-adder (n)
      (lambda (x) (+ n x)))

    (define add5  (make-adder  5))
    (define add10 (make-adder 10))
    (println (add5  3))           ; => 8
    (println (add10 3))           ; => 13

    ;; Variadic (rest args)
    (defun sum-all (first . rest)
      (letrec ((go (lambda (xs acc)
                     (if (null? xs) acc
                         (go (cdr xs) (+ acc (car xs)))))))
        (go rest first)))
    (println (sum-all 1 2 3 4 5)) ; => 15

    ;; Mutual recursion
    (letrec
      ((is-even? (lambda (n)
         (if (= n 0) #t (is-odd? (- n 1)))))
       (is-odd? (lambda (n)
         (if (= n 0) #f (is-even? (- n 1))))))
      (println (is-even? 42))     ; => #t
      (println (is-odd?  13)))    ; => #t
  ))
```

---

## Higher-Order Functions

`cookbook/basics/higher-order.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/basics/higher-order.eta)

`map*`, `filter`, `foldl`, `foldr`, `reduce`, `any?`, `every?`, `count`,
`zip`, `take`, `drop`, `range`, `flatten`, and `sort`.

```scheme
(module higher-order
  (import std.core std.io std.collections std.math)
  (begin

    (define nums (list 1 2 3 4 5 6 7 8 9 10))

    ;; map* -- transform every element
    (println (map* square nums))
    ;; => (1 4 9 16 25 36 49 64 81 100)

    ;; filter -- keep matching elements
    (println (filter even? nums))         ; => (2 4 6 8 10)

    ;; foldl -- left fold (sum)
    (println (foldl + 0 nums))            ; => 55

    ;; foldr -- right fold (copy a list)
    (println (foldr cons '() '(a b c)))   ; => (a b c)

    ;; reduce -- fold without initial value
    (println (reduce max '(3 7 2 9 1)))   ; => 9

    ;; Combine: sum of squares of even numbers
    (define result
      (foldl + 0 (map* square (filter even? nums))))
    (println result)                      ; => 220

    ;; any? / every?
    (println (any? negative? '(1 -2 3)))  ; => #t
    (println (every? positive? '(1 2 3))) ; => #t

    ;; count
    (println (count even? nums))          ; => 5

    ;; zip
    (println (zip '(a b c) '(1 2 3)))
    ;; => ((a . 1) (b . 2) (c . 3))

    ;; take / drop
    (println (take 3 nums))               ; => (1 2 3)
    (println (drop 7 nums))               ; => (8 9 10)

    ;; range
    (println (range 1 6))                 ; => (1 2 3 4 5)

    ;; flatten
    (println (flatten '((1 2) (3 4) (5))))  ; => (1 2 3 4 5)

    ;; sort
    (println (sort < '(5 3 8 1 4 2 7 6))) ; => (1 2 3 4 5 6 7 8)
  ))
```

---

## Recursion

`cookbook/basics/recursion.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/basics/recursion.eta)

Classic recursive algorithms: Fibonacci (naive and tail-recursive), list
reversal, depth-first flatten, Ackermann function, and Tower of Hanoi.

```scheme
(module recursion
  (import std.io)
  (begin

    ;; Fibonacci (naive)
    (defun fib (n)
      (cond
        ((= n 0) 0)
        ((= n 1) 1)
        (#t (+ (fib (- n 1)) (fib (- n 2))))))
    (println (fib 10))             ; => 55

    ;; Fibonacci (tail-recursive)
    (defun fib-fast (n)
      (letrec ((go (lambda (i a b)
                     (if (= i 0) a
                         (go (- i 1) b (+ a b))))))
        (go n 0 1)))
    (println (fib-fast 30))        ; => 832040

    ;; List reversal
    (defun my-reverse (xs)
      (letrec ((go (lambda (xs acc)
                     (if (null? xs) acc
                         (go (cdr xs) (cons (car xs) acc))))))
        (go xs '())))
    (println (my-reverse '(1 2 3 4 5)))  ; => (5 4 3 2 1)

    ;; Deep flatten
    (defun deep-flatten (xs)
      (cond
        ((null? xs) '())
        ((pair? (car xs))
         (append (deep-flatten (car xs))
                 (deep-flatten (cdr xs))))
        (#t (cons (car xs) (deep-flatten (cdr xs))))))
    (println (deep-flatten '((1 (2 3)) (4) 5)))  ; => (1 2 3 4 5)

    ;; Ackermann function
    (defun ackermann (m n)
      (cond
        ((= m 0) (+ n 1))
        ((= n 0) (ackermann (- m 1) 1))
        (#t (ackermann (- m 1) (ackermann m (- n 1))))))
    (println (ackermann 3 4))      ; => 125

    ;; Tower of Hanoi
    (defun hanoi (n from to via)
      (when (> n 0)
        (hanoi (- n 1) from via to)
        (display "Move disk ") (display n)
        (display " from ") (display from)
        (display " to ") (display to) (newline)
        (hanoi (- n 1) via to from)))
    (hanoi 3 'A 'C 'B)
  ))
```

---

## Composition & Currying

`cookbook/basics/composition.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/basics/composition.eta)

`compose`, `flip`, `constantly`, manual currying, `negate`, and functional
data pipelines.

```scheme
(module composition
  (import std.core std.io std.collections std.math)
  (begin

    ;; compose: (f o g)(x) = f(g(x))
    (define inc-then-double
      (compose (lambda (x) (* 2 x))
               (lambda (x) (+ x 1))))
    (println (inc-then-double 5))     ; => 12  ((5+1)*2)

    ;; flip -- swap argument order
    (define rcons (flip cons))
    (println (rcons '(1 2 3) 0))      ; => (0 1 2 3)

    ;; constantly -- always returns the same value
    (define always-42 (constantly 42))
    (println (always-42))             ; => 42
    (println (always-42 'ignored))    ; => 42

    ;; Manual currying
    (defun add (a) (lambda (b) (+ a b)))
    (define inc   (add 1))
    (define add10 (add 10))
    (println (inc 99))                ; => 100
    (println (add10 32))              ; => 42

    ;; Curried multiply
    (defun mul (a) (lambda (b) (* a b)))
    (define double (mul 2))
    (define triple (mul 3))
    (println (map* double '(1 2 3 4 5)))   ; => (2 4 6 8 10)
    (println (map* triple '(1 2 3 4 5)))   ; => (3 6 9 12 15)

    ;; Compose curried functions
    (define double-then-inc (compose (add 1) (mul 2)))
    (println (map* double-then-inc '(1 2 3 4 5)))
    ;; => (3 5 7 9 11)

    ;; negate -- flip a predicate
    (define not-even? (negate even?))
    (println (filter not-even? (range 1 11)))
    ;; => (1 3 5 7 9)

    ;; Pipeline: first 5 odd squares from 1..20
    (define odds    (filter odd? (range 1 21)))
    (define squares (map* square odds))
    (define top5    (take 5 squares))
    (println top5)
    ;; => (1 9 25 49 81)
  ))
```

---

## Argument Parsing

`cookbook/basics/args.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/basics/args.eta)

Declarative command-line argument parsing via `std.args` with flags, strings,
integers, lists, custom parsers, and validators.

```scheme
(module args-example
  (import std.io std.args)
  (begin

    (define parse-level
      (lambda (raw)
        (let ((n (string->number raw)))
          (if (and (number? n) (integer? n))
              n
              (error "level must be an integer")))))

    (define level-in-range?
      (lambda (n) (and (>= n 0) (<= n 5))))

    ;; Spec: (name flags type default [validators])
    (define spec
      (list
        '(v (--verbose -v)  flag)
        '(o (--out -o)      string "report.txt")
        '(n (--num)         int    0)
        '(t (--tag)         list)
        (list 'level
              '(--level)
              'int
              1
              (list (cons 'parse    parse-level)
                    (cons 'validate level-in-range?)))))

    (defun run-demo ()
      (args:parse
        spec
        '("-v" "--out=demo.txt" "--num" "3"
          "--tag" "risk" "--tag" "stress"
          "--level=4"
          "--" "-z")))

    (defun summarize (r)
      (list
        (cons 'verbose     (args:get r 'v))
        (cons 'out         (args:get r 'o))
        (cons 'num         (args:get r 'n))
        (cons 'tag         (args:get r 't))
        (cons 'level       (args:get r 'level))
        (cons 'positional  (args:get r 'positional))))

    (println (summarize (run-demo)))))
```

---

## Exception Handling

`cookbook/basics/exceptions.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/basics/exceptions.eta)

Eta uses `(raise 'tag value)` and `(catch 'tag body)` for structured
non-local exits. A tag-less `(catch body)` catches any exception.

```scheme
(module exceptions
  (import std.io std.core)
  (begin

    ;; Basic catch / raise
    (println (catch 'err 42))                         ; => 42
    (println (catch 'err (raise 'err "oops!")))       ; => oops!

    ;; Catch-all (no tag)
    (let ((result (catch (raise 'anything "caught by catch-all"))))
      (println result))                               ; => caught by catch-all

    ;; Tag specificity
    (let ((result
           (catch 'outer-err
             (+ 10 (catch 'inner-err
                     (raise 'inner-err 5))))))
      (println result))                               ; => 15

    ;; Propagation through calls
    (defun safe-divide (a b)
      (if (= b 0)
          (raise 'division-by-zero 'undefined)
          (/ a b)))

    (println (catch 'division-by-zero (safe-divide 10 2)))  ; => 5
    (println (catch 'division-by-zero (safe-divide 10 0)))  ; => undefined

    ;; Structured payloads
    (defun validate-age (age)
      (cond
        ((< age 0)   (raise 'validation-error (cons 'negative age)))
        ((> age 150) (raise 'validation-error (cons 'too-large age)))
        (#t          age)))

    (let ((err (catch 'validation-error (validate-age -5))))
      (println (car err))   ; => negative
      (println (cdr err)))  ; => -5

    ;; Early exit from search
    (defun first-negative (lst)
      (catch 'found
        (letrec ((loop (lambda (xs)
                         (cond
                           ((null? xs)     #f)
                           ((< (car xs) 0) (raise 'found (car xs)))
                           (#t             (loop (cdr xs)))))))
          (loop lst))))

    (println (first-negative '(3 1 4 1 5)))      ; => #f
    (println (first-negative '(3 1 -4 2 -1)))    ; => -4

    ;; Cleanup on exception using dynamic-wind
    (define cleanup-called #f)
    (catch 'resource-error
      (dynamic-wind
        (lambda () (set! cleanup-called #f))
        (lambda () (raise 'resource-error "boom"))
        (lambda () (set! cleanup-called #t))))
    (println cleanup-called)                     ; => #t
  ))
```

