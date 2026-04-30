# eval

[<- Back to README](./README.md)

---

## Form

```scheme
(eval expr)
```

`eval` compiles and executes `expr` at runtime.

Evaluation uses the current lexical environment first, then global bindings and builtins.

| `expr` | Result |
| --- | --- |
| Number, string, boolean, and other self-evaluating values | Returned unchanged |
| Symbol | Resolved in current lexical scope, then globals/builtins |
| List or pair | Compiled and evaluated as code |
| Other heap values | Returned unchanged |

---

## Error behavior

- Runtime errors raised while executing eval code preserve their existing tags (for example `runtime.type-error`).
- User-raised tags preserve their original symbol.
- Compile-path failures are reported as `runtime.user-error` with diagnostic text.
- There is no `eval-error` wrapper tag.

---

## Examples

```scheme
(eval '(+ 1 2))                           ; => 3
(let ((x 10)) (eval '(+ x 5)))           ; => 15
(let ((x 10))
  (let ((f (lambda (y) (eval '(+ x y)))))
    (f 7)))                               ; => 17
```

Runtime tag preservation:

```scheme
(catch 'runtime.type-error
  (eval '(car 1)))
```

User tag preservation:

```scheme
(catch 'my-tag
  (eval '(raise 'my-tag 7)))
```

---

## Notes

- `eval` is a builtin and is available without importing a module.
- Reentrant calls are supported (calling `eval` from inside running functions is valid).
