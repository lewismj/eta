"""
Test interpreter, should load minimal prelude.
"""

import unittest
from eta.interpreter import Interpreter


def my_function(a, b, c):
    return a*b*c


def my_wrapper(env, expr):
    return my_function(expr[0], expr[1], expr[2])


class InterpreterTest(unittest.TestCase):

    def setUp(self):
        self.interpreter = Interpreter()

    def test_sort(self):
        expression = """
            (define (xs) '(45 12 99 -45 2 17 1))
            ; sort will return a Lisp list, i.e. '( n1, n2, ... )
            ; eval will convert to a Python list. 
            eval (sort (xs)) 
        """
        result = self.interpreter.execute(expression)
        expected = [-45, 1, 2, 12, 17, 45, 99]
        self.assertEqual(expected, result)

    def test_function_add(self):
        self.interpreter.add_binding("add3", my_wrapper)
        expression = """
        ; The list implementation would just be: defun (add3 x y z) (+ x y z)
        ; This expands to define (add3) (lambda (x y z) (+ x y z)
        ; Wrapping up a function we add a builtin.
        
        add3 (+ 1 1) (+ 2 2) (+ 3 3)
        
        ; Calls the wrapped function with [2, 4, 6] as input
        """
        result = self.interpreter.execute(expression)
        expected = 48
        self.assertEqual(expected, result)


def make_suite():
    return unittest.makeSuite(InterpreterTest, 'Interpreter test')


if __name__ == '__main__':
    suite = make_suite()
    runner = unittest.TextTestRunner()
    runner.run(suite)
