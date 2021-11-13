## Eta

### Summary
This project implements a simple Lisp interpreter using Python.
A subset of the language is implemented. 

- Higher order, partially applied & Lambda functions are supported and a simple prelude (A ‘prelude’ is a very basic ‘standard library’ implemented via the core language builtin functions).

- An Interpreter can be used within Python packages. See the end of this page for an example on how to make a Python function callable from the ‘Lisp’ interpreter.

- A ‘REPL’ is provided that supports vi or emacs edit mode, multi-line editing, history (tab completion) and tracing function calls. 
	- The ‘show’ command can be used to display the functions defined in your environment.
	- Meta-Enter (vi mode Esc-Enter) is used to evaluate in the REPL, as 
		we have multiline support.

A minimal core of built-in functions is used to provide a basis from
which the language can be defined via a Lisp like prelude. 

In essence, the interpreter should be a λ-calculus engine with the functionality of the language largely implemented by the prelude.

#### Todo
1. Use a trampoline package to optimise tail calls in the ‘eval’ functions, e.g.  Consider standard definitions of functions within the prelude. Many functional algorithms can be tail recursive.

![](https://github.com/lewismj/eta/blob/main/docs/resources/repl.map.png)

2. Common expansions, e.g. `defun (fun x y) (body) -> define (fun) (lambda (x y) (body)`  are supported currently as ‘special forms’. That is, we have parse rules that will construct AST nodes that can be easily traversed by the ‘evaluation’. This saves some checking at run-time.  Generic macro expansion is something that could be added.

3. Arguments are evaluated using ‘map’ in the ‘eval’ function. This could be parallelised.

#### repl
- Control-D, to exit the REPL.
- Esc-Enter, to run command(s).
- F-1 to toggle edit mode (vi/emacs)
- F-2 to toggle trace mode.
- Up/Down arrow keys to search through history.
- Tab for builtin function name completion.
- 'show' to disable the content of the environment.

![](https://github.com/lewismj/eta/blob/main/docs/resources/repl.3.png)

To start the REPL and load the prelude:
```lisp
(eta) lewismj@waiheke eta % python -m eta.repl
eta> load "./eta/prelude/prelude.lsp"
.....
['./eta/prelude/prelude.lsp']
[]
eta>
```

#### Within Python
```python
from eta.interpreter import Interpreter

if __name__ == '__main__':
    interpreter = Interpreter()

    expression = """
    (define (xs) '(45 12 99 -45 2 17 1))
    ; sort will return a Lisp list, i.e. '( n1, n2, ... )
    ; eval will convert to a Python list.
    eval (sort (xs))
    """
    result = interpreter.execute(expression)
    print(result)
```

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
eta> ; Support for variable number of arguments via '&' syntax.
eta> defun (foo x & xs) (sort xs)
.....
()
eta> foo 1 3 2
.....
'(2 3)
eta> foo 1 3 2 4 -1 0 10
.....
'(-1 0 2 3 4 10)
eta> ; Can define local functions.
eta> defun (foo n) (
.....   (defun (y x) (+ 1 x))
.....   y n
.....   )
eta> foo 10
11
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

; apply a function to a variable list of arguments.
(defun (apply f & xs) (eval (map f xs)))

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
eta> ; Use 'apply' method to apply a function to variable number of arguments. Similar to map, but handles var args.
eta> apply (lambda (x) (+ x x)) 1 2 
[2, 4]
eta> apply (lambda (x) (+ x x)) 1 2 3 4 5
.....
[2, 4, 6, 8, 10]
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

#### Making existing Python functions callable from the ‘Lisp’ …

```python
def my_function(a, b, c):
    return a*b*c


# Some work on the eval/calling mechanism could probably remove
# the requirement to define a wrapper function.
def my_wrapper(env, expr):
    return my_function(expr[0], expr[1], expr[2])


if __name__ == '__main__':
    interpreter = Interpreter()
    interpreter.add_binding("fn3", my_wrapper)
    result = interpreter.execute("fn3 (+ 1 1) (+ 2 2) (+ 3 3)")
    print(result)

```

In this example, I use [Ray](https://www.ray.io) to define a wrapper function that will remotely execute the Python code when invoked by the Interpreter.

This is something that could be utilised within the project itself, to have remote execution as a switch (via the *execution context*). Allow function to be run remotely or locally.

```python
import ray
from eta.interpreter import Interpreter
from eta.types import Environment

@ray.remote
def my_function(p, q, r):
    return p*q*r 


# Some work on the eval/calling mechanism could probably remove
# the requirement to define a wrapper function.

# Or, switching wrapper should be a Python/AST function that
# re-writes the wrapper function (or calls the 'Lisp' interpreter to rebind the symbol).
def my_ray_wrapper(env, expr):
    future = my_function.remote(expr[0], expr[1], expr[2])
    return ray.get(future)


def my_other_wrapper(env, expr):
    return my_function(expr[0], expr[1], expr[2])



if __name__ == '__main__':
    interpreter = Interpreter()

    # Choose at runtime what wrapper to use....
    interpreter.add_binding("fn3", my_ray_wrapper)

    result = interpreter.execute("fn3 (+ 1 1) (+ 2 2) (+ 3 3)")
    print(result)
    result = interpreter.execute("fn3 1 2 3")
    print(result)
```
