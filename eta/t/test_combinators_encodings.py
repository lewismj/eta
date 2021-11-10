"""
Test that we can correctly encode the Church Booleans and some simple
combinators.
"""

import unittest
from eta.types import Environment
from eta.builtins import add_builtins
from eta.parser import parser
from eta.eval import evaluate


class CombinatorTest(unittest.TestCase):

    def setUp(self):
        self.env = Environment()
        add_builtins(self.env)
        church_booleans = """
        (defun (toBoolean f) (f (#t) (#f)))
        (defun (true x y) (x))
        (defun (false x y) (y))
        (defun (and0 p q) ((p q) p))
        (defun (or0 p q) ((p p) q))
        
        ; Identity combinator
        (defun (I f) (f))
        (define (c) (1))
        """
        evaluate(parser.parse(church_booleans), self.env)

    def test_church_or(self):
        """
        Test the Church encoding of booleans (or).
        """
        self.assertEqual(True, evaluate(parser.parse("(toBoolean (or0 true true))"), self.env))
        self.assertEqual(True, evaluate(parser.parse("(toBoolean (or0 true false))"), self.env))
        self.assertEqual(True, evaluate(parser.parse("(toBoolean (or0 false true))"), self.env))
        self.assertEqual(False, evaluate(parser.parse("(toBoolean (or0 false false))"), self.env))

    def test_church_and(self):
        """
        Test the Church encoding of booleans (and).
        """
        self.assertEqual(True, evaluate(parser.parse("(toBoolean (and0 true true))"), self.env))
        self.assertEqual(False, evaluate(parser.parse("(toBoolean (and0 true false))"), self.env))
        self.assertEqual(False, evaluate(parser.parse("(toBoolean (and0 false true))"), self.env))
        self.assertEqual(False, evaluate(parser.parse("(toBoolean (and0 false false))"), self.env))

    def test_identity(self):
        """
        Test the identity function.
        """
        self.assertEqual(1, evaluate(parser.parse("(I(I(I(I(I(I(I(c))))))))"), self.env))


def make_suite():
    return unittest.makeSuite(CombinatorTest, 'Combinator test')


if __name__ == '__main__':
    suite = make_suite()
    runner = unittest.TextTestRunner()
    runner.run(suite)
