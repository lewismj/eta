"""
Utility to provide an Interpreter that can be invoked within Python function.
A minimal prelude is defined.
"""

from eta.builtins import add_builtins
from eta.parser import parser
from eta.types import Environment, Symbol
from eta.eval import evaluate


prelude = """
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
"""


class Interpreter:
    """
    Simple wrapper that allows interpreter to be used within Python function.
    """
    def __init__(self):
        self.env = Environment()
        add_builtins(self.env)
        evaluate(parser.parse(prelude), self.env)

    def execute(self, text):
        try:
            ast = parser.parse("({})".format(text))
            result = [evaluate(exp, self.env) for exp in ast]
            if len(result) == 1:
                return result[0]
            else:
                return result
        except Exception as ex:
            return str(ex)

    # Experiment with function interface, so that wrapped Python function
    # can be added.

    def add_binding(self, name, function):
        self.env.add_binding(Symbol(name), function)

