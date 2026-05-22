# std.actor.gen_server

OTP-style server behaviour built on `std.actor`.

```scheme
(import std.actor.gen_server)
```

| Symbol | Description |
| --- | --- |
| `(gen-server-start callbacks init-arg opts)` | Start one server process and return its pid. |
| `(gen-server-start-link callbacks init-arg opts)` | Start one server process and link it to the caller. |
| `(gen-server-call server request timeout-ms)` | Send one synchronous request; return reply value or `#f` on timeout/server-down. |
| `(gen-server-cast server message)` | Send one asynchronous message; returns `'ok` or `#f`. |
| `(gen-server-stop server reason timeout-ms)` | Stop one server with `reason`; waits for stop acknowledgement. |

`callbacks` is an alist containing server callbacks:

- `'init`: `(lambda (init-arg) ...)`  
  Returns `'(ok state)` or `'(stop reason)`.
- `'handle-call`: `(lambda (request from state) ...)`  
  Returns `'(reply value new-state)`, `'(noreply new-state)`, `'(stop reason value new-state)`, or `'(stop reason new-state)`.
- `'handle-cast`: `(lambda (message state) ...)`  
  Returns `'(noreply new-state)` or `'(stop reason new-state)`.
- `'handle-info` (optional): `(lambda (message state) ...)`  
  Handles unmatched mailbox messages; same return contract as `handle-cast`.
- `'terminate` (optional): `(lambda (reason state) ...)`  
  Called before server shutdown initiated by callback return or `gen-server-stop`.

`from` in `handle-call` is the caller pid.

Supported `opts` keys:

- `'name`: register server under symbol/string name.
- `'trap-exit`: enable `trap-exit!` in server process on startup.

Message protocol:

```scheme
'(gen-call ref from request)
'(gen-cast message)
'(gen-reply ref value)
```

Example:

```scheme
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
(gen-server-call 'counter 'get 1000)     ; => 3
(gen-server-stop 'counter 'shutdown 1000) ; => 'ok
```
