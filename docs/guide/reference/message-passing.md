# Message Passing & Actors

[← Back to README](../../../README.md) · [Actor stdlib](../stdlib/actor.md) ·
[Actor supervision](../stdlib/actor-supervisor.md) · [Gen server](../stdlib/actor-gen-server.md) ·
[Actor nodes](../stdlib/actor-node.md) · [Networking Primitives](networking.md) ·
[Examples](../examples-tour.md)

---

Eta's primary concurrency model is now a **BEAM-like actor runtime**:
actors are addressed by PIDs, each actor owns a VM-level mailbox, and
failures are represented as ordinary data through exits, links, monitors,
and supervisor restart policies.

NNG is still part of the stack, but it is no longer the local actor mailbox
abstraction. Use:

| Layer | Use it for |
| --- | --- |
| `std.actor` | Local PID/mailbox actors, selective receive, links, monitors, exits, registry, introspection. |
| `std.actor.supervisor` | OTP-style child specs, `one-for-one`, `one-for-all`, `rest-for-one`, restart intensity. |
| `std.actor.gen_server` | OTP-style server behaviour with `call`, `cast`, callback state, and termination hooks. |
| `std.actor.node` | Distributed node handshakes, remote actor routing, node monitors, and remote process monitor notifications over NNG. |
| `std.net` | Explicit NNG socket workflows: REQ/REP, PUB/SUB, survey, raw endpoints, and legacy socket-mailbox compatibility. |

## Actor model overview

A local actor is a lightweight runtime process with:

- an opaque PID returned by `(spawn thunk)`;
- a mailbox owned by the runtime, not an NNG socket;
- selective receive that scans for the first matching message and leaves
  unmatched messages queued in order;
- BEAM-style failure signals: `link`, `unlink`, `monitor`, `demonitor`,
  `exit`, `kill`, and `trap-exit!`;
- local name registration through `register`, `whereis`, and `registered`;
- process introspection through `process-info` and `mailbox-length`.

```scheme
(import std.actor std.io)

(define worker
  (spawn
    (lambda ()
      (let loop ()
        (receive
          (list
            (match-case (match-list 'ping 1)
                        (lambda (msg)
                          (send (car (cdr msg)) 'pong)
                          (loop)))
            (match-case (match-list 'double 2)
                        (lambda (msg)
                          (let ((reply-to (car (cdr msg)))
                                (n (car (cdr (cdr msg)))))
                            (send reply-to (list 'result (* n 2)))
                            (loop))))
            (match-case (match-symbol 'stop)
                        (lambda (msg) 'ok)))
          'wait)))))

(send worker (list 'ping (self)))
(println (receive-after 1000))        ; => pong

(send worker (list 'double (self) 21))
(println (receive-after 1000))        ; => (result 42)

(send worker 'stop)
```

## Selective receive

`receive` takes a list of `(match-case matcher handler)` clauses. The runtime
returns the first queued message matching any clause. Messages that do not
match stay in the mailbox for later receives.

```scheme
(receive
  (list
    (match-case (match-list 'ready 1)
                (lambda (msg) (car (cdr msg))))
    (match-case (match-predicate number?)
                (lambda (n) (* n 2))))
  1000
  (lambda () 'timeout))
```

Helpers in `std.actor`:

| Helper | Purpose |
| --- | --- |
| `match-case` | Pair one matcher with one handler. |
| `match-list` | Match list messages by head value and optional tail arity. |
| `match-symbol` | Match one symbol by `eq?`. |
| `match-predicate` | Lift any predicate into a receive matcher. |
| `receive-match` | Receive by one predicate or a clause list. |
| `receive-after` | Receive one message with a timeout; returns `#f` on timeout. |

## Failure semantics

Actor failures follow BEAM-style conventions, expressed in Eta data:

```scheme
(import std.actor)

(trap-exit! #t)

(define child
  (spawn
    (lambda ()
      (receive-after 'wait))))

(link child)
(exit child '(error boom))

(receive
  (list
    (match-case (match-list 'EXIT 2)
                (lambda (msg) msg)))
  1000
  (lambda () 'timeout))
;; => (EXIT #<pid ...> (error boom))
```

Monitors are one-way and deliver exactly one `DOWN`-shaped notification for
that monitor reference:

```scheme
(define doomed (spawn (lambda () 'done)))
(define ref (monitor doomed))

(receive
  (list
    (match-case (match-list 'DOWN 4)
                (lambda (msg) msg)))
  1000
  (lambda () 'timeout))
;; => (DOWN ref process #<pid ...> normal)
```

Use `demonitor` with `#t` to remove a monitor and flush any queued stale
notification for that reference.

## Supervision

`std.actor.supervisor` packages the usual "let it crash" pattern with child
specs, restart policies, and restart intensity gates.

```scheme
(import std.actor.supervisor std.actor)

(define specs
  (list
    (child-spec 'worker
                (lambda ()
                  (receive-after 'wait))
                'restart 'permanent
                'shutdown 5000
                'type 'worker)))

(define sup (one-for-one specs 'max-restarts 3 'max-seconds 5))
(supervisor-which-children sup)
(supervisor-count-children sup)
```

Supported strategies:

| Strategy | Behaviour |
| --- | --- |
| `one-for-one` | Restart only the failed child. |
| `one-for-all` | Stop and restart every child when one fails. |
| `rest-for-one` | Stop and restart the failed child and children started after it. |

`std.supervisor` remains as a compatibility shim that re-exports
`std.actor.supervisor`; new code should import `std.actor.supervisor`.

## Gen server behaviour

`std.actor.gen_server` provides an OTP-style server loop with callback state,
synchronous calls, asynchronous casts, and controlled stop/terminate handling.

```scheme
(import std.actor.gen_server)

(define callbacks
  (list
    (cons 'init (lambda (initial) (list 'ok initial)))
    (cons 'handle-call
          (lambda (request from state)
            (if (eq? request 'get)
                (list 'reply state state)
                (list 'reply 'unknown state))))
    (cons 'handle-cast
          (lambda (message state)
            (if (and (pair? message) (eq? (car message) 'inc))
                (list 'noreply (+ state (car (cdr message))))
                (list 'noreply state))))))

(define server (gen-server-start callbacks 0 '(name counter)))
(gen-server-cast 'counter '(inc 3))
(gen-server-call 'counter 'get 1000)      ; => 3
(gen-server-stop 'counter 'shutdown 1000) ; => ok
```

## Distributed actor nodes

`std.actor.node` uses NNG as the node-to-node transport bridge. It manages
node identity, cookie handshakes, connected-node listing, node monitors,
remote actor routing, and netsplit-style monitor notifications.

```scheme
(import std.actor.node)

(node-listen "tcp://127.0.0.1:7010" 'node-name 'alpha 'cookie 'secret)
(node-connect "tcp://127.0.0.1:7010" 'node-name 'beta 'cookie 'secret)
(nodes)
```

Node monitor messages use these shapes:

```scheme
'(nodeup ref node-name node-id)
'(nodedown ref node-name reason)
```

Remote process monitors use the same `DOWN` shape as local monitors. Node
loss reports remote process `DOWN` with reason `'noconnection`; rejected
cookie handshakes report node monitor `nodedown` with reason `'bad-cookie`.

## Runtime scheduler controls

The actor scheduler defaults to the pool scheduler. Runtime controls:

| Variable | Meaning |
| --- | --- |
| `ETA_ACTOR_SCHEDULER=thread-per-actor|pool|pool-shadow` | Select scheduler mode. |
| `ETA_ACTOR_REDUCTION_BUDGET=<int>` | Reduction budget before yielding; default `2000`. |
| `ETA_ACTOR_DIRTY_SCHEDULERS=<int>` | Dirty scheduler count for blocking native work; default `0`. |

## Where `std.net` fits

Use `std.net` when you explicitly want sockets and endpoint-level protocols:

- `nng-socket`, `nng-listen`, `nng-dial`, `nng-close`;
- `send!`, `recv!`, `nng-poll`, `nng-subscribe`, `nng-set-option`;
- transport patterns such as `request-reply`, `pub-sub`, `survey`;
- compatibility helpers such as socket-based child workers.

Do not use `std.net` as the primary local actor API. For actor code, prefer
`std.actor` and send to PIDs or registered names.

## See also

- [`std.actor`](../stdlib/actor.md)
- [`std.actor.supervisor`](../stdlib/actor-supervisor.md)
- [`std.actor.gen_server`](../stdlib/actor-gen-server.md)
- [`std.actor.node`](../stdlib/actor-node.md)
- [`std.net`](../stdlib/net.md)
- [Concurrency cookbook](../../cookbook/concurrency.md)
