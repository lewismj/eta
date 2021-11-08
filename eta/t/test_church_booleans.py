"""
Test that we can correct encode the Church Booleans.
"""

import unittest
from eta.types import Environment, Symbol, Expression, LispError
from eta.builtins import add_builtins
from eta.parser import parser
from eta.eval import evaluate


class ChurchBooleansTest(unittest.TestCase):

    def setUp(self):
        self.env = Environment()
        add_builtins(self.env)
        church_booleans = """
        (define (T) (#t))
        (define (F) (#f))
        (defun (toBoolean f) (f T F))
        (defun (true x y) (x))
        (defun (false x y) (y))
        (defun (and0 p q) ((p q) p))
        (defun (or0 p q) ((p p) q))
        """
        evaluate(parser.parse(church_booleans), self.env)

    def test_church_or(self):
        self.assertEqual(True, evaluate(parser.parse("(toBoolean (or0 true true))"), self.env))
        self.assertEqual(True, evaluate(parser.parse("(toBoolean (or0 true false))"), self.env))
        self.assertEqual(True, evaluate(parser.parse("(toBoolean (or0 false true))"), self.env))
        self.assertEqual(False, evaluate(parser.parse("(toBoolean (or0 false false))"), self.env))

    def test_church_and(self):
        self.assertEqual(True, evaluate(parser.parse("(toBoolean (and0 true true))"), self.env))
        self.assertEqual(False, evaluate(parser.parse("(toBoolean (and0 true false))"), self.env))
        self.assertEqual(False, evaluate(parser.parse("(toBoolean (and0 false true))"), self.env))
        self.assertEqual(False, evaluate(parser.parse("(toBoolean (and0 false false))"), self.env))


def make_suite():
    return unittest.makeSuite(ChurchBooleansTest, 'testEnvironment')


if __name__ == '__main__':
    suite = make_suite()
    runner = unittest.TextTestRunner()
    runner.run(suite)
