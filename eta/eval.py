"""
eval
A set of functions for evaluating an AST. By design we separate the evaluator from
the AST. Rather than have the eval functions as methods on the different node types.
"""

from eta.types import Symbol, Expression, Definition, EmptyExpr, IfExpression, \
    Lambda, EtaError, AndDefinition, OrDefinition, QuoteType
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


def find(items, pred): return next((i for i in items if pred(i)), None)


def evaluate(expr, env):
    """
    The main evaluation loop.
    :param expr: the expression to evaluate.
    :param env: the environment to use for evaluation.
    :return: the result, tail recursive.
    """

    if isinstance(expr, list):
        if len(expr) == 0:
            return Expression([])

        xs = [evaluate(x, env) for x in expr]

        if len(xs) == 1:
            return xs[0]
        else:
            return xs

    if isinstance(expr, Symbol):
        return env.lookup_binding(expr)

    if isinstance(expr, Definition):
        return eval_definition(expr, env)

    if isinstance(expr, AndDefinition):
        return eval_and_definition(expr, env)

    if isinstance(expr, OrDefinition):
        return eval_or_definition(expr, env)

    if isinstance(expr, IfExpression):
        return eval_if(expr, env)

    if isinstance(expr, Expression) and not expr.is_quoted():
        return eval_s_expr(expr, env)

    return expr


def empty_expr(expr):
    if not isinstance(expr, Expression) or expr.kind != QuoteType.NoQuote:
        return False
    return expr == EmptyExpr


def eval_s_expr(expr, env):
    if evaluation_context.trace:
        print(str(expr))

    if len(expr) == 0:
        return expr

    if len(expr) == 1:
        result = evaluate(expr[0], env)
        # Handle the case where s-expression is of the form (symbol)
        # And symbol is a built-in function that takes no args.
        # It should be defined via 'define' rather than a function. However,
        # support it here, as we may want some of these functions to work without
        # prelude? (see definition of env in builtin).
        # User defined functions are just Lambda, that can be partially applied;
        # no handling required here.
        if callable(result):
            if evaluation_context.trace:
                print("builtin:{}".format(result.__name__))
            return result(env, Expression([]))
        else:
            return result

    # Filter out EmptyExpr, which is success of 'define'.
    expr = list(filter(lambda x: not empty_expr(x), map(lambda x: evaluate(x, env), expr)))

    # Check to see if there is any evaluation to do, as we may have
    # an s-expression that just contains 'define' statements.
    if expr:

        if isinstance(expr[0], Lambda):
            function, *arguments = expr
            return eval_lambda(function, arguments, env)

        if callable(expr[0]):
            function, *arguments = expr
            if evaluation_context.trace:
                print("builtin:{} {}".format(function.__name__, arguments))
            return function(env, arguments)

        else:
            if len(expr) == 1:
                return expr[0]
            else:
                return expr
    else:
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
        return EtaError("Lambda function supplied too many arguments: {}, ".format(n_args)
                        + str(lambda_fn) + " Expected {} arguments.".format(n_formals))

    # todo; could be extended to support a varargs syntax x & xs etc?

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

    # Substitute formals for supplied arguments.
    while len(arguments) > 0:
        argument = arguments.pop(0)
        formal = formals.pop(0)
        env.add_binding(formal, argument)

    # If all the arguments were supplied, return the evaluation of the function.
    # If not, return a Lambda; that is the partially applied function.
    # (Essentially a Lambda, with same body but any argument supplied is added
    #  to the lambda environment and removed from the list of expected arguments).
    if len(formals) == 0:
        return evaluate(body, env)
    else:
        return Lambda(formals, body, env)

#
# Special forms.
#  Handle these directly, though they could be implemented as built-in functions,
#  with their grammar rules removed form the parser.


def eval_definition(expr, env):
    if evaluation_context.trace:
        print(expr)
    value = evaluate(expr.value, env)
    env.add_binding(expr.symbol, value)
    return EmptyExpr


def eval_condition(expr, env):
    if evaluation_context.trace:
        print(str(expr))

    condition = evaluate(expr, env)

    if isinstance(condition, EtaError):
        return condition

    if isinstance(condition, list):
        lisp_error = find(condition, lambda x: isinstance(x, EtaError))
        if lisp_error:
            return lisp_error
        else:
            return EtaError("Invalid 'if' condition: {}"
                            " Could not reduce to boolean.".format(str(condition)))

    # This could be changed, but in general don't allow 'everything' to be
    # casted to boolean automatically.
    if isinstance(condition, (float, int, complex, str, list)):
        condition = bool(condition)

    if not isinstance(condition, bool):
        return EtaError("'If' expression clause {} did not evaluate to boolean.".format(str(condition)))

    return condition


def eval_or_definition(expr, env):
    if evaluation_context.trace:
        print(expr)

    or_expr = find(expr, lambda x: eval_condition(x, env))
    if or_expr:
        return True

    return False


def eval_and_definition(expr, env):
    if evaluation_context.trace:
        print(expr)

    for sub_expr in expr:
        condition = eval_condition(sub_expr, env)
        if isinstance(condition, EtaError):
            return EtaError
        elif not condition:
            return False

    return True


def eval_if(expr, env):
    if evaluation_context.trace:
        print(expr)

    condition = eval_condition(expr.clause, env)

    if isinstance(condition, EtaError):
        return condition

    if condition:
        return evaluate(expr.then_expr, env)
    else:
        return evaluate(expr.else_expr, env)
