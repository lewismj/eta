# Macros — `syntax-rules`

[← Back to Language Guide](../language_guide.md)

Eta's macro system is `define-syntax` + `syntax-rules`: hygienic,
pattern-based, and fully expanded at compile time. There are no
procedural macros; this keeps macro expansion deterministic and
serialisable into `.etac` bytecode.

---

## Anatomy

```scheme
(define-syntax NAME
  (syntax-rules (LITERAL …)
    ((_ pattern …) template)
    …))
```

- `LITERAL`s are matched as themselves (e.g. `else`, `=>`).
- `_` in the first position of a pattern conventionally stands for the
  macro name itself.
- A pattern variable is any identifier in the pattern that is not a
  literal; it binds to whatever sub-form occupies its position and is
  substituted into the template.
- `...` (ellipsis) matches zero or more repetitions of the preceding
  pattern element. The same `...` is then required in the template
  after every variable bound under the pattern's repetition.

---

## A first macro

```scheme
(define-syntax swap!
  (syntax-rules ()
    ((_ a b)
     (let ((tmp a))
       (set! a b)
       (set! b tmp)))))

(define x 1) (define y 2)
(swap! x y)
(list x y)                       ; => (2 1)
```

Hygiene guarantees that the macro's `tmp` cannot collide with a `tmp`
in the caller's scope.

---

## Ellipses

```scheme
(define-syntax my-when
  (syntax-rules ()
    ((_ test body ...)
     (if test (begin body ...) #f))))

(my-when (> 3 0)
  (println "positive")
  (println "ok"))
```

Multiple ellipsis variables are matched in lock-step:

```scheme
(define-syntax my-let
  (syntax-rules ()
    ((_ ((name val) ...) body ...)
     ((lambda (name ...) body ...) val ...))))
```

---

## Literals

Literals are identifiers that must match themselves in the input — they
do **not** bind. Used to build embedded sub-syntax:

```scheme
(define-syntax for
  (syntax-rules (in)
    ((_ x in xs body ...)
     (for-each (lambda (x) body ...) xs))))

(for n in '(1 2 3) (println n))
```

---

## Worked example: `dotimes`

From [`cookbook/quant/portfolio.eta`](../../cookbook/quant/portfolio.eta):

```scheme
(define-syntax dotimes
  (syntax-rules ()
    ((_ (i n) body ...)
     (let loop ((i 0))
       (when (< i n)
         body ...
         (loop (+ i 1)))))))

(dotimes (k 3) (println k))      ; prints 0 1 2
```

The synthetic `loop` introduced by hygiene cannot clash with anything
in the caller.

---

## Recursive expansion

A macro may reference itself in its template; expansion is iterated
until no macro head remains:

```scheme
(define-syntax my-and
  (syntax-rules ()
    ((_)            #t)
    ((_ e)          e)
    ((_ e1 e2 ...)  (if e1 (my-and e2 ...) #f))))
```

---

## Limits and idioms

- No procedural / `syntax-case` macros: code that needs to compute over
  the AST belongs in a build step or in regular runtime code.
- Use `let-syntax` / `letrec-syntax` for macros local to a body.
- For dispatch on shapes that `syntax-rules` cannot pattern-match (e.g.
  arbitrary positional arity), expand to a runtime helper that does the
  rest.

---

## Debugging expansions

`etac --dump-expand file.eta` prints the fully-expanded source before
compilation; useful for confirming hygiene and ellipsis behaviour. See
[Bytecode & Tools](./bytecode-and-tools.md).

---

## Related

- [Functions, Closures & Tail Calls](./functions-and-closures.md)
- [Modules](./reference/modules.md)
- [`compiler.md`](./reference/compiler.md)


