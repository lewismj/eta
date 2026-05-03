# std.db

Datalog/Prolog-style relations layered on top of fact tables, with optional
SLG-style tabling for recursive rules.

```scheme
(import std.db)
```

## Relation definition

| Symbol | Description |
| --- | --- |
| `(defrel name arity)` | Declare a relation. |
| `(defrel-clause name (head ...) body ...)` | Add a rule clause. |
| `(tabled name arity)` | Declare a tabled (memoising) relation. |
| `(tabled-clause name (head ...) body ...)` | Add a clause to a tabled relation. |

## Facts

| Symbol | Description |
| --- | --- |
| `(assert-fact! name args)` | Insert a ground fact. |
| `(retract-fact! name args)` | Remove a ground fact. |
| `(retract-all name)` | Remove all facts and rules for `name`. |
| `(index-rel! name cols)` | Build an index on the named relation. |

## Querying

| Symbol | Description |
| --- | --- |
| `(call-rel name args)` | Goal that succeeds for each matching tuple. |
| `(call-rel? name args)` | True when at least one tuple matches. |

## Deprecated

| Symbol | Replacement |
| --- | --- |
| `assert` | `assert-fact!`. |
| `retract` | `retract-fact!`. |

