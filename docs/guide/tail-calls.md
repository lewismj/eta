# Tail Calls

[← Back to Language Guide](./language_guide.md)

Eta guarantees **proper tail-call optimisation** (TCO): a function call
in tail position reuses the current activation frame instead of pushing
a new one. This means recursion is the right way to write loops — there
is no stack-depth penalty.

---

## What "tail position" means

An expression is in tail position if its value is the value of the
enclosing function. The following positions are all tail positions:

| Construct                      | Tail position(s)                          |
| :----------------------------- | :---------------------------------------- |
| `(lambda (…) … expr)`          | The last `expr` of the body               |
| `(if t then else)`             | Both `then` and `else`                    |
| `(cond … (test body …))`       | The last `body` of every clause           |
| `(when test body …)`           | The last `body`                           |
| `(unless test body …)`         | The last `body`                           |
| `(case key … (lits body …))`   | The last `body` of every clause           |
| `(and e₁ … eₙ)`                | `eₙ` only                                 |
| `(or  e₁ … eₙ)`                | `eₙ` only                                 |
| `(begin … expr)`               | The last `expr`                           |
| `(let / let* / letrec … expr)` | The last `expr` of the body               |

Calls that are *not* in tail position (the most common: arguments to
another call, the test of `if`, intermediate `begin` forms) consume
stack as normal.

---

## Worked examples

### Tail-recursive factorial

```scheme
(defun factorial (n)
  (let loop ((n n) (acc 1))
    (if (= n 0)
        acc
        (loop (- n 1) (* acc n)))))    ; tail call
(factorial 20)                          ; => 2432902008176640000
```

### Mutual recursion

```scheme
(letrec
  ((even? (lambda (n) (if (= n 0) #t (odd?  (- n 1)))))
   (odd?  (lambda (n) (if (= n 0) #f (even? (- n 1))))))
  (even? 1000000))                      ; => #t  (constant stack)
```

---

## VM mechanics

Tail calls compile to a dedicated `TailCall` opcode, distinct from
`Call`. `TailCall` overwrites the current frame's slot region with the
new arguments, then jumps to the callee — no return-address push, no
frame growth.

See [`bytecode-vm.md`](./reference/bytecode-vm.md) for the opcode catalogue.

---

## Interactions

| With                | Behaviour                                                       |
| :------------------ | :-------------------------------------------------------------- |
| `catch` / `raise`   | A call inside `(catch tag …)` is **not** in tail position relative to the enclosing function — the catch frame must be popped on return. |
| `dynamic-wind`      | Same: the *after* thunk must run on exit.                       |
| Logic / unification | Goal calls inside `findall` / `run1` are not user-visible tail calls; the trail manages its own stack discipline. |

> [!WARNING]
> If you wrap a recursive helper in `(catch …)` you lose TCO across the
> recursion. Push the `catch` outside the loop, or use the early-exit
> pattern of [Error Handling](./error-handling.md) where the loop is
> *inside* the catch.

---

## Diagnosing missed TCO

Symptoms: stack-overflow `runtime.error` on inputs that should be
linear, or a profile that shows huge `frames` peaks.

Checks:

1. Is the recursive call literally the last expression of every branch?
2. Is the call wrapped in `(let ((r (recur …))) r)` instead of being
   the tail expression directly?
3. Is the surrounding form `catch`, `dynamic-wind`, or a non-tail
   `begin`?

Use `etac --disasm file.eta` to confirm the call site emits `TailCall`,
not `Call` — see [Bytecode & Tools](./bytecode-and-tools.md).

---

## Related

- [Control Flow](./control-flow.md)
- [Functions, Closures & Tail Calls](./functions-and-closures.md)
- [`bytecode-vm.md`](./reference/bytecode-vm.md)

