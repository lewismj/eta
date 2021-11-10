## Eta

### Summary
This project implements a simple Lisp interpreter using Python.
A subset of the language is implemented. 

- Higher order, partially applied & Lambda functions are supported and a simple prelude is included.
- An Interpreter can be used within Python packages.
- A ‘REPL’ is provided that supports vi or emacs edit mode, multi-line editing, history (tab completion) and tracing function calls. 
	- The ‘show’ command can be used to display the functions defined in your environment.
	- Meta-Enter (vi mode Esc-Enter) is used to evaluate in the REPL, as 
		we have multiline support.

A minimal core of built-in functions is used to provide a basis from
which the language can be defined via a Lisp like prelude. 

In essence, the interpreter should be a λ-calculus engine with the functionality of the language largely implemented by the prelude.

#### Todo
1. Use some trampoline package (if available) to optimise tail calls in the ‘eval’ functions.
2. Support variable arguments to functions (should be simple, i.e. allow `x & xs` syntax and bind the `xs` to the list of arguments).
3. Add support for macro expansion. For some efficiency, the common expansion `defun (fun x y) (body) -> define (fun) (lambda (x y) (body)` and others are supported by a ‘special form’ (via Parser rules).
4. Arguments are evaluated using ‘map’ in the ‘eval’ function, this could be parallelised.

#### REPL

- Control-D, to exit the REPL.
- Esc-Enter, to run command(s).
- F-1 to toggle edit mode (vi/emacs)
- F-2 to toggle trace mode.
- Up/Down arrow keys to search through history.
- Tab for builtin function name completion.
- 'show' to disable the content of the environment.

![](https://github.com/lewismj/eta/blob/main/docs/resources/repl.3.png)

#### Examples
```lisp
eta> defun (add x y) (+ x y)
()
eta>; to show the definition of the function, enter its name.
eta> add
.....
(λ (x y) (+ x y))
eta> ; This shows that a function is just a named Lambda. 
eta> add 2 3
.....
5
eta>
.....; partial function application.
.....define (add1) (add 1)
()
eta> add1 10
.....
11
eta> ; can also define Lambda functions,
eta> (lambda (x y) (* x y)) 10 20
.....
200
eta>; toggle tracing on (F2)
eta> (+ (* 3 (+ (* 2 4) (+ 3 5))) (+ (- 10 7) 6))
.....
((+ (* 3 (+ (* 2 4) (+ 3 5))) (+ (- 10 7) 6)))
(+ (* 3 (+ (* 2 4) (+ 3 5))) (+ (- 10 7) 6))
(* 3 (+ (* 2 4) (+ 3 5)))
(+ (* 2 4) (+ 3 5))
(* 2 4)
builtin:multiply [2, 4]
(+ 3 5)
builtin:add [3, 5]
builtin:add [8, 8]
builtin:multiply [3, 16]
(+ (- 10 7) 6)
(- 10 7)
builtin:subtract [10, 7]
builtin:add [3, 6]
builtin:add [48, 9]
57
eta>
```

#### Prelude

```lisp
; Eta prelude

; negation.
(defun (not x)
    (if (== #t x)
        (#f)
        (#t)
    ))

; list functions.
(define (nil) '())

(defun (empty xs) (if (== xs nil) (#t) (#f)))

; length of list.
(defun (len xs)
  (if (== xs nil) 
    (0) 
    (+ 1 (len (tail xs)))
  )) 

; first element of a list.
(defun (fst xs) ( eval (head xs) ))

; drop n-elements from a list.
(defun (drop n xs)
    (if (== n 0)
        (xs)
        (drop (- n 1) (tail xs))
     ))

; fold a list.
(defun (foldl f z xs)
    (if (== xs nil)
        (z)
        (foldl f (f z (fst xs)) (tail xs))
    ))

; map over the elements of a list.
(defun (map f xs)
    ((if (== xs nil)
        (nil)
        (join (list (f (fst xs))) (map f (tail xs))))
    ))

; filter elements of list.
(defun (filter f xs)
        (if (== xs nil)
            (nil)
            (join (if (f (fst xs)) (head xs) (nil)) (filter f (tail xs)))
        ))

; quicksort
(defun (sort xs)
    (if (<= (len xs) 1)
        (xs)
        (
            ( let (pivot (fst xs)) )
            (join
                (sort (filter (lambda (n) (> pivot n)) xs))
                (pivot)
                (sort (tail (filter (lambda (n) (<= pivot n)) xs)))
               )
          )
      ))

; math constants
(define (_pi) (3.141592653589793))
(define (_tau) (6.283185307179586))
(define (_e) (2.718281828459045))

(defun (odd n) (if (== 0 (% n 2)) (#f) (#t)))
(defun (even n) (not (odd n)))
```

#### Prelude Examples

```lisp
eta> load "./eta/prelude/prelude.lsp"
.....
.....
['./eta/prelude/prelude.lsp']
[]
eta> define (xs) '(99 98 -2 -4 2 1 12 0)
.....
()
eta> tail xs
'(98 -2 -4 2 1 12 0)
eta> head xs
'(99)
eta> ; Show the definition of the function.
eta> sort
.....
(λ (xs) (if ((<= (len xs) 1) (xs) (((define (pivot) ((fst xs))) (join (sort (filter (λ (n) (> pivot n)) xs)) (pivot) (sort (tail (filter (λ (n) (<= pivot n)) xs))))))
eta> sort xs
.....
'(-4 -2 0 1 2 12 98 99)
eta> ; to Py list.
.....eval (sort xs)
.....
[-4, -2, 0, 1, 2, 12, 98, 99]
eta>
```

#### Testing
See the ’t’ directory for tests. For example, testing that we can correctly encode the Church Booleans:

```lisp
eta> ; Note, and/or are provided as special forms.
.....; Name these 'and0', 'or0' to avoid name clash.
.....(defun (toBoolean f) (f (#t) (#f)))
.....(defun (true x y) (x))
.....(defun (false x y) (y))
.....(defun (and0 p q) ((p q) p))
.....(defun (or0 p q) ((p p) q))
.....
.....; Identity combinator
.....(defun (I f) (f))
.....(define (c) (1))
.....
.....; Test the Identity function
.....(I(I(I(I(I(I(I(c))))))))
.....
.....; Test we can correctly encode Church Booleans.
.....(
.....    (toBoolean (or0 true true))
.....    (toBoolean (or0 true false))
.....    (toBoolean (or0 false true))
.....    (toBoolean (or0 false false))
.....    )
.....
.....(
.....    (toBoolean (and0 true true))
.....    (toBoolean (and0 true false))
.....    (toBoolean (and0 false true))
.....    (toBoolean (and0 false false))
.....)
[1, [True, True, True, False], [True, False, False, False]]
eta>
```
