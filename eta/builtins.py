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
    # reduce_unary_ops(exp)
    try:
        return reduce(op, exp)
    except Exception as ex:
        return LispError(ex)


def bin_function(func, exp):
    # reduce_unary_ops(exp)
    try:
        return func(exp)
    except Exception as ex:
        return LispError(ex)

# All builtin functions should have arguments 'env', 'eval'.


def add(env, expr):
    return builtin_reduce(operator.add, expr)


def sub(env, expr):
    return builtin_reduce(operator.sub, expr)


def mul(env, expr):
    return builtin_reduce(operator.mul, expr)


def div(env, expr):
    return builtin_reduce(operator.truediv, expr)


def error(env, expr):
    """
    Allow a function to return a LispError. There should never be 'exceptions' in the Lisp code.
    """
    return LispError("Error: {}".format(str(expr)))


def maximum(env, expr):
    return bin_function(max, expr)


def minimum(env, expr):
    return bin_function(min, expr)


def add_builtins(env):
    env.add_binding(Symbol('+'), add)
    env.add_binding(Symbol('-'), sub)
    env.add_binding(Symbol('*'), mul)
    env.add_binding(Symbol('/'), div)
    env.add_binding(Symbol("error"), error)
    env.add_binding(Symbol("max"), maximum)
    env.add_binding(Symbol("min"), minimum)
