"""
eval
A set of functions for evaluating an AST. By design we separate the evaluator from
the AST. Rather than have the eval functions as methods on the different node types.
"""

from eta.types import Symbol, Expression, Definition, EmptyExpr, IfExpression, Lambda, LispError, Environment
from copy import deepcopy

# n.b. Most of these methods would be improved by use of the
# structured pattern matching (Python 3.10).


class EvaluationContext:
    """
    The evaluation context is an instance that is used to set flags for evaluator.
    For example, trace the evaluation, etc.
    """
    def __init__(self):
        self.trace = False


evaluation_context = EvaluationContext()


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
    if evaluation_context.trace:
        print(exp)

    if len(exp) == 0:
        return exp
    if len(exp) == 1:
        return evaluate(exp[0], env)

    # Filter out EmptyExpr, which is success of 'define'.
    exp = list(filter(lambda x: x != EmptyExpr, map(lambda x: evaluate(x, env), exp)))

    # Check to see if there is any evaluation to do, as we may have
    # an s-expression that just contains 'define' statements.
    if exp:
        if isinstance(exp[0], Lambda):
            function, *arguments = exp
            return eval_lambda(function, arguments, env)

        if callable(exp[0]):
            function, *arguments = exp
            if evaluation_context.trace:
                print("builtin:{} {}".format(function.__name__, arguments))
            return function(env, arguments)
        else:
            if len(exp) == 1:
                return exp[0]
            else:
                return exp
    else:
        return EmptyExpr


def eval_definition(exp, env):
    if evaluation_context.trace:
        print(exp)
    value = evaluate(exp.value, env)
    env.add_binding(exp.symbol, value)
    return EmptyExpr


def eval_lambda(lambda_fn, arguments, outer_env):
    """
    Evaluate a lambda expression.
    :param lambda_fn the lambda function to execute.
    :param arguments the arguments to the function (may be partially applied).
    :param outer_env the parent environment, each lambda will use its own environment for evaluation.
    """
    n_formals = len(lambda_fn.formals)
    n_args = len(arguments)

    # It is ok to supply too few parameters (i.e. partially apply the function, but
    # return an error if too many arguments are supplied.
    if n_args > n_formals:
        return LispError("Lambda function supplied too many arguments: {}, ".format(n_args)
                         + str(lambda_fn) + " Expected {} arguments.".format(n_formals))

    # Create an environment to evaluate the lambda.
    # For each formal (Argument) specified, place that argument into the environment.
    # Two cases:
    #   1. All the arguments are supplied, the function can be evaluated.
    #   2. Some of the arguments are supplied, we return a new lambda (i.e. a partially applied function).

    # When a lambda is defined its passed a ref to the empty environment.
    # So each time the lambda is invoked, we copy it, so that each function application
    # has its own environment.

    env = deepcopy(lambda_fn.environment)
    env.outer = outer_env

    # Take a copy of the function arguments that will be bound to the supplied
    # arguments in this evaluation (i.e. add bindings into the function environment).
    formals = deepcopy(lambda_fn.formals)

    # Generally the body of a lambda function should be immutable, so no need to copy it.
    # However, we could allow the manipulation of it (via macro or some other way), so
    # on each invocation, make a copy of the body.
    body = deepcopy(lambda_fn.body)

    # 1. Substitute formals for supplied arguments.
    while len(arguments) > 0:
        argument = arguments.pop(0)
        formal = formals.pop(0)
        env.add_binding(formal, argument)

    if len(formals) == 0:
        # arguments full specified, lambda can be evaluated.
        return eval_s_expr(body, env)
    else:
        # function is partially applied, return partially applied function.
        return Lambda(formals, body, env)


def eval_if(exp, env):
    pass
