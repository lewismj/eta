"""
Test that basic combinators are correct.
"""
import unittest


class CombinatorTest(unittest.TestCase):
    pass


def make_suite():
    return unittest.makeSuite(CombinatorTest, 'combinator tests')


if __name__ == '__main__':
    suite = make_suite()
    runner = unittest.TextTestRunner()
    runner.run(suite)