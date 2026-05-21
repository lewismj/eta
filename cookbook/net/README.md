# Net cookbook examples

These examples demonstrate the `eta-http` package and the `(import net.http)`
module.

They are intentionally small, runnable building blocks rather than a full
application. The package-local smoke tests run them against the offline loopback
fixture at:

```text
packages/net/native/http/tests/fixtures/loopback_server.py
```

## Examples

| File | Demonstrates |
| --- | --- |
| `http-quickstart.eta` | Basic GET, POST, custom request headers, explicit sessions, and response inspection. |
| `rest-client.eta` | Reusable sessions, timeout/user-agent options, bearer token option, `get-json`, `post-json`, and `body-json`. |
| `download-large-file.eta` | Streaming a response body to disk with `download`, low-speed protection, status checks, and cleanup. |

## `http-quickstart.eta`

Module:

```scheme
(import cookbook.net.http-quickstart)
```

Entry point:

```scheme
(run-http-quickstart base-url)
```

What it does:

1. Sends `GET <base-url>/`.
2. Sends `POST <base-url>/echo` with the string body `"eta-http"`.
3. Creates an explicit HTTP session.
4. Builds a GET request manually for `<base-url>/headers-echo`.
5. Adds the request header `X-Eta-Test: quickstart`.
6. Performs the request and closes the session.
7. Returns an alist containing response statuses and the echoed header body.

Return shape:

```scheme
((get-status . 200)
 (post-status . 200)
 (header-status . 200)
 (header-body . "quickstart"))
```

This is the best first example if you want to see the difference between the
one-shot helpers (`get`, `post`) and the lower-level request builder
(`make-session`, `make-request`, `request-set-url!`, `request-set-header!`,
`perform`, `close-session!`).

## `rest-client.eta`

Module:

```scheme
(import cookbook.net.rest-client)
```

Entry point:

```scheme
(run-rest-client base-url bearer-token)
```

What it does:

1. Creates a reusable session with:
   - `timeout-ms`
   - `user-agent`
2. Optionally applies a `bearer-token` when a non-empty token is supplied.
3. Calls `get-json` on `<base-url>/json-get`.
4. Sends the decoded value back with `post-json` to `<base-url>/json`.
5. Decodes the POST response with `body-json`.
6. Closes the session.
7. Returns an alist containing the GET value, POST status, and echoed POST value.

Return shape against the loopback fixture:

```scheme
((get-value . 42)
 (post-status . 200)
 (post-value . 42))
```

This example is a compact pattern for API clients:

```scheme
(define session
  (make-session
	'((timeout-ms . 30000)
	  (user-agent . "cookbook-rest-client/0.1"))))

(session-set-option! session 'bearer-token token)
(get-json endpoint session)
(post-json endpoint value session)
(close-session! session)
```

## JSON behavior shown by the cookbook

`rest-client.eta` uses the high-level JSON helpers from `net.http`:

| Helper | Purpose |
| --- | --- |
| `get-json` | Performs GET and decodes the response body using `body-json`. |
| `post-json` | Encodes an Eta value as JSON, sets `Content-Type: application/json`, and POSTs it. |
| `body-json` | Decodes an existing response body with `std.json`. |

For richer JSON objects, use Eta hash maps:

```scheme
(import net.http)

(define payload
  (hash-map
	"symbol" "ETA"
	"quantity" 100
	"active" #t))

(define resp (post-json "https://example.com/orders" payload))
(define decoded (body-json resp))
```

## `download-large-file.eta`

Module:

```scheme
(import cookbook.net.download-large-file)
```

Exports:

```scheme
(run-download-example url out-path)
(cleanup-file! path)
```

What it does:

1. Calls `download` to stream `url` directly to `out-path`.
2. Uses:
   - `timeout-ms`
   - `low-speed-limit-bps`
3. Calls `raise-for-status!` so non-2xx responses fail loudly.
4. Prints the output path and status.
5. Returns the response handle.

Example:

```scheme
(run-download-example
  "https://example.com/archive.bin"
  "archive.bin")
```

Cleanup helper:

```scheme
(cleanup-file! "archive.bin")
```

Use this pattern for large files where you do not want the whole response body
buffered in memory.

## Running against the loopback fixture

The test suite uses a local Python fixture and passes its URL through the
`ETA_HTTP_FIXTURE_BASE_URL` environment variable. The exact command depends on
your local Eta build output, but conceptually the examples expect URLs such as:

```text
http://127.0.0.1:<port>
```

Fixture endpoints exercised by the cookbook/tests include:

| Endpoint | Used by |
| --- | --- |
| `/` | `http-quickstart.eta` basic GET |
| `/echo` | `http-quickstart.eta` POST |
| `/headers-echo` | `http-quickstart.eta` request header echo |
| `/json-get` | `rest-client.eta` JSON GET |
| `/json` | `rest-client.eta` JSON POST roundtrip |
| `/download/1k` | `download-large-file.eta` download example |

## Related package documentation

For the complete HTTP API, including all session/request options, response
helpers, forms, multipart bodies, URL helpers, and native build details, see:

```text
packages/net/native/http/README.md
```
