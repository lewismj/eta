---
title: "Cookbook - Concurrency"
---

# Concurrency

Eta now uses PID-based actors as the primary local concurrency model.
Use `std.actor` (and higher-level `std.actor.gen_server`) for actor workflows.
Use `std.net` only for transport-level socket patterns.

Run any example with:

```bash
etai cookbook/concurrency/<file>.eta
```

## Actor-first examples

- `message-passing.eta`: basic `spawn`, `send`, `receive-after`.
- `worker-pool.eta`: one worker per task, indexed fan-out/fan-in.
- `parallel-map.eta`: parallel square-map with ordered result assembly.
- `parallel-fib.eta`: parallel Fibonacci jobs.
- `monte-carlo.eta`: Monte Carlo pi estimate with actor workers.
- `scatter-gather.eta`: broadcast request + bounded gather timeout.
- `pub-sub.eta`: in-memory broker and topic subscribers.
- `inproc.eta`: local actor workers with tagged replies.

### Message Passing

`cookbook/concurrency/message-passing.eta`

```scheme
(module message-passing
  (import std.actor std.io)
  (begin
    (define worker
      (spawn
        (lambda ()
          (let loop ()
            (receive
              (list
                (match-case (match-list 'hello 1)
                            (lambda (msg)
                              (send (car (cdr msg)) '(hello from-worker))
                              (loop)))
                (match-case (match-list 'compute 2)
                            (lambda (msg)
                              (send (car (cdr msg))
                                    (list 'result
                                          (car (cdr (cdr msg)))
                                          (* (car (cdr (cdr msg))) 2)))
                              (loop)))
                (match-case (match-symbol 'stop)
                            (lambda (msg) 'ok)))
              'wait)))))
    (send worker (list 'hello (self)))
    (println (receive-after 1000))
    (send worker 'stop)))
```

### Worker Pool and Ordered Gather

`worker-pool.eta`, `parallel-map.eta`, and `parallel-fib.eta` all follow this
pattern:

1. Include an index in each request.
2. Collect responses in any order.
3. Place each response into a result vector at its index.
4. Convert the vector back to an ordered list.

This keeps the external result order stable while allowing concurrent execution.

### Scatter-Gather Timeout

`scatter-gather.eta` sends one status request to each responder with a shared
request reference and gathers matching responses until timeout:

```scheme
(define survey-ref 'survey-ref)
(for-each (lambda (pid) (send pid (list 'status (self) survey-ref))) responders)

(define replies (make-vector (length responders) #f))
(let collect ((remaining (length responders)))
  (when (> remaining 0)
    (let ((msg (receive-match (%status-for-ref? survey-ref) 200)))
      (when msg
        (vector-set! replies (car (cdr (cdr msg))) msg)
        (collect (- remaining 1))))))
```

## OTP and distributed actor examples

- `gen-server-counter.eta`: OTP-style request/reply and cast workflow via
  `std.actor.gen_server`.
- `distributed-actors.eta`: node handshake and topology listing via
  `std.actor.node`.

## Transport-level NNG examples

These remain for explicit socket workflows:

- `echo-server.eta` and `echo-client.eta`
- `distributed-compute-server.eta` and `distributed-compute.eta`

These examples are intentionally transport-oriented and use `std.net`/NNG
primitives directly.
