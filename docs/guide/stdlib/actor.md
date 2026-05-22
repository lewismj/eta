# std.actor

[<- Stdlib Reference](./README.md) | [Language Guide](../../language_guide.md) | [Detailed reference](../../stdlib/actor.md)

Local actor runtime wrappers over `%actor-*` primitives.

```scheme
(import std.actor)
```

Use `std.actor` for local PID and mailbox messaging. Use `std.actor.node`
for distributed actor-node links and `std.net` for transport-level NNG sockets.

| Symbol | Description |
| --- | --- |
| `(self)` | Return the current actor PID. |
| `(pid? x)` | Return `#t` when `x` is a PID value. |
| `(alive? pid)` | Return `#t` when `pid` is still alive in the local actor system. |
| `(spawn thunk)` | Spawn a new actor running zero-argument `thunk`; returns PID. |
| `(send pid payload)` | Send one message to `pid`; returns `payload` on success or `#f`. |
| `(send/checked pid payload)` | Send with delivery status: `'ok`, `'no-process`, `'no-route`, or `'transport-error`. |
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
| `(process-info pid [key])` | Return process info alist or one keyed value (`'pid`, `'state`, `'last-yield-reason`, `'message-queue-len`, `'registered-name`, `'links`, `'monitors`, `'reductions`). `state` is one of `'runnable`, `'running`, `'waiting`, `'exited`. `last-yield-reason` is one of `'none`, `'budget-exhausted`, `'blocked-on-receive`, `'finished`, `'error`. |
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

Trap-exit and monitor message shapes:

```scheme
'(EXIT from-pid reason)
'(DOWN ref process pid reason)
```

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

---

**Source:** [`stdlib/std/actor.eta`](../../../stdlib/std/actor.eta)
