"""
repl
A command line repl for the Lisp.
"""

from prompt_toolkit import PromptSession
from prompt_toolkit.auto_suggest import AutoSuggestFromHistory
from prompt_toolkit.history import FileHistory
from prompt_toolkit.key_binding import KeyBindings
from prompt_toolkit.enums import EditingMode
from prompt_toolkit.application.current import get_app
from prompt_toolkit.lexers import PygmentsLexer
from pygments.lexers.lisp import SchemeLexer
from pygments.styles.paraiso_dark import ParaisoDarkStyle
from prompt_toolkit.styles import style_from_pygments_cls
from prompt_toolkit.completion import WordCompleter

import eta
from eta.parser import parser
from eta.types import Environment
from eta.builtins import add_builtins
from eta.eval import evaluate

from pathlib import Path


# Set up word completion, use keywords that define special forms,
# the global-env can be examined at start-up for additional keywords.
completer_words = WordCompleter(['define', 'defun', 'if', 'lambda', 'and', 'or'])


def prompt_continuation(width, line_number, is_soft_wrap):
    return '.' * width


def repl(env):
    """
    Runs a simple read-eval-print loop. Without defining a custom lexer
    (n.b. Lexer here is used just for highlighting, Lark is used for
    scanner/parser); the pygments 'SchemeLexer' is used.
    """

    bindings = KeyBindings()
    history_file = str(Path.home().joinpath(".eta_history.txt"))

    @bindings.add('f1')
    def _event(event):
        app = event.app
        if app.editing_mode == EditingMode.VI:
            app.editing_mode = EditingMode.EMACS
        else:
            app.editing_mode = EditingMode.VI

    @bindings.add('f2')
    def _event(event):
        eta.evaluation_context.trace = not eta.evaluation_context.trace

    # Add a toolbar at the bottom to display the current input mode.
    def bottom_toolbar():
        edit_mode = 'vi' if get_app().editing_mode == EditingMode.VI else 'emacs'
        trace_mode = 'trace/on' if eta.evaluation_context.trace else 'trace/off'
        return [
            ('class:toolbar', ' [F1] {}'.format(edit_mode)),
            ('class:toolbar', ' [F2] {}'.format(trace_mode))
        ]

    session = PromptSession(history=FileHistory(history_file), lexer=PygmentsLexer(SchemeLexer))
    while True:
        try:
            text = session.prompt('eta> ', vi_mode=True,
                                  multiline=True,
                                  prompt_continuation=prompt_continuation,
                                  style=style_from_pygments_cls(ParaisoDarkStyle),
                                  auto_suggest=AutoSuggestFromHistory(),
                                  completer=completer_words,
                                  key_bindings=bindings,
                                  bottom_toolbar=bottom_toolbar).strip()
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
    # add longer symbols in the global environment to the word completer.
    additional_words = list(filter(lambda x: len(x) > 2, global_env.keys()))
    completer_words.words += additional_words
    repl(global_env)
