"""
Basic evaluator tests.
"""

import unittest
from eta.parser import parser
from eta.eval import evaluate
from eta.builtins import add_builtins
from eta.types import Environment


class EvaluatorTest(unittest.TestCase):

    def setUp(self):
        self.env = Environment([])
        add_builtins(self.env)

    def test_basic_numeric_expr(self):
        ast = parser.parse("(+ (* 3 (+ (* 2 4) (+ 3 5))) (+ (- 10 7) 6))")
        self.assertEqual(evaluate(ast, self.env), 57)
        ast = parser.parse("(+ 21 35 12 7)")
        self.assertEqual(evaluate(ast, self.env), 75)
        ast = parser.parse("(* 25 4 12)")
        self.assertEqual(evaluate(ast, self.env), 1200)
        ast = parser.parse("(+ 2.7 10)")
        self.assertEqual(evaluate(ast, self.env), 12.7)
        ast = parser.parse("(486)")
        self.assertEqual(evaluate(ast, self.env), 486)

    def test_quoted_return_self(self):
        pass

    def test_negative_number(self):
        ast = parser.parse("(+ 1 2)")
        self.assertEqual(3, evaluate(ast, self.env))


def make_suite():
    return unittest.makeSuite(EvaluatorTest, 'testEvaluator')


if __name__ == '__main__':
    suite = make_suite()
    runner = unittest.TextTestRunner()
    runner.run(suite)
