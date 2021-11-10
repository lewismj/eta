"""
builtins
The purpose of builtins is to provide a set of functions to the environment,
such that a standard library (prelude) can be loaded. The language functionality
should mostly be defined by the prelude (i.e. the language itself).
Note, use term head, tail etc. rather than car, cdr. cons expressions are simply
lists.
"""
import math

import eta
from eta.types import EtaError, Symbol, Expression
from eta.eval import evaluate, find, empty_expr
from eta.parser import parser
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

# def first(items, pred): return next((i for i in items if pred(i)), None)


def builtin_reduce(op, expr):
    """
    Basic numeric expressions are of the form (+ values... ) so we always reduce.
    Rather than treat, e.g. '+' as a repeated operation of operator.add functions.
    """
    lisp_error = find(expr, lambda x: isinstance(x, EtaError))
    if lisp_error:
        return lisp_error

    try:
        return reduce(op, expr)
    except Exception as ex:
        return EtaError(ex)


def unary_function(expr, func):
    """

    """
    lisp_error = find(expr, lambda x: isinstance(x, EtaError))
    if lisp_error:
        return lisp_error

    try:
        return func(expr)
    except Exception as ex:
        return EtaError(ex)


def binary_function(env, expr, function):
    """

    """
    lisp_error = find(expr, lambda x: isinstance(x, EtaError))
    if lisp_error:
        return lisp_error

    if len(expr) == 2:
        return function(expr[0], expr[1])
    else:
        return EtaError("Comparison defining {} arguments."
                        " Expected two arguments".format(len(expr)))


def is_equal(e1, e2):
    if type(e1) == type(e2):
        return e1 == e2
    else:
        return False


def equals(env, expr):
    if len(expr) <= 1:
        return True
    else:
        first = expr[0]
        is_different = find(expr[1:], lambda x: not is_equal(first, x))
        if is_different is None:
            return True
        else:
            return False


def not_equals(env, expr):
    return not equals(env, expr)


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
        return EtaError("Function passed {} arguments. "
                        " head function expects a single list as an argument.".format(len(expr)))


def tail(env, expr):
    if len(expr) == 1:
        _, *tl = expr[0]
        tail_expr = Expression(tl)
        tail_expr.quote()
        return tail_expr
    else:
        return EtaError("Function passed {} arguments. "
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


# Notes)
# 1) Why define individual functions, rather than use (lambda env, ex: builtin_reduce ... ) ?
#
# We lose the function name, when tracing the evaluation, which can be useful for debugging,
# within a repl environment.
#
# 2 Why check for LispError in the functions builtin_* , function could reduce LispErrors?
#
# The prelude should define map/filter/reduce, that can reduce on LispError values which
# are valid values. Builtin is for specific builtin operators (e.g. +/- etc.), so if wei
# encounter a LispError in the input return it as the result.


def add(env, expr):
    return builtin_reduce(operator.add, expr)


def subtract(env, expr):
    return builtin_reduce(operator.sub, expr)


def multiply(env, expr):
    return builtin_reduce(operator.mul, expr)


def divide(env, expr):
    return builtin_reduce(operator.mul, expr)


def lt(env, expr):
    return binary_function(env, expr, operator.lt)


def gt(env, expr):
    return binary_function(env, expr, operator.gt)


def ge(env, expr):
    return binary_function(env, expr, operator.ge)


def le(env, expr):
    return binary_function(env, expr, operator.le)


def mod(env, expr):
    return binary_function(env, expr, operator.mod)


def power(env, expr):
    return binary_function(env, expr, math.pow)


def sqrt(env, expr):
    if len(expr) == 1:
        if isinstance(expr[0], (float, int)):
            return math.sqrt(expr[0])
        else:
            return EtaError("Runtime error, Function argument {}"
                            " to 'sqrt' is not numeric type.".format(str(expr[0])))
    else:
        return EtaError("Runtime error, Builtin function 'sqrt' does not reduce a list.")


def error(env, expr):
    if len(expr) == 1:
        return EtaError("Runtime error, {}".format(str(expr[0])))
    else:
        return EtaError("Runtime error, {}".format(str(expr)))


def maximum(env, expr):
    return unary_function(expr, max)


def minimum(env, expr):
    return unary_function(expr, min)


def unquote_and_eval(env, expr):
    if isinstance(expr, Expression):
        expr.unquote()
        return evaluate(expr, env)
    else:
        return evaluate(expr, env)


def eval_quoted(env, expr):
    xs = list(map(lambda x: unquote_and_eval(env, x), expr))
    if len(xs) == 1:
        return xs[0]
    else:
        return xs


def load_file(env, expr):
    if eta.evaluation_context:
        print(str(expr))

    if len(expr) > 1:
        return EtaError("Function supplied {} arguments ,"
                        "'load' function expects a single filename argument.".format(len(expr)))
    else:
        if isinstance(expr[0], str):
            try:
                with open(expr[0], 'r') as file:
                    content = file.read()
                    ast = parser.parse(content)
                    return list(filter(lambda x: not empty_expr(x), [evaluate(node, env) for node in ast]))
            except Exception as ex:
                return EtaError(ex)
        else:
            return EtaError("Load function expects argument \"filename\", received: {}".format(expr[0]))


def environment(env, expr):
    """
    Display the contents of an environment.
    """
    if env:
        expression = Expression(env.keys())
        expression.quote()
        return expression


def add_builtins(env):
    env.add_binding(Symbol('+'), add)
    env.add_binding(Symbol('-'), subtract)
    env.add_binding(Symbol('*'), multiply)
    env.add_binding(Symbol('/'), divide)
    env.add_binding(Symbol("error"), error)
    env.add_binding(Symbol("max"), maximum)
    env.add_binding(Symbol("min"), minimum)
    env.add_binding(Symbol("^"), power)
    env.add_binding(Symbol("sqrt"), sqrt)
    env.add_binding(Symbol("<"), lt)
    env.add_binding(Symbol(">"), gt)
    env.add_binding(Symbol(">="), ge)
    env.add_binding(Symbol("<="), le)
    env.add_binding(Symbol("=="), equals)
    env.add_binding(Symbol("%"), mod)
    env.add_binding(Symbol("!="), not_equals)
    env.add_binding(Symbol("head"), head)
    env.add_binding(Symbol("tail"), tail)
    env.add_binding(Symbol("join"), join)
    env.add_binding(Symbol("list"), cons)
    env.add_binding(Symbol("eval"), eval_quoted)
    env.add_binding(Symbol("load"), load_file)
    env.add_binding(Symbol("show"), environment)

