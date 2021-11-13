"""
parser
Uses lark to define a simple Lisp style syntax.
"""
from lark import Lark, Transformer
from lark.visitors import VisitError
from eta.types import Symbol, Expression, _quote, _quasi_quote, Lambda, Definition, \
    IfExpression, EmptyEnvironment, AndDefinition, OrDefinition


# Definition of basic grammar.
# Note:
# 1. We only -really- need "(" expression* ")" as rule.
# 2. For example, all the list operators, 'head', 'tail', and utility functions like 'load'
#    are regular (if builtin) functions and obviously don't have their own grammar rules.
# 3. The purpose of having 'special forms' , namely , 'if', 'define' etc. under '?expression'
#    is so that we can perform syntax check during parse and create AST node that would
#    allow for a faster evaluation. i.e. Simplify and speed up the interpreter.
# 4. I may experiment and start removing grammar rules and just implement them as regular functions,
#    if there isn't too much of a performance hit.

grammar = r"""
    start:expression+
    
    //  However, trade off - treating these as special forms and parsing them via grammar,
    //  allows for syntax check during parse and AST nodes to built that allow simplified 
    //  evaluation.
    
    ?expression:  "(" "if"  expression  expression expression ")" -> if_expression
                | "(" ("define" | "def") variable expression ")" -> define
                | "(" ("define" | "def") "(" variable ")" expression ")" -> define
                | "(" "defun" "(" function_name formals ")" expression ")" -> defun
                | "(" "lambda" "(" formals ")" expression ")" -> lambda_expression
                | "(" "and" expression+ ")" -> and_expression
                | "(" "or" expression+ ")" -> or_expression
                | "(" "let"  let_body+  ")" -> let_expression 
                | [quote] "(" value* ")" -> expression
                
                
    let_body: "(" variable let_value ")" | "(" variable expression ")"
    let_value: number | boolean | string
    
    ?quote: normal_quote | quasi_quote
    normal_quote:"'" -> const_quote 
    quasi_quote: "`" -> const_quasi

    ?value: symbol | number | expression | boolean | string 
    symbol: SYMBOL -> symbol_def
    
    // for convenience, we treat "+ " => symbol definition, whereas "-" number is unary operator.
    // practical optimisation, otherwise we'd require a 'reduce unary operator' function in the 
    // evaluator, here we can just parse -ve numbers before they reach the eval.
    // i.e. have token (-x) rather than op(*,-1,x).
    
    SYMBOL: NAME | "+" " " | "-" " "  | "*" | "/" | "<" | ">" | ">=" | "<=" | "==" | "="  | "%" | "^" | "!=" 
    !number:  ["-"] NUMBER | ["+"] NUMBER

    formals: variable+ -> formals
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
        return Symbol(xs[0])

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
            definition = Definition(Symbol(xs[0]), Lambda(xs[1], xs[2], EmptyEnvironment))
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
            return Lambda(xs[0], xs[1], EmptyEnvironment)
        else:
            raise VisitError("Lambda expression defining {} arguments, should be (formals) (body).".format(len(xs)))

    @staticmethod
    def let_expression(xs):
        expression = Expression()
        [expression.append(x) for x in xs]
        return expression

    @staticmethod
    def let_body(xs):
        if len(xs) == 2:
            definition = Definition(Symbol(xs[0]), xs[1])
            return definition
        else:
            raise VisitError("Let expression defining {} arguments, "
                             "should be of the form 'variable value' or 'variable expression'".format(len(xs)))

    @staticmethod
    def let_value(xs):
        return xs[0]

    @staticmethod
    def and_expression(xs):
        definition = AndDefinition()
        definition.extend(xs)
        return definition

    @staticmethod
    def or_expression(xs):
        definition = OrDefinition()
        definition.extend(xs)
        return definition

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
