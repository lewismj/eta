"""
Basic unit tests for the environment class.
"""
import unittest
from eta.types import Environment, Symbol, Expression, EtaError


class EnvironmentTest(unittest.TestCase):
    def setUp(self):
        # Creating some dummy bindings, note these may not be valid Lisp expressions.
        # Used to test add_binding and lookup functions in the Environment.
        self.global_env = Environment()
        self.global_env.add_binding(Symbol('x'), True)
        self.global_env.add_binding(Symbol('y'), False)
        self.local_env = Environment()

        expr = Expression([Symbol('+'), 1, 2])
        self.local_env.add_binding(Symbol('ex'), expr)
        self.local_env.outer = self.global_env

        # create a symbol with the same name as an inner frame.
        expr2 = Expression([Symbol('+'), 2, 2])
        self.global_env.add_binding(Symbol('ex'), expr2)

    def test_local_lookup(self):
        self.assertEqual(self.local_env.lookup_binding('ex'), Expression(['+', 1, 2]))

    def test_lookup_uses_outer_scope(self):
        self.assertEqual(True, self.global_env.lookup_binding('x'))
        self.assertEqual(False, self.global_env.lookup_binding('y'))

    def test_lookup_global_scope(self):
        self.assertEqual(True, self.global_env.lookup_binding('x'))
        self.assertEqual(False, self.global_env.lookup_binding('y'))

    def test_unbound_symbol(self):
        val = self.local_env.lookup_binding('z')
        self.assertIsInstance(val, EtaError)


def make_suite():
    return unittest.makeSuite(EnvironmentTest, 'Environment test')


if __name__ == '__main__':
    suite = make_suite()
    runner = unittest.TextTestRunner()
    runner.run(suite)
