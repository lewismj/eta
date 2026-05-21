# eta-http

HTTP client support for Eta, backed by a native libcurl sidecar.

`eta-http` provides the `(import net.http)` module for making HTTP requests,
working with sessions, handling JSON, setting headers/options, streaming
downloads, and using URL helpers.

```scheme
(import net.http)

(define resp (get "https://example.com"))
(status resp)       ; => 200
(body-string resp)  ; response body as text
```

## Module

```scheme
(import net.http)
```

The public Eta API lives in `src/net/http.eta`. The low-level native primitives
are registered by the sidecar as `http/*` primitives, but normal user code should
prefer the `net.http` wrappers documented here.

## Quick start

```scheme
(import net.http)

(let ((resp (get "https://example.com" '((timeout-ms . 10000)))))
  (raise-for-status! resp)
  (display (status resp))
  (newline)
  (display (body-string resp))
  (newline))
```

One-shot helpers create and close a temporary session automatically unless you
pass an existing session.

## One-shot requests

| Function | Description |
| --- | --- |
| `(get url [opts-or-session] [session])` | Execute a GET request and return a response handle. |
| `(post url body [opts-or-session] [session])` | Execute a POST request with a UTF-8 string body. |
| `(post-json url value [opts-or-session] [session])` | JSON-encode `value`, POST it, and set `Content-Type: application/json`. |
| `(get-json url [opts-or-session] [session])` | GET a URL and decode the response body as JSON. |
| `(download url path [opts])` | Stream the response body directly to `path`; returns a response handle. |

Examples:

```scheme
(import net.http)

(define resp (get "https://example.com/api/status"
                  '((timeout-ms . 5000)
                    (user-agent . "eta-client/1.0"))))

(status resp)
(headers resp)
(body-string resp)
```

```scheme
(import net.http)

(define created
  (post "https://example.com/api/items"
        "{\"name\":\"eta\"}"
        '((timeout-ms . 10000))))

(raise-for-status! created)
```

## JSON

`net.http` imports `std.json` internally and exposes JSON convenience helpers:

| Function | Description |
| --- | --- |
| `(post-json url value ...)` | Encodes `value` with `json:write-string` and sends it as UTF-8 JSON. |
| `(get-json url ...)` | Performs GET and returns `(body-json response)`. |
| `(body-json response)` | Decodes response text with `json:read-string`. |

Example POST/GET JSON flow:

```scheme
(import net.http)

(define payload
  (hash-map
    "name" "eta"
    "count" 2
    "enabled" #t))

(define resp
  (post-json "https://example.com/api/items"
             payload
             '((timeout-ms . 10000))))

(raise-for-status! resp)

(define decoded (body-json resp))
```

Eta JSON values follow `std.json` conventions:

- JSON objects decode to hash maps.
- JSON arrays decode to vectors.
- JSON booleans decode to `#t` / `#f`.
- JSON `null` decodes to `'()`.
- Numbers decode using the `std.json` defaults.

If you need exact integer preservation for manual decoding, use `std.json`
directly:

```scheme
(import std.json)

(json:read-string "{\"id\":1}" 'keep-integers-exact? #t)
```

## Sessions

Sessions hold default options and shared libcurl state such as DNS/cache/cookie
sharing. Use a session when making multiple requests to the same service.

| Function | Description |
| --- | --- |
| `(make-session [opts])` | Create a session and optionally apply default options. |
| `(session? value)` | Return whether `value` is a live HTTP session. |
| `(close-session! session)` | Close a session and release native resources. |
| `(session-set-option! session name value)` | Set a session default option. |
| `(session-get-option session name)` | Read a session option. |

Example:

```scheme
(import net.http)

(define session
  (make-session
    '((timeout-ms . 30000)
      (connect-timeout-ms . 5000)
      (user-agent . "my-service/1.0")
      (bearer-token . "TOKEN"))))

(define user-json
  (get-json "https://example.com/api/me" session))

(define post-resp
  (post-json "https://example.com/api/events"
             (hash-map "event" "started")
             session))

(close-session! session)
```

The one-shot helpers accept either:

```scheme
(get url)
(get url opts)
(get url session)
(get url opts session)
```

The same pattern applies to `post`, `post-json`, and `get-json`.

## Request builder API

Use the builder API when you need full control over method, headers, bodies, or
per-request overrides.

| Function | Description |
| --- | --- |
| `(make-request session method)` | Create a request bound to `session`; `method` is normally a symbol such as `'get`, `'post`, `'put`, `'delete`, `'head`, etc. |
| `(request? value)` | Return whether `value` is a request handle. |
| `(request-set-url! request url)` | Set the request URL. |
| `(request-set-header! request name value-or-false)` | Add a header; pass `#f` to remove existing headers with that name. |
| `(request-set-option! request name value)` | Override an option for this request only. |
| `(request-set-body! request string)` | Set a UTF-8 string body. |
| `(request-set-body-string! request string charset)` | Set a string body and a text content type using `charset`. |
| `(request-set-body-bytes! request bytevector)` | Set a raw bytevector body. |
| `(request-set-body-file! request path)` | Load the request body from a file path. |
| `(request-set-form! request fields)` | Send `application/x-www-form-urlencoded` form fields. |
| `(request-set-multipart! request parts)` | Send multipart form data. |
| `(perform request)` | Execute and buffer the response body in memory. |
| `(perform-stream request)` | Execute through the streaming transfer loop; currently returns a response handle with captured body. |

Example with custom headers:

```scheme
(import net.http)

(define session (make-session '((timeout-ms . 10000))))
(define req (make-request session 'put))

(request-set-url! req "https://example.com/api/items/123")
(request-set-header! req "Accept" "application/json")
(request-set-header! req "Content-Type" "application/json")
(request-set-body! req "{\"name\":\"updated\"}")

(define resp (perform req))
(raise-for-status! resp)
(close-session! session)
```

## Forms and multipart

URL-encoded form fields are alists of `(key . value)` pairs. Keys and values may
be symbols or strings.

```scheme
(import net.http)

(define session (make-session))
(define req (make-request session 'post))

(request-set-url! req "https://example.com/login")
(request-set-form! req
  '((username . "demo")
    (password . "secret")))

(define resp (perform req))
(close-session! session)
```

Multipart parts are lists with 2 to 4 fields:

```scheme
(name data)
(name filename data)
(name filename content-type data)
```

`data` may be a string or bytevector.

```scheme
(import net.http)

(define session (make-session))
(define req (make-request session 'post))

(request-set-url! req "https://example.com/upload")
(request-set-multipart! req
  '(("description" "Eta upload")
    ("file" "hello.txt" "text/plain" "hello from Eta\n")))

(define resp (perform req))
(close-session! session)
```

## Responses

| Function | Description |
| --- | --- |
| `(response? value)` | Return whether `value` is a response handle. |
| `(status response)` | Return the HTTP status code. |
| `(ok? response)` | Return true for 2xx status codes. |
| `(raise-for-status! response)` | Return `response` for 2xx; raise an error otherwise. |
| `(body-bytes response)` | Return the response body as a bytevector. |
| `(body-string response)` | Decode response body bytes as a bytewise string. |
| `(body-json response)` | Decode response body as JSON. |
| `(headers response)` | Return response headers as `(name . value)` pairs. |
| `(header response name)` | Return the first matching header value, or `#f`. |
| `(effective-url response)` | Return the final URL after redirects. |

Example:

```scheme
(import net.http)

(define resp (get "https://example.com"))

(if (ok? resp)
    (begin
      (display (header resp "Content-Type"))
      (newline)
      (display (body-string resp)))
    (raise-for-status! resp))
```

## Options

Options are supplied as an alist:

```scheme
'((timeout-ms . 10000)
  (follow-redirects . #t)
  (user-agent . "eta-client/1.0"))
```

Option names may be symbols or strings. They are case-normalized internally.
Values must be booleans, non-negative integers, symbols, or strings depending on
the option.

### Boolean options

| Option | Default | Description |
| --- | ---: | --- |
| `follow-redirects` | `#t` | Follow redirects when `max-redirects` is greater than zero. |
| `verify-tls` | `#t` | Verify TLS peer and host. |
| `verbose` | `#f` | Enable libcurl verbose output. |

### Integer options

| Option | Default | Description |
| --- | ---: | --- |
| `max-redirects` | `10` | Maximum redirects to follow. |
| `connect-timeout-ms` | `30000` | Connection timeout in milliseconds. |
| `timeout-ms` | `0` | Total transfer timeout in milliseconds. `0` means no total timeout. |
| `low-speed-limit-bps` | `0` | Low-speed threshold in bytes per second. |
| `low-speed-time-s` | `0` | Seconds below the low-speed threshold before aborting. |

### String/path options

| Option | Description |
| --- | --- |
| `user-agent` | User-Agent header value. |
| `accept-encoding` | Accepted response encodings; empty string lets libcurl request supported encodings. |
| `ca-bundle` | Path to a CA bundle file. |
| `ca-path` | Path to a CA certificate directory. |
| `client-cert` | Path to client certificate. |
| `client-key` | Path to client private key. |
| `username` | Username for authentication. |
| `password` | Password for authentication. |
| `bearer-token` | OAuth2 bearer token used by libcurl. |
| `proxy` | Proxy URL. |
| `cookie-jar` | Path where cookies are written. |
| `cookie-file` | Cookie file to read; empty uses in-memory cookies. |
| `unix-socket-path` | Unix domain socket path for HTTP-over-UDS. |

### Symbol/string option

| Option | Values | Description |
| --- | --- | --- |
| `http-version` | `'any`, `'http/1.1`, `'http/2`, `'http/2-tls` | Preferred HTTP version. The native sidecar falls back when libcurl lacks HTTP/2 support. |

Example with session defaults and a request override:

```scheme
(import net.http)

(define session
  (make-session
    '((timeout-ms . 30000)
      (user-agent . "eta-client/1.0")
      (http-version . http/2-tls))))

(define req (make-request session 'get))
(request-set-url! req "https://example.com/slow")
(request-set-option! req 'timeout-ms 5000)

(define resp (perform req))
(close-session! session)
```

## Downloads

`download` streams the response body directly to a file instead of keeping the
payload in memory.

```scheme
(import net.http)

(define resp
  (download "https://example.com/archive.zip"
            "archive.zip"
            '((timeout-ms . 0)
              (low-speed-limit-bps . 10000)
              (low-speed-time-s . 30))))

(raise-for-status! resp)
(status resp)
```

The returned response contains status, headers, and effective URL. The body bytes
are written to the requested path.

## URL helpers

| Function | Description |
| --- | --- |
| `(url-encode text)` | Percent-encode URL text. |
| `(url-decode text)` | Decode percent-encoded URL text. |
| `(url-parse text)` | Parse URL text into a component alist. |
| `(url-build components)` | Build a URL from a component alist. |

```scheme
(import net.http)

(define encoded (url-encode "alpha beta/%eta?"))
(define decoded (url-decode encoded))
```

## Version information

```scheme
(import net.http)

(http-version)
; => (libcurl-version protocols features)
```

The result is a three-item list containing the libcurl version string, supported
protocol strings, and feature symbols such as `ssl`, `http2`, `brotli`, `zstd`,
or `ipv6` when available.

## Error handling

Network failures, invalid options, malformed URLs, and non-2xx status checks
raise runtime errors through the Eta wrappers.

`raise-for-status!` is explicit: `get`, `post`, and `post-json` return responses
for all HTTP status codes unless libcurl itself fails.

```scheme
(import net.http)

(define resp (get "https://example.com/missing"))

(if (ok? resp)
    (body-string resp)
    (raise-for-status! resp))
```

## Native sidecar

`eta-http` is loaded automatically when `(import net.http)` is evaluated,
provided `ETA_MODULE_PATH` includes the package directory.

Host sidecar artifacts are staged under `libs/<arch>/`:

| Platform | Artifact |
| --- | --- |
| Windows x64 | `libs/amd64/eta_http.dll` |
| Linux x64 | `libs/amd64/libeta_http.so` |
| Linux arm64 | `libs/arm64/libeta_http.so` |
| macOS x64 | `libs/amd64/libeta_http.dylib` |
| macOS arm64 | `libs/arm64/libeta_http.dylib` |

## Build and test

From the repository root, one local Windows/MSVC build shape is:

```powershell
cmake -S packages/net/native/http -B out/http-msvc `
  -DETA_CORE_LIBRARY="C:/Users/lewis/develop/eta/out/msvc-release/eta/core/eta_core.lib" `
  -DETA_ETAI_EXECUTABLE="C:/Users/lewis/develop/eta/out/msvc-release/eta/tools/interpreter/etai.exe" `
  -DETA_STDLIB_DIR="C:/Users/lewis/develop/eta/stdlib"
cmake --build out/http-msvc --config Release
ctest --test-dir out/http-msvc --output-on-failure
```

Package-local smoke tests run against the loopback fixture in
`tests/fixtures/loopback_server.py` and exercise sessions, GET/POST, JSON,
downloads, status handling, URL helpers, and the cookbook examples.
