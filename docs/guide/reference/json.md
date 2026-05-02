# JSON

[Back to README](../../../README.md) ·
[Modules and Stdlib](modules.md) ·
[Language Guide](../../language_guide.md) ·
[CSV](csv.md) ·
[Hash Maps](hashmap.md) ·
[Next Steps](../../next-steps.md)

---

## Quick Start

```scheme
(module demo
  (import std.json std.io)
  (begin
    ;; Parse from a string
    (let ((cfg (json:read-string
                "{\"name\":\"eta\",\"n\":7,\"flags\":[true,false]}"
                'keep-integers-exact? #t)))
      (println (hash-map-ref cfg "name"))           ;; "eta"
      (println (hash-map-ref cfg "n"))              ;; 7
      (println (vector-ref (hash-map-ref cfg "flags") 0))) ;; #t

    ;; Serialise back
    (println (json:write-string
              (hash-map "ok" #t "xs" #(1 2 3))))
    ;; => {"ok":true,"xs":[1,2,3]}

    ;; Stream from a port
    (let ((p (open-input-string "{\"msg\":\"ok\"}")))
      (println (hash-map-ref (json:read p) "msg")))))
```

`std.json` is a thin Eta layer over the native JSON codec implemented
in [`eta/core/src/eta/util/json.h`](../../../eta/core/src/eta/util/json.h) —
a hand-written, RFC 8259 compliant parser and serialiser with no
third-party dependency. It is auto-imported by `std.prelude`; direct
import works when you want a smaller surface:

```scheme
(import std.json)
```

---

## API

| Function              | Signature                                  | Notes                                                  |
| :-------------------- | :----------------------------------------- | :----------------------------------------------------- |
| `json:read`           | `(port [opts ...]) -> value`               | Reads a single JSON document from any input port.      |
| `json:read-string`    | `(string [opts ...]) -> value`             | Convenience for in-memory text.                        |
| `json:write`          | `(value [port]) -> '()`                    | Writes JSON to `port` (defaults to `(current-output-port)`). |
| `json:write-string`   | `(value) -> string`                        | Returns the serialised form as a string.               |

### Options

Reader options are alternating keyword/value pairs at the end of the
argument list. Both symbol (`'keep-integers-exact?`) and keyword
(`:keep-integers-exact?`) spellings are accepted.

| Option                    | Type | Default | Effect                                                                      |
| :------------------------ | :--- | :------ | :-------------------------------------------------------------------------- |
| `keep-integers-exact?`    | bool | `#f`    | When `#t`, integer-typed JSON numbers decode to fixnums; floats stay flonum. The default decodes all numbers to flonums for predictable arithmetic. |

---

## Type Mapping

| JSON         | Eta                              |
| :----------- | :------------------------------- |
| `object`     | hash map (string keys)           |
| `array`      | vector                           |
| `string`     | string                           |
| `number` (int) | flonum (or fixnum with `keep-integers-exact?`) |
| `number` (float) | flonum                       |
| `true` / `false` | `#t` / `#f`                  |
| `null`       | `'()` (empty list)               |

The same mapping is used in reverse by `json:write` / `json:write-string`,
with these additional conventions:

- Hash-map keys are coerced to strings via `display`. Use string keys
  (or symbols whose printed form is the desired key) for stable
  serialisation.
- Pairs / proper lists serialise as JSON arrays, mirroring vectors.
- `'()` (the empty list) serialises as `null`.

---

## Patterns

### Loading config from disk

```scheme
(import std.json std.io)
(defun load-config (path)
  (let ((p (open-input-file path)))
    (let ((cfg (json:read p 'keep-integers-exact? #t)))
      (close-port p)
      cfg)))
```

### Round-tripping a hash map

```scheme
(let* ((src  (hash-map "name" "eta" "count" 2))
       (text (json:write-string src))
       (dst  (json:read-string text 'keep-integers-exact? #t)))
  (assert-equal "eta" (hash-map-ref dst "name"))
  (assert-equal 2     (hash-map-ref dst "count")))
```

### Pretty-printed output

`json:write` writes the compact RFC 8259 form. Wrap the value in your
own formatter, or pass through a re-decode + manual rendering loop, if
human-readable indentation is required.

---

## Notes

- Reader and writer are RFC 8259 compliant; non-conforming inputs
  raise a runtime error catchable with `(catch …)`.
- The codec is single-document: `json:read` consumes one top-level
  value per call. Stream-style "newline-delimited JSON" is a small
  loop on top (`(let loop () (let ((v (json:read p))) … (loop)))`).
- Hash maps used by `json:read` are the same `HashMap` runtime kind
  documented in [`hashmap.md`](hashmap.md), so all `hash-map-*`
  builtins apply to decoded objects without conversion.
- For tabular ingest prefer [`std.csv`](csv.md); for record-shaped
  config / API payloads prefer `std.json`.


