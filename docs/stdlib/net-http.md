# net.http

HTTP/HTTPS client helpers backed by the `eta-http` native sidecar package
(`packages/net/native/http`) and libcurl.

```scheme
(import net.http)
```

`net.http` is opt-in. Add the `eta-http` package to your app dependencies.

## Session

| Symbol | Description |
| --- | --- |
| `(make-session . opts)` | Create a reusable HTTP session handle with optional default options. |
| `(session? value)` | Predicate for session handles. |
| `(close-session! session)` | Close a session early and release native resources. |
| `(session-set-option! session name value)` | Set a session option. |
| `(session-get-option session name)` | Read one session option value. |

## Request builder

| Symbol | Description |
| --- | --- |
| `(make-request session method)` | Create a request handle bound to `session`. |
| `(request? value)` | Predicate for request handles. |
| `(request-set-url! request url)` | Set request URL. |
| `(request-set-option! request name value)` | Override one option for this request only. |
| `(request-set-header! request name value-or-false)` | Add/update a header, or remove it with `#f`. |
| `(request-set-body! request text)` | Set string body (UTF-8). |
| `(request-set-body-bytes! request bytes)` | Set bytevector body. |
| `(request-set-body-string! request text charset)` | Set string body with explicit charset label. |
| `(request-set-body-file! request path)` | Stream body from file path. |
| `(request-set-form! request fields)` | Submit URL-encoded form body from alist. |
| `(request-set-multipart! request parts)` | Submit multipart/form-data parts. |
| `(perform request)` | Execute request and return response handle. |
| `(perform-stream request)` | Execute request through streaming path. |

## Response

| Symbol | Description |
| --- | --- |
| `(response? value)` | Predicate for response handles. |
| `(status response)` | HTTP status code. |
| `(ok? response)` | `#t` for 2xx status. |
| `(raise-for-status! response)` | Raise runtime error on non-2xx status. |
| `(body-bytes response)` | Body as bytevector. |
| `(body-string response)` | Body decoded byte-by-byte to string. |
| `(body-json response)` | Parse body as JSON via `std.json`. |
| `(headers response)` | Header alist `(name . value)`. |
| `(header response name)` | First matching header value or `#f`. |
| `(effective-url response)` | Final URL after redirects. |

## Convenience helpers

| Symbol | Description |
| --- | --- |
| `(get url . rest)` | One-shot GET (`rest` is optional opts and/or session). |
| `(get-json url . rest)` | One-shot GET plus JSON decode. |
| `(post url body . rest)` | One-shot POST string body. |
| `(post-json url value . rest)` | One-shot POST with JSON-encoded body. |
| `(download url path . rest)` | Stream payload to file path. |

## URL helpers

| Symbol | Description |
| --- | --- |
| `(url-encode text)` | Percent-encode text. |
| `(url-decode text)` | Percent-decode text. |
| `(url-parse text)` | Parse URL to component alist. |
| `(url-build components)` | Build URL from component alist. |

## Common options

Options are set with `session-set-option!` or `request-set-option!`.

| Option | Type | Description |
| --- | --- | --- |
| `'follow-redirects` | bool | Enable redirect following. |
| `'max-redirects` | int | Redirect cap. |
| `'connect-timeout-ms` | int | Connect timeout in milliseconds. |
| `'timeout-ms` | int | Total request timeout in milliseconds (`0` means no cap). |
| `'user-agent` | string | User-Agent header value. |
| `'accept-encoding` | string | Accepted compression list (`""` lets libcurl negotiate). |
| `'verify-tls` | bool | TLS certificate/host verification toggle. |
| `'ca-bundle` / `'ca-path` | path | Custom trust bundle/path (backend dependent). |
| `'username` / `'password` | string | Basic auth credentials. |
| `'bearer-token` | string | Bearer token. |
| `'proxy` | string | Proxy URL. |
| `'http-version` | symbol | HTTP version preference. |
| `'verbose` | bool | Enable verbose transfer logging from libcurl. |

## Relationship to std.net

`std.net` is message-passing over NNG (Eta-to-Eta patterns). `net.http` is
client HTTP/HTTPS over libcurl (Eta-to-web-service patterns). They are
complementary surfaces and can be used together in one application.

## Notes

- `net.http` is a client package. It does not provide an HTTP server surface.
- HTTP status 4xx/5xx does not raise automatically; call `raise-for-status!`.
- Public internet examples should not be used as CI tests; use loopback fixtures.
