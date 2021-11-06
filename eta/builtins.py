"""
builtins
The purpose of builtins is to provide a set of functions to the environment,
such that a standard library (prelude) can be loaded. The language functionality
should mostly be defined by the prelude (i.e. the language itself).
Note, use term head, tail etc. rather than car, cdr. cons expressions are simply
lists.
"""

from eta.types import Expression, LispError, Environment, Symbol
from functools import reduce
import operator


# Basic numeric operator are all reduce functions, since we allow
# expressions of the form  (+ 1 2 3 4 5) and can calculate them
# directly, rather than a sequence of operator.add(a,b)

def builtin_reduce(op, exp):
    try:
        return reduce(op, exp)
    except Exception as ex:
        return LispError(ex)

# All builtin functions should have arguments 'env', 'eval'.


def builtin_add(env, expr):
    return builtin_reduce(operator.add, expr)


def builtin_sub(env, expr):
    return builtin_reduce(operator.sub, expr)


def builtin_mul(env, expr):
    return builtin_reduce(operator.mul, expr)


def builtin_div(env, expr):
    return builtin_reduce(operator.truediv, expr)


def add_builtins(env):
    env.add_binding(Symbol('+'), builtin_add)
    env.add_binding(Symbol('-'), builtin_sub)
    env.add_binding(Symbol('*'), builtin_mul)
    env.add_binding(Symbol('/'), builtin_div)
