"""
types
Defines the core types used by the Lisp interpreter.
"""
from io import StringIO
from enum import Enum
from collections import deque

# Constants
_quote = "'"
_quasi_quote = "`"


class EtaError(Exception):
    """
    The LispError is a type that represents any error that may be generated when evaluating a Lisp expression.

    Since we are using Python to interpret (i.e. evaluator is using Python constructs and not a stack machine
    written in Python) we lift any Python exceptions into instances of LispError.
    """
    def __init__(self, message=None):
        super().__init__(message)

    def __eq__(self, other):
        """
        If the Lisp expression is the same, they're equivalent.
        """
        return self.__repr__() == super().__repr__()


class Symbol(str):
    """
    A Symbol is a string that represent an identifier, function or other atom.

    The most common type of symbol is an identifier, i.e. a simple variable name.
    Within Lisp environment function names, for example '+' or 'sqrt' are symbols too.
    More formally, any sequence of letters, digits and permissible special characters
    that are not a number.
    """
    pass


class QuoteType(Enum):
    """
    Quote type is just a flag that can be set on an expression to convert
    it from from a regular S-Expression to some quoted type (and visa versa).
    """
    NoQuote = 1  # S expression, to be evaluated, applicative order.
    Quote = 2  # Quoted expression; not eagerly evaluated.
    QuasiQuote = 3  # Quasi-quoted expression.


class Expression(deque):
    """
    An expression can be an S Expression of the form:
        (+ 1 1)
        (lambda (x) (+ 1 1) 10
        ... etc.

    Or a quoted expression, using `( ... ) syntax.
    The quoted expression will return itself on evaluation.  Only evaluated when we call 'eval' built-in.
    Setting an expression type to 'quoted' can be used to delay evaluation of say function arguments etc.

    Expression is a type of list, each element in the list is essentially a 'cell' in Lisp terms.
    """

    def __init__(self, values=None):
        if not values:
            values = []
        super().__init__(values)
        self.kind = QuoteType.NoQuote

    def unquote(self):
        self.kind = QuoteType.NoQuote

    def quote(self):
        self.kind = QuoteType.Quote

    def quasi_quote(self):
        self.kind = QuoteType.QuasiQuote

    def is_quoted(self):
        return self.kind == QuoteType.Quote

    def is_quasi_quoted(self):
        return self.kind == QuoteType.QuasiQuote

    def __str__(self):
        """
        Return a string representation of the expression.
        :return: a string representation of the expression.
        """
        buffer = StringIO()
        if self.is_quoted():
            buffer.write('\'')

        buffer.write('(')
        buffer.write(" ".join("{}".format(k) for k in self))
        buffer.write(')')

        desc = buffer.getvalue()
        buffer.close()
        return desc

    def __repr__(self):
        """
        Return __str__ (Lisp expression, so 'repr' in this context is correct).
        """
        return self.__str__()


EmptyExpr = Expression([])


class Lambda:
    """
    Class holds the formals, body and environment of a lambda function.
    For example,
     ( lambda (x y) (+ x y) )
    Is an S-Expression containing a lambda function of the form:
        formals: (x y)
        body: (+ x y)
    When defined a Lambda will have an empty environment. Or, may
    have an environment with some values defined (the result of
    partial function application).
    However, each instance of the lambda will have its own environment
    when 'running' which will be a clone of the definition.
    """

    def __init__(self, formals, body, environment):
        """
        Create an Lambda function, that can be used as a template by function invocation.
        :param formals: the function arguments.
        :param body: the function body.
        """
        self.formals = formals
        self.body = body
        self.environment = environment

    def __str__(self):
        """
        Generates a string that represents the Lambda function. Of the form:
            formals: expression
            body:    expression.
        :return: A string representation of the formals and body of the lambda function.
        """
        buffer = StringIO()
        buffer.write("(λ (")
        buffer.write(" ".join("{}".format(x) for x in self.formals))
        buffer.write(") ")
        buffer.write(str(self.body))
        buffer.write(")")
        desc = buffer.getvalue()
        buffer.close()

        return desc

    def __repr__(self):
        """
        Return __str__ (Lisp expression, so 'repr' in this context is correct).
        """
        return self.__str__()


class Definition:
    """
    Used by parser, this creates a pair (Symbol, Value), that
    represents a binding. Objects of this type will be part of
    the AST interpreted by the evaluation.
    Symbol can be a name (E.g. function name or identifier) and
    Value can be an expression (E.g. lambda or atom etc..)
    """

    def __init__(self, symbol, value):
        self.symbol = symbol
        self.value = value

    def __str__(self):
        buffer = StringIO()
        buffer.write("(define (")
        buffer.write(str(self.symbol))
        buffer.write(") (")
        buffer.write(str(self.value))
        buffer.write(")")
        desc = buffer.getvalue()
        buffer.close()

        return desc

    def __repr__(self):
        """
        Return __str__ (Lisp expression, so 'repr' in this context is correct).
        """
        return self.__str__()


class IfExpression:
    """
    Used by the parser to represent an 'if then else' expression.
    Objects of this type will be part of the AST interpreted by
    the evaluation, and part of the 'fixed' syntax.
    """

    def __init__(self, clause, then_expr, else_expr):
        self.clause = clause
        self.then_expr = then_expr
        self.else_expr = else_expr

    def __str__(self):
        buffer = StringIO()
        buffer.write("(if (")
        buffer.write(str(self.clause) + " ")
        buffer.write(str(self.then_expr) + " ")
        buffer.write(str(self.else_expr))
        desc = buffer.getvalue()
        buffer.close()

        return desc

    def __repr__(self):
        """
        Return __str__ (Lisp expression, so 'repr' in this context is correct).
        """
        return self.__str__()

# note, if And/Or, change to 'list' rather than 'deque' the instance
# type check would need to be re-ordered to test for And/Or first,
# otherwise it would evaluate all arguments. Alternatively, And/Or could
# just have a container as a member rather than inheriting.


class AndDefinition(deque):
    """
    This class represent AST representation of (and expr1 expr2 ... expr n)
    The 'and' operator could be implemented within the prelude (even implement via church encoding,
    which is a test) however, for efficiency, it makes sense to define it as a special form.

    Simply a list of expressions. Structural pattern matching in the evaluator is used to process
    the expression  ( and ... ) to a True/False value.
    """
    def __init__(self):
        super().__init__()

    def __str__(self):
        buffer = StringIO()
        buffer.write("(and ")
        [buffer.write(str(x) + " ") for x in self]
        buffer.write(")")
        desc = buffer.getvalue()
        buffer.close()

        return desc

    def __repr__(self):
        """
        Return __str__ (Lisp expression, so 'repr' in this context is correct).
        """
        return self.__str__()


class OrDefinition(deque):
    """
    This class represent AST representation of (or expr1 expr2 ... expr n)
    The 'or' operator could be implemented within the prelude (even implement via church encoding,
    which is a test) however, for efficiency, it makes sense to define it as a special form.

    Simply a list of expressions. Structural pattern matching in the evaluator is used to process
    the expression  ( and ... ) to a True/False value.
    """
    def __init__(self):
        super().__init__()

    def __str__(self):
        buffer = StringIO()
        buffer.write("(or ")
        [buffer.write(str(x) + " ") for x in self]
        buffer.write(")")
        desc = buffer.getvalue()
        buffer.close()

        return desc

    def __repr__(self):
        """
        Return __str__ (Lisp expression, so 'repr' in this context is correct).
        """
        return self.__str__()


class Environment(dict):
    """
    An environment is essentially a set of dictionaries where variables and functions can be stored.

    From "The Structure and Interpretation of Computer Programs.", Harold Abelson et al. pp. 236
    "An environment is a sequence of frames. Each frame is a table (possibly empty) of bindings,
    which associate variable names with their corresponding values. A single frame may contain at
    most one binding for any variable.) Each frame also has a pointer to its enclosing environment,
    unless for the purposes of discussion, the frame is considered to be global.
    The value of a variable with respect to an environment is the value given by the binding of the
    variable in the first frame in the environment that contains a binding for that variable.
    If no frame in the sequence specified a binding for the variable, then the variable is said to
    be unbound in the environment."

    It is important to note that an symbol may be bound to a built-in function, lambda, partially
    applied function in addition to any primitive type.
    """

    def __init__(self, outer=None):
        """
        Initialises an empty frame with specified outer frame.
        :param outer: the outer enclosing scope.
        """
        super().__init__()
        self.outer = outer

    def add_binding(self, sym, val):
        """
        Add a binding of the symbol to the value.
        :param sym: the symbol.
        :param val: the value.
        """
        if isinstance(sym, Symbol):
            self[sym] = val
        else:
            # Throw this as an exception since it should never happen.
            raise EtaError("Trying to bind value to non Symbol type.")

    def lookup_binding(self, sym):
        """
        Lookup a binding in the environment. This function will search the outer scope(s)
        for the symbol if necessary.
        :param sym: the symbol to lookup.
        :return: the corresponding value, or LispError itself, if the value could not be found.
        """
        try:
            return self[str(sym)]
        except KeyError:
            if self.outer is None:
                return EtaError("Runtime error, unbound symbol: " + sym)
            else:
                return self.outer.lookup_binding(sym)


EmptyEnvironment = Environment()
