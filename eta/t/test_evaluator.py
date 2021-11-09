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
        self.assertEqual(57, evaluate(ast, self.env))
        ast = parser.parse("(+ 21 35 12 7)")
        self.assertEqual(75, evaluate(ast, self.env))
        ast = parser.parse("(* 25 4 12)")
        self.assertEqual(1200, evaluate(ast, self.env))
        ast = parser.parse("(+ 2.7 10)")
        self.assertEqual(12.7, evaluate(ast, self.env))
        ast = parser.parse("(486)")
        self.assertEqual(486, evaluate(ast, self.env))

    def test_quoted_expressions(self):
        ast = parser.parse("(tail '( 1 2 3 4))")
        self.assertEqual("'(2 3 4)", str(evaluate(ast, self.env)))
        ast = parser.parse("(head '(1 2 3))")
        self.assertEqual("'(1)", str(evaluate(ast, self.env)))
        ast = parser.parse("(list 1 2 3)")
        self.assertEqual("'([1, 2, 3])", str(evaluate(ast, self.env)))
        ast = parser.parse("(join '(1 2 3) '(4 5 6))")
        self.assertEqual("'(1 2 3 4 5 6)", str(evaluate(ast, self.env)))

    def test_negative_number(self):
        ast = parser.parse("(+ -1 -2)")
        self.assertEqual(-3, evaluate(ast, self.env))


def make_suite():
    return unittest.makeSuite(EvaluatorTest, 'Evaluator test')


if __name__ == '__main__':
    suite = make_suite()
    runner = unittest.TextTestRunner()
    runner.run(suite)
