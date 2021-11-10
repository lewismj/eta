"""
Test interpreter, should load minimal prelude.
"""

import unittest
from eta.interpreter import Interpreter


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


def make_suite():
    return unittest.makeSuite(InterpreterTest, 'Interpreter test')


if __name__ == '__main__':
    suite = make_suite()
    runner = unittest.TextTestRunner()
    runner.run(suite)
