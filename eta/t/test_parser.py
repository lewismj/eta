"""
Basic unit tests for the parser class.
"""
import unittest
from eta.parser import parser
from eta.types import Symbol
from lark.visitors import VisitError


class ParserTest(unittest.TestCase):

    def test_basic_expressions(self):
        try:
            parser.parse("(defun (foo x y) (+ x y))")
            parser.parse("(+ 21 35 12  7)")
            parser.parse("(/ 10 2)")
            parser.parse("(defun (len xs) "
                         "(if (== xs nil) "
                         "  (0) (+ 1 (len (tail xs)))))")
        except VisitError:
            self.fail("Failed to parse basic expressions.")

    def test_expression_structure_basic(self):
        ast = parser.parse("(+ 1 2)")
        self.assertEqual(1, len(ast))
        expression = ast[0]
        self.assertEqual(Symbol("+"), expression[0])
        self.assertEqual(1, expression[1])
        self.assertEqual(2, expression[2])

    def test_parsed_as_number(self):
        ast = parser.parse("(-1)")
        self.assertEqual(1, len(ast))


def make_suite():
    return unittest.makeSuite(ParserTest, 'Parser test')


if __name__ == '__main__':
    suite = make_suite()
    runner = unittest.TextTestRunner()
    runner.run(suite)
