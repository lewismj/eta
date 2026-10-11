# Eta formatter style

The invocation defaults are width 100 and indentation 2. They are explicit CLI
or library options; there is no project style file or configurable alias table.
Width is measured in Unicode scalars; a tab in raw text counts as one character.

A plain list fits on one line when its flattened Doc fits the remaining width.
In broken layout, each argument starts on a new line at the list's starting
column plus the selected indentation. The head stays after the opener, and the
close follows the last argument on its final line. A trailing line comment
forces the close onto a new line at the list's starting column. Empty and
singleton lists have no synthetic argument separator. Original parentheses,
brackets, dispatch tokens and dots are preserved.

```scheme
(head
  argument
  another)
```

Phase 3 expands the alias table to the common special-form heads: `defun`,
`define`, `lambda`, `let`, `let*`, `letrec*`, `begin`, `module`, `if`,
`when`, `unless`, `cond`, `case`, `do`, `import`, `export` and
`define-record-type`. Their shape
checks are still required before selection; a spelling match with an unverified
shape uses generic layout. The dedicated aliases keep the body/branch structure
stable while the fallback rule still handles unknown macro invocations and
patterns.

Reader abbreviations retain their exact prefix and bound operand; explicit
quote forms stay explicit. Dotted lists retain the dot/tail in token order.
Vectors and bytevectors retain dispatch, delimiters, values and raw numeric
spelling. Strings, escaped symbols and comment bodies are never rewritten.

Comment gaps preserve ordered comment identities, raw bodies, enclosing paths
and anchor sides. End-of-line comments stay with the preceding token and force
a break; standalone comments remain before their original next item/close/EOF.
Nested block comments retain their internal indentation and line endings.
Comment-separated blank lines preserve their banner/directive association.
Ordinary arguments have no blank lines; top-level forms and known begin/module
gaps retain at most one separating blank line.

Two conservative paths report coverage with the original byte ranges:

- A datum comment and every reader item consumed by the compiler's discard
  routine are copied exactly. The discard grammar is more permissive than the
  active-expression grammar; formatting never invents a stricter interpretation.
- Physical-line lint directives retain their original applicability through
  exact passthrough. Form-bound directives retain their checked comment anchors.

Passthrough bytes are stable on repeated runs; surrounding source can still be
formatted when its boundary is proven. A successful check means the selected
format operation makes no further changes, including its documented passthrough
regions. It does not claim those regions have canonical generic layout.

The production guard uses fresh original compiler readers on both sources, full
directive/state/path traces, exact ordered tokens, suppressed datum boundaries
and ordered comment associations. The CST S-expression bridge is not its oracle.
The property suite checks idempotence with the full effective option set,
including reader configuration, LF/CRLF policy and final-newline presence.

