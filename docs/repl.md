# Eta REPL Semantics

[<- Back to Quick Start](quickstart.md) · [Build](build.md) · [Modules](modules.md)

---

## Overview

`eta_repl` reads one submission at a time, wraps it as a module, compiles it,
and keeps state across submissions.

The REPL supports redefinition by shadowing older bindings for new code.

---

## Redefinition Rules

1. If you redefine a name, later submissions use the newest definition.
2. Older submissions keep the bindings they were compiled with.
3. Unrelated names from older submissions remain visible until shadowed.

Example:

```scheme
eta> (defun f (x) x)
eta> (f 10)
=> 10
eta> (defun f (x) (+ x 1))
eta> (f 10)
=> 11
```

Closure behavior:

```scheme
eta> (defun f (x) x)
eta> (defun g (x) (f x))
eta> (defun f (x) (* x 2))
eta> (g 5)
=> 5
eta> (f 5)
=> 10
```

`g` keeps calling the original `f` because it was compiled before the
redefinition.

---

## Implementation Notes

Each submission is wrapped into an internal module named like `__repl_N`.
When building a new submission, the REPL imports only the currently live names
from prior REPL modules. Older shadowed bindings are not imported into new
submissions.

