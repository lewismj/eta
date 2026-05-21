# std.actor

Local actor runtime wrappers over the `%actor-*` primitives.

```scheme
(import std.actor)
```

`std.actor` is the local PID and mailbox API. Use `std.net` for transport-level
NNG socket workflows.

| Symbol | Description |
| --- | --- |
| `(self)` | Return the current actor PID. |
| `(pid? x)` | Return `#t` when `x` is a PID value. |
| `(alive? pid)` | Return `#t` when `pid` is still alive in the local actor system. |
| `(spawn thunk)` | Spawn a new actor running zero-argument `thunk`; returns PID. |
| `(send pid payload)` | Send one message to `pid`; returns `payload` on success or `#f`. |
| `(! pid payload)` | Alias for `send`. |
| `(trap-exit! enabled?)` | Enable or disable trap-exit mode for the current actor. |
| `(link pid)` | Link the current actor with `pid`. |
| `(unlink pid)` | Remove a link to `pid`. |
| `(monitor pid)` | Monitor `pid`; returns a monitor reference. |
| `(demonitor ref [flush?])` | Remove monitor `ref`; optional `flush?` removes queued `DOWN` for that ref. |
| `(exit pid reason)` | Send an exit signal with `reason` to `pid`. |
| `(kill pid)` | Send an untrappable kill signal to `pid`. |
| `(register name pid)` | Register local `name` to resolve to `pid`. |
| `(unregister name)` | Remove one local registration. |
| `(whereis name)` | Resolve local registered `name` to PID or `#f`. |
| `(registered)` | Return all registered local names. |
| `(match-case matcher handler)` | Build one receive clause for `receive-match`. |
| `(match-list head [arity])` | Match list messages by head value and optional tail arity. |
| `(match-symbol sym)` | Match one symbol message by `eq?`. |
| `(match-predicate pred)` | Lift predicate `pred` into a receive matcher. |
| `(receive cases [timeout-ms [timeout-thunk]])` | Selective receive with a clause list and optional timeout handler thunk. |
| `(receive-match matcher-or-cases . rest)` | Receive by predicate or clause list; optional timeout and timeout thunk. |
| `(receive-after timeout-ms)` | Receive one message with timeout; returns `#f` on timeout. |
| `(mailbox-length)` | Return queued message count for the current actor. |

`case` is a language keyword in Eta, so the clause constructor is named
`match-case`.

When trap-exit is enabled, linked exits arrive as:

```scheme
'(EXIT from-pid reason)
```

Monitor notifications arrive as:

```scheme
'(DOWN ref process pid reason)
```

Example selective receive:

```scheme
(receive
  (list
    (match-case (match-list 'ping 1)
                (lambda (msg) (send (car (cdr msg)) 'pong)))
    (match-case (match-predicate number?)
                (lambda (n) (* n 2))))
  1000
  (lambda () 'timeout))
```
