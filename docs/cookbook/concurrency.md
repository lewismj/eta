---
title: "Cookbook — Concurrency"
---

# Concurrency

Erlang-style message passing, worker pools, Monte Carlo parallelism,
scatter-gather, pub/sub, and distributed compute — all via `std.net`.

**Run any example with:**
```bash
etai cookbook/concurrency/<file>.eta
```

Worker processes are launched automatically by `spawn` or `worker-pool`.

---

## Contents

- [Gen-Server Counter](#gen-server-counter)
- [Message Passing](#message-passing)
- [Worker Pool](#worker-pool)
- [Parallel Fibonacci](#parallel-fibonacci)
- [Parallel Map](#parallel-map)
- [Monte Carlo π](#monte-carlo-π)
- [Scatter-Gather](#scatter-gather)
- [Pub / Sub](#pub--sub)
- [In-Process Messaging](#in-process-messaging)
- [Echo Server / Client](#echo-server--client)
- [Distributed Compute](#distributed-compute)

---

## Gen-Server Counter

`cookbook/concurrency/gen-server-counter.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/concurrency/gen-server-counter.eta)

OTP-style counter service built on `std.actor.gen_server`.

```scheme
(module gen-server-counter
  (import std.io std.actor.gen_server)
  (begin
    (define server
      (gen-server-start counter-module 5 '(name counter-server)))

    (println (gen-server-call 'counter-server 'get 1000))
    (gen-server-cast 'counter-server '(inc 4))
    (println (gen-server-call server 'get 1000))
    (println (gen-server-call 'counter-server '(add 10) 1000))
    (gen-server-stop 'counter-server 'shutdown 1000)))
```

---

## Message Passing

`cookbook/concurrency/message-passing.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/concurrency/message-passing.eta)

Erlang-style parent/child messaging via `spawn`, `send!`, and `recv!`.
The child module is `message-passing-worker.eta`.

```scheme
(module message-passing
  (import std.net std.io)
  (begin

    (println "parent: spawning worker...")
    (define worker (spawn "cookbook/concurrency/message-passing-worker.eta"))

    ;; Send a greeting
    (send! worker '(hello from-parent) 'wait)
    (println "parent: sent (hello from-parent)")

    ;; Receive the reply
    (define reply (recv! worker 'wait))
    (println (string-append "parent: received: " (->string reply)))

    ;; Send a compute request
    (send! worker '(compute 21) 'wait)
    (define result (recv! worker 'wait))
    (println (string-append "parent: compute result: " (->string result)))

    ;; Tell the child to exit
    (send! worker '(exit) 'wait)
    (spawn-wait worker)
    (nng-close worker)
    (println "parent: done.")))
```

**Worker** (`message-passing-worker.eta`):

```scheme
(module message-passing-worker
  (import std.net std.io)
  (begin
    (define parent (current-mailbox))

    (let loop ()
      (define msg (recv! parent 'wait))
      (cond
        ((equal? (car msg) 'hello)
          (send! parent '(hello from-worker) 'wait)
          (loop))
        ((equal? (car msg) 'compute)
          (send! parent (* (cadr msg) 2) 'wait)
          (loop))
        ((equal? (car msg) 'exit)
          (println "worker: exiting"))
        (#t (loop))))))
```

---

## Worker Pool

`cookbook/concurrency/worker-pool.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/concurrency/worker-pool.eta)

`worker-pool` spawns one child process per task, sends each its task,
collects results in parallel, and returns them in submission order.

```scheme
(module worker-pool-demo
  (import std.net std.io)
  (begin

    (define tasks '(1 2 3 4 5))

    (println "Distributing tasks to worker pool...")
    (println tasks)

    ;; Each worker receives one task and returns a result
    (define results
      (worker-pool "cookbook/concurrency/worker-pool-worker.eta" tasks))

    (println results)             ; => (2 4 6 8 10)
    (println (equal? results '(2 4 6 8 10)))  ; => #t
  ))
```

**Worker** (`worker-pool-worker.eta`):

```scheme
(module worker-pool-worker
  (import std.net)
  (begin
    (define parent  (current-mailbox))
    (define task    (recv! parent 'wait))
    ;; Double the input and send back
    (send! parent (* task 2) 'wait)))
```

---

## Parallel Fibonacci

`cookbook/concurrency/parallel-fib.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/concurrency/parallel-fib.eta)

Compute several Fibonacci numbers in parallel, each in a separate worker
process, using `worker-pool`.

```scheme
(module parallel-fib
  (import std.net std.io)
  (begin

    ;; Compute fib(n) for each n in the task list in parallel
    (define ns '(30 31 32 33 34 35))

    (define results
      (worker-pool "cookbook/concurrency/parallel-fib-worker.eta" ns))

    (for-each
      (lambda (n r)
        (display "fib(") (display n) (display ") = ") (println r))
      ns results)))
```

---

## Parallel Map

`cookbook/concurrency/parallel-map.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/concurrency/parallel-map.eta)

Generic parallel `map` using `worker-pool`; each element is processed by a
separate worker subprocess.

```scheme
(module parallel-map
  (import std.net std.io)
  (begin

    ;; Square a list of numbers in parallel
    (define xs (list 1 2 3 4 5 6 7 8))

    (define results
      (worker-pool "cookbook/concurrency/parallel-map-worker.eta" xs))

    (println results)    ; => (1 4 9 16 25 36 49 64)
  ))
```

---

## Monte Carlo π

`cookbook/concurrency/monte-carlo.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/concurrency/monte-carlo.eta)

Estimate π in parallel using 8 independent workers. Each worker draws
random samples in the unit square and counts hits inside the unit circle.
The parent averages the per-worker π estimates.

```scheme
(module monte-carlo
  (import std.net std.collections std.math std.io)
  (begin

    (define n-workers          8)
    (define samples-per-worker 50000)

    ;; Task list: (seed samples) per worker — different seeds for independence
    (define tasks
      (map (lambda (i) (list (* i 1000007) samples-per-worker))
           (range 0 n-workers)))

    (println (string-append
      "Estimating pi using " (number->string n-workers)
      " workers x " (number->string samples-per-worker) " samples"))

    (define estimates
      (worker-pool "cookbook/concurrency/monte-carlo-worker.eta" tasks))

    (define pi-est (/ (apply + estimates) n-workers))
    (println (string-append "pi ~ " (number->string pi-est)))
  ))
```

---

## Scatter-Gather

`cookbook/concurrency/scatter-gather.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/concurrency/scatter-gather.eta)

SURVEYOR/RESPONDENT pattern: the surveyor broadcasts a question and
collects all responses that arrive before a deadline. Useful for querying
worker status or load.

```scheme
(module scatter-gather
  (import std.net std.collections std.io)
  (begin

    (define survey-endpoint "tcp://127.0.0.1:5557")
    (define n-workers 3)

    ;; Set up the surveyor socket
    (define surveyor (nng-socket 'surveyor))
    (nng-listen surveyor survey-endpoint)

    ;; Spawn respondent workers
    (define workers
      (map (lambda (_) (spawn "cookbook/concurrency/scatter-gather-worker.eta"))
           (range 0 n-workers)))

    ;; survey: broadcast question, collect all replies within 1 000 ms
    (define replies
      (survey survey-endpoint '(status-query) 1000))

    (println "Responses received:")
    (for-each println replies)

    ;; Clean up
    (for-each (lambda (w) (send! w '(exit) 'wait)) workers)
    (for-each spawn-wait workers)
    (nng-close surveyor)
  ))
```

---

## Pub / Sub

`cookbook/concurrency/pub-sub.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/concurrency/pub-sub.eta)

PUB/SUB pattern: a publisher worker emits messages on named topics;
the parent subscribes and receives only matching messages.

```scheme
(module pub-sub-demo
  (import std.net std.io)
  (begin

    (define pub-endpoint "tcp://127.0.0.1:5556")

    ;; Spawn the publisher worker
    (println "pub-sub: spawning publisher...")
    (define pub-proc (spawn "cookbook/concurrency/pub-sub-publisher.eta"))

    ;; Subscribe to two topics
    (define sub (nng-socket 'sub))
    (nng-dial sub pub-endpoint)
    (nng-subscribe sub "prices")
    (nng-subscribe sub "rates")

    ;; Receive 6 messages
    (let loop ((i 0))
      (when (< i 6)
        (define msg (nng-recv sub 'wait))
        (println msg)
        (loop (+ i 1))))

    ;; Clean up
    (nng-close sub)
    (send! pub-proc '(exit) 'wait)
    (spawn-wait pub-proc)
  ))
```

---

## In-Process Messaging

`cookbook/concurrency/inproc.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/concurrency/inproc.eta)

Intra-process worker threads communicating over `inproc://` sockets —
no OS process boundary crossed, maximum throughput.

```scheme
(module inproc
  (import std.net std.io)
  (begin

    (define endpoint "inproc://worker-channel")

    ;; Spawn in-process worker
    (define worker (spawn "cookbook/concurrency/inproc-worker.eta"))

    ;; Exchange messages
    (send! worker '(ping 1) 'wait)
    (println (recv! worker 'wait))    ; => (pong 1)

    (send! worker '(ping 2) 'wait)
    (println (recv! worker 'wait))    ; => (pong 2)

    (send! worker '(exit) 'wait)
    (spawn-wait worker)
  ))
```

---

## Echo Server / Client

`cookbook/concurrency/echo-server.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/concurrency/echo-server.eta)

A REQ/REP echo server that returns every message unchanged. Shows explicit
socket lifecycle: `nng-socket`, `nng-listen`, `nng-recv`, `nng-send`.

```scheme
;; echo-server.eta
(module echo-server
  (import std.net std.io)
  (begin
    (define endpoint "tcp://127.0.0.1:5555")
    (define rep (nng-socket 'rep))
    (nng-listen rep endpoint)
    (println "echo-server: listening")

    (let loop ()
      (define msg (nng-recv rep 'wait))
      (nng-send rep msg 'wait)
      (unless (equal? msg '(exit))
        (loop)))

    (nng-close rep)
    (println "echo-server: done")))
```

```scheme
;; echo-client.eta
(module echo-client
  (import std.net std.io)
  (begin
    (define endpoint "tcp://127.0.0.1:5555")
    (define req (nng-socket 'req))
    (nng-dial req endpoint)

    (for-each
      (lambda (msg)
        (nng-send req msg 'wait)
        (println (nng-recv req 'wait)))
      '((hello) (world) (exit)))

    (nng-close req)))
```

---

## Distributed Compute

`cookbook/concurrency/distributed-compute.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/concurrency/distributed-compute.eta)

Offload work to a persistent compute server over TCP. The client sends tasks
and receives results; the server runs as a separate process (or on a remote host).

```scheme
;; distributed-compute.eta (client)
(module distributed-compute
  (import std.net std.io)
  (begin

    (define server-endpoint "tcp://127.0.0.1:5560")
    (define req (nng-socket 'req))
    (nng-dial req server-endpoint)

    ;; Submit tasks
    (for-each
      (lambda (task)
        (nng-send req task 'wait)
        (define result (nng-recv req 'wait))
        (display "result: ") (println result))
      '((square 5) (square 12) (factorial 10)))

    (nng-send req '(shutdown) 'wait)
    (nng-close req)))
```

```scheme
;; distributed-compute-server.eta
(module distributed-compute-server
  (import std.net std.io)
  (begin

    (define endpoint "tcp://127.0.0.1:5560")
    (define rep (nng-socket 'rep))
    (nng-listen rep endpoint)
    (println "compute-server: ready")

    (defun factorial (n)
      (if (= n 0) 1 (* n (factorial (- n 1)))))

    (let loop ()
      (define task (nng-recv rep 'wait))
      (cond
        ((equal? (car task) 'square)
          (nng-send rep (* (cadr task) (cadr task)) 'wait)
          (loop))
        ((equal? (car task) 'factorial)
          (nng-send rep (factorial (cadr task)) 'wait)
          (loop))
        ((equal? (car task) 'shutdown)
          (nng-send rep 'ok 'wait))))

    (nng-close rep)
    (println "compute-server: done")))
```

