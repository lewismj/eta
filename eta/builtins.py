"""
builtins
The purpose of builtins is to provide a set of functions to the environment,
such that a standard library (prelude) can be loaded. The language functionality
should mostly be defined by the prelude (i.e. the language itself).
Note, use term head, tail etc. rather than car, cdr. cons expressions are simply
lists.
"""

from eta.types import LispError, Symbol, Expression
from functools import reduce
import operator


# Notes on builtins:
#   Builtins should generally be as few functions as possible:
#       1. The language itself should be defined by itself (or the core builtins and usable
#          language defined by a prelude in the language itself.
#       2. We could just implement SKI :) But the builtins should be a practical minima of
#          functions to enable a prelude.
#   As an example, many simple math functions could be defined by a prelude; however it
#   make sense to build-in (since they are available) common ones, thus avoid interpreter
#   cost.


def builtin_reduce(op, exp):
    """
    Basic numeric expressions are of the form (+ values... ) so we always reduce.
    Rather than treat, e.g. '+' as a repeated operation of operator.add functions.
    """
    try:
        return reduce(op, exp)
    except Exception as ex:
        return LispError(ex)


def unary_function(exp, func):
    """

    """
    try:
        return func(exp)
    except Exception as ex:
        return LispError(ex)


def binary_function(env, expr, function):
    """

    """
    if len(expr) == 2:
        return function(expr[0], expr[1])
    else:
        return LispError("Comparison defining {} arguments."
                         " Expected two arguments".format(len(expr)))


def equality(env, expr):
    pass


def cons(env, expr):
    if isinstance(expr, Expression):
        expr.quote()
    else:
        value = expr
        expression = Expression([value])
        expression.quote()
        return expression


def head(env, expr):
    if len(expr) == 1:
        value = expr[0][0]
        expression = Expression([value])
        expression.quote()
        return expression
    else:
        return LispError("Function passed {} arguments. "
                         " head function expects a single list as an argument.".format(len(expr)))


def tail(env, expr):
    if len(expr) == 1:
        _, *tl = expr[0]
        tail_expr = Expression(tl)
        tail_expr.quote()
        return tail_expr
    else:
        return LispError("Function passed {} arguments. "
                         " tail function expects a single list as an argument.".format(len(expr)))


def join(env, expr):
    if len(expr) <= 1:
        return expr
    else:
        expression = Expression([])
        expression.quote()
        for sub_expr in expr:
            if isinstance(sub_expr, Expression):
                for item in sub_expr:
                    # may a copy
                    tmp = item
                    expression.append(tmp)
            else:
                expression.append(sub_expr)
        return expression

# why define individual functions, rather than use (lambda ev, ex: builtin_reduce ... ) ?
# we lose the function name, when tracing the evaluation, which can be useful for debugging,
# within a repl environment.


def add(env, ex):
    return builtin_reduce(operator.add, ex)


def add_builtins(env):
    import math
    env.add_binding(Symbol('+'), add)
    env.add_binding(Symbol('-'), lambda ev, ex: builtin_reduce(operator.sub, ex))
    env.add_binding(Symbol('*'), lambda ev, ex: builtin_reduce(operator.mul, ex))
    env.add_binding(Symbol('/'), lambda ev, ex: builtin_reduce(operator.truediv, ex))
    env.add_binding(Symbol("error"), lambda ev, ex: LispError("Error: {}".format(str(ex))))
    env.add_binding(Symbol("max"), lambda ev, ex: unary_function(ex, max))
    env.add_binding(Symbol("min"), lambda ev, ex: unary_function(ex, min))
    env.add_binding(Symbol("_pi"), math.pi)
    env.add_binding(Symbol("_tau"), math.tau)
    env.add_binding(Symbol("_e"), math.e)
    env.add_binding(Symbol("<"), lambda ev, ex: binary_function(ev, ex, operator.lt))
    env.add_binding(Symbol(">"), lambda ev, ex: binary_function(ev, ex, operator.gt))
    env.add_binding(Symbol(">="), lambda ev, ex: binary_function(ev, ex, operator.ge))
    env.add_binding(Symbol("<="), lambda ev, ex: binary_function(ev, ex, operator.le))
    env.add_binding(Symbol("=="), equality)
    env.add_binding(Symbol("head"), head)
    env.add_binding(Symbol("tail"), tail)
    env.add_binding(Symbol("join"), join)
    env.add_binding(Symbol("list"), cons)
