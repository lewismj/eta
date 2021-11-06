"""
parser
Uses lark to define a simple Lisp style syntax.
"""
from lark import Lark, Transformer, v_args
from lark.visitors import VisitError
from eta.types import Symbol, Expression, _quote, _quasi_quote, Lambda, Definition, IfExpression

grammar = r"""
    start:expression+
    
    ?expression:  "(" "if"  expression  expression expression ")" -> if_expression
                | "(" "define" variable expression ")" -> define
                | "(" "define" "(" variable ")" expression ")" -> define
                | "(" "defun" "(" function_name formals ")" expression ")" -> defun
                | "(" "lambda" "(" formals ")" expression ")" -> lambda_expression
                | [quote] "(" value+ ")" -> expression
                | "[" value+ "]" -> quoted_expression 
                
    ?quote: normal_quote | quasi_quote
    normal_quote:"'" -> const_quote 
    quasi_quote: "`" -> const_quasi

    ?value: symbol | number | expression | boolean | string 
    symbol: SYMBOL -> symbol_def
    
    // for convenience, we treat "+ " => symbol definition, whereas "-" number is unary operator.
    // practical optimisation, otherwise we'd require a 'reduce unary operator' function in the 
    // evaluator, here we can just parse -ve numbers before they reach the eval.
    // i.e. have token (-x) rather than op(*,-1,x).
    
    SYMBOL: NAME | "+" " " | "-" " "  | "*" | "/" | "<" | ">" | ">=" | "<=" | "==" | "="  
    !number:  ["-"] NUMBER | ["+"] NUMBER

    formals: symbol+ -> formals
    function_name: NAME -> function_name
    variable: NAME -> variable
    
    boolean: "#t" -> const_true 
            | "#f" -> const_false

    string: ESCAPED_STRING
    COMMENT: /;[^\n]*/
    
    %import common.CNAME -> NAME
    %import common.ESCAPED_STRING
    %import common.NUMBER
    %import common.WS

    %ignore COMMENT
    %ignore WS
"""


class AstTransformer(Transformer):
    """
    Transforms the parse tree (defined as token types in structure of 'grammar')
    into an AST that is ready to be evaluated.
    """
    ESCAPED_STRING = str
    NAME = str

    def __init__(self):
        super().__init__()

    @staticmethod
    def start(items):
        return items

    @staticmethod
    def variable(xs):
        return xs[0]

    @staticmethod
    def symbol_def(xs):
        return Symbol(xs[0].strip())

    @staticmethod
    def define(xs):
        if len(xs) == 2:
            definition = Definition(Symbol(xs[0]), xs[1])
            return definition
        else:
            VisitError("Definition with {} arguments."
                       " 'define' should be of the form (value) (expression).".format(len(xs)))

    @staticmethod
    def defun(xs):
        if len(xs) == 3:
            definition = Definition(xs[0], Lambda(xs[1], xs[2], []))
            return definition
        else:
            VisitError("Function definition with {} arguments."
                       " 'defun' should be of the form (function_name params) (expression).".format(len(xs)))

    @staticmethod
    def if_expression(xs):
        if (len(xs)) == 3:
            definition = IfExpression(xs[0], xs[1], xs[2])
            return definition
        else:
            VisitError("If expression with {} arguments."
                       " 'if' should be of the form (clause) (then) (else).".format(len(xs)))

    @staticmethod
    def string(xs):
        return xs[0][1:-1].replace('\\"', '"')

    @staticmethod
    def function_name(xs):
        return xs[0]

    @staticmethod
    def formals(xs):
        return xs

    @staticmethod
    def const_true(t):
        return True

    @staticmethod
    def const_false(f):
        return False

    @staticmethod
    def const_quote(q):
        return _quote

    @staticmethod
    def const_quasi_quote(q):
        return _quasi_quote

    @staticmethod
    def lambda_expression(xs):
        if len(xs) == 2:
            return Lambda(xs[0], xs[1], [])
        else:
            raise VisitError("Lambda expression defining {} arguments, should be (formals) (body).".format(len(xs)))

    @staticmethod
    def expression(xs):
        if xs[0] in [_quote, _quasi_quote]:
            expression = Expression(xs[1:])
            if xs[0] == _quote:
                expression.quote()
            else:
                expression.quasi_quote()
            return expression
        else:
            return Expression(xs)

    @staticmethod
    def quoted_expression(xs):
        expression = Expression(xs)
        expression.quote()
        return expression

    @staticmethod
    def to_number(s):
        try:
            return int(s)
        except ValueError:
            return float(s)

    @staticmethod
    def number(xs):
        value = AstTransformer.to_number(xs[-1])
        if xs[0] == "-":
            return value * -1
        else:
            return value


parser = Lark(grammar, parser='lalr', transformer=AstTransformer())

if __name__ == '__main__':
    test = parser.parse("(+ 1 2)")
    print(test)

