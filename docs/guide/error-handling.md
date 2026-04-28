# Error Handling

[← Back to Language Guide](./language_guide.md)

Eta's exception model is built on two special forms: `raise` and
`catch`. They compile directly to the `Throw` and `SetupCatch` opcodes
on the VM and integrate cleanly with `dynamic-wind` and the logic-trail
unwinder.

---

## Forms

| Form                       | Meaning                                                                                  |
| :------------------------- | :--------------------------------------------------------------------------------------- |
| `(raise tag value)`        | Unwind the stack to the nearest matching `catch`.                                        |
| `(catch tag body …)`       | Evaluate `body`. If a `raise` with the same `tag` escapes, return its payload.           |
| `(catch body …)`           | Catch-all. Intercepts any `raise` *and* any `runtime.*` error.                           |
| `(dynamic-wind before body after)` | Run `before`, then `body`, then `after` — `after` runs even on exception unwind. |

Tags are ordinary symbols. The raised value can be any Eta value —
number, string, list, record.

---

## Basics

```scheme
(catch 'err 42)                            ; => 42  (no raise)
(catch 'err (raise 'err "oops"))           ; => "oops"
(catch 'math (raise 'math 404))            ; => 404
(catch       (raise 'anything "caught"))   ; => "caught"   ; catch-all
```

---

## Runtime errors

Built-in failures are tagged in the `runtime.*` namespace:

| Tag                        | Trigger                                  |
| :------------------------- | :--------------------------------------- |
| `runtime.type-error`       | Wrong argument type                      |
| `runtime.invalid-arity`    | Wrong number of arguments                |
| `runtime.unbound`          | Reference to an unbound name             |
| `runtime.div-by-zero`      | Integer division by zero                 |
| `runtime.index-out-of-range` | Vector / string index out of bounds    |
| `runtime.error`            | Any runtime error (parent tag)           |

```scheme
(catch 'runtime.type-error (car 42))
;; => (runtime-error runtime.type-error "car: argument must be a pair" <span> <stack>)

(catch (car 42))                ; tag-less catch-all also intercepts it
```

The payload of a runtime error is a tuple:

```scheme
(runtime-error <tag> <message-string> <span-record> <stack-trace>)

<span-record> ::= (span <file-id> <start-line> <start-col> <end-line> <end-col>)
<stack-trace> ::= ((frame <function-name> <span-record>) …)
```

---

## Tag specificity

Handlers are matched **inside out**. Inner handlers fire first when
their tag matches; otherwise the raise propagates outward.

```scheme
(catch 'outer
  (+ 10 (catch 'inner
          (raise 'inner 5))))           ; => 15

(catch
  (catch 'wrong-tag
    (raise 'real-tag "bypassed")))      ; => "bypassed"
```

---

## Structured payloads

Use any data shape; pairs of `(code . detail)` are a common idiom:

```scheme
(defun validate-age (n)
  (cond
    ((< n 0)   (raise 'validation (cons 'negative n)))
    ((> n 150) (raise 'validation (cons 'too-large n)))
    (else      n)))

(catch 'validation (validate-age -3))   ; => (negative . -3)
```

---

## Early exit

`raise` is a fast non-local exit — useful for short-circuiting deep
recursion:

```scheme
(defun first-negative (xs)
  (catch 'found
    (let loop ((xs xs))
      (cond
        ((null? xs)     #f)
        ((< (car xs) 0) (raise 'found (car xs)))
        (else           (loop (cdr xs)))))))

(first-negative '(3 1 -4 2))            ; => -4
```

---

## Resource cleanup

`dynamic-wind` guarantees the *after* thunk runs even when an exception
escapes the body:

```scheme
(define cleanup-ran #f)

(catch 'boom
  (dynamic-wind
    (lambda () (set! cleanup-ran #f))
    (lambda () (raise 'boom "ow"))
    (lambda () (set! cleanup-ran #t))))

cleanup-ran                              ; => #t
```

---

## Re-raising

An inner handler can wrap and re-raise:

```scheme
(defun wrap (thunk)
  (catch 'low
    (let ((v (thunk)))
      ;; v holds the intercepted low-level payload; wrap it
      (raise 'high (list 'wrapped v)))))
```

---

## Implementation notes

- `catch` pushes a frame on the **catch stack** containing the tag,
  the handler PC, and a snapshot of the value-stack height.
- `raise` walks the catch stack to the nearest matching tag, restores
  the snapshot, and resumes at the handler PC.
- The catch stack is **separate from the logic trail**, so backtracking
  inside a `catch` body cannot accidentally discard a live exception
  handler.

See [`bytecode-vm.md`](./reference/bytecode-vm.md) for the opcodes.

---

## Related

- [Control Flow](./control-flow.md)
- [Logic Programming](./reference/logic.md)
- [`bytecode-vm.md`](./reference/bytecode-vm.md)

