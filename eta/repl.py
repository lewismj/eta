"""
repl
A command line repl for the Lisp.
"""

from prompt_toolkit import PromptSession
from prompt_toolkit.auto_suggest import AutoSuggestFromHistory
from prompt_toolkit.lexers import PygmentsLexer
from pygments.lexers.lisp import SchemeLexer
from pygments.styles.paraiso_dark import ParaisoDarkStyle
from prompt_toolkit.styles import style_from_pygments_cls
from eta.parser import parser
from eta.types import Environment
from eta.builtins import add_builtins
from eta.eval import evaluate

# builtin, or funcs read from env..
# from prompt_toolkit.completion import WordCompleter


def prompt_continuation(width, line_number, is_soft_wrap):
    return '.' * width


def repl(env):
    """
    Runs a simple read-eval-print loop. Without defining a custom lexer
    (n.b. Lexer here is used just for highlighting, Lark is used for
    scanner/parser); the pygments 'SchemeLexer' is used.
    """
    session = PromptSession(lexer=PygmentsLexer(SchemeLexer))
    while True:
        try:
            text = session.prompt('eta> ', vi_mode=True,
                                  multiline=True,
                                  prompt_continuation=prompt_continuation,
                                  style=style_from_pygments_cls(ParaisoDarkStyle),
                                  auto_suggest=AutoSuggestFromHistory()).strip()
        except KeyboardInterrupt:
            continue
        except EOFError:
            break
        else:
            try:
                if text:
                    ast = parser.parse("({})".format(text))
                    result = evaluate(ast[0], env)
                    print(str(result))
            except Exception as ex:
                print(ex)


if __name__ == '__main__':
    global_env = Environment()
    add_builtins(global_env)
    repl(global_env)
