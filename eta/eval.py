"""
eval
This is the basic Lisp-ish evaluator.
"""

from eta.types import Symbol, Expression, Definition, EmptyExpr, IfExpression


# n.b. Most of these methods would be improved by use of the
# structured pattern matching (Python 3.10).

def evaluate(exp, env):
    """
    The main evaluation loop.
    :param exp: the expression to evaluate.
    :param env: the environment to use for evaluation.
    :return: the result, tail recursive.
    """
    if isinstance(exp, list):
        if len(exp) == 0:
            return Expression([])

        xs = [evaluate(x, env) for x in exp]

        if len(xs) == 1:
            return xs[0]
        else:
            return xs

    if isinstance(exp, Symbol):
        return env.lookup_binding(exp)

    if isinstance(exp, Definition):
        return eval_definition(exp, env)

    if isinstance(exp, IfExpression):
        return eval_if(exp, env)

    if isinstance(exp, Expression) and not exp.is_quoted():
        return eval_s_expr(exp, env)

    return exp


def eval_s_expr(exp, env):
    if len(exp) == 0:
        return exp
    if len(exp) == 1:
        return evaluate(exp[0], env)

    # applicative order evaluation; a function definition would be skipped over and
    # its arguments reduced first, before finally invoking the function.
    for i in range(len(exp)):
        exp[i] = evaluate(exp[i], env)

    # n.b. type check for eta.Lambda should come first.

    if callable(exp[0]):
        function, *arguments = exp
        return function(env, arguments)
    else:
        return exp


def eval_definition(exp, env):
    value = evaluate(exp.value, env)
    env.add_binding(exp.symbol, value)
    return EmptyExpr
