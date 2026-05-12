# libcurl HTTP/HTTPS Sidecar Package Plan

[Back to README](../../README.md) ·
[Stdlib Reference](../stdlib.md) ·
[Architecture](../architecture.md) ·
[Packaging](../packaging.md) ·
[HiGHS plan](HiGHS_package_plan.md)

> **Status.** Authoritative implementation plan for shipping
> [libcurl](https://curl.se/libcurl/) as an Eta native sidecar package
> that exposes an HTTP / HTTPS client (and, transitively, the other
> URL schemes libcurl supports) to Eta programs. An implementer
> should be able to execute this plan end-to-end without any other
> planning document.

---

## 0) Sequencing — curl vs HiGHS, and why both

### 0.1 Recommended order: **libcurl first, HiGHS second**

1. **Reach.** Approximately every non-trivial cookbook / app
   eventually wants to fetch a CSV, hit a REST API, or download a
   model. Today Eta has to shell out to system `curl`
   (`eta/cli/src/eta/cli/main_eta.cpp:1290`), which (a) requires
   `curl` on `PATH`, (b) gives no programmatic access to status
   codes / headers / streaming, and (c) is unusable from inside Eta
   programs without re-shelling. HiGHS, by contrast, only matters
   to optimisation users.
2. **Risk surface.** libcurl is the most-deployed C library on
   earth; its CMake story, ABI, and threading semantics are
   battle-tested across every target Eta supports. HiGHS is fine
   but newer to our toolchain. Landing libcurl first burns down
   any sidecar-pattern issues against the easier dependency.
3. **Unblocks the CLI.** Once `net.http` exists, the `eta` CLI's
   own package-download path can drop the `curl` shell-out and
   call into the sidecar (or a tiny statically linked variant of
   the same library), removing a runtime dependency from the
   distribution.
4. **Demo leverage.** HTTP unlocks a swathe of cookbook examples
   (REST clients, webhook receivers via existing `nng` for the
   server side, dataset downloaders for `ml.lightgbm` /
   `ml.torch`) — broader payoff per byte of sidecar than HiGHS.

So: ship libcurl in the next milestone, HiGHS the one after.

### 0.2 Are we duplicating network stacks? **No.**

Eta currently has exactly **one** network surface: `std.net`,
backed by [nng](https://nng.nanomsg.org). nng is a *message-passing*
library — REQ/REP, PUB/SUB, PUSH/PULL, SURVEYOR, BUS — with its own
on-the-wire framing. It exists to support actor-style concurrency
(`spawn`, `send!`, `recv!`, `request-reply`) and inter-process
message buses. It is the wrong tool for talking to the open web:

| Concern                          | nng                    | libcurl                 |
| -------------------------------- | ---------------------- | ----------------------- |
| Protocols                        | nng custom framing over TCP/IPC/inproc/WS | HTTP/1.1, HTTP/2, HTTP/3, HTTPS, FTP, SMTP, IMAP, SCP, SFTP, MQTT, … |
| Role                             | Peer-to-peer messaging | Client to a remote server |
| TLS                              | mbedTLS (optional)     | OpenSSL / Schannel / Secure Transport / NSS / mbedTLS / wolfSSL — pick at build |
| Cookies / redirects / auth       | n/a                    | Built in                |
| HTTP semantics (verbs, headers, status, multipart) | n/a  | Built in                |
| Connection pooling, HTTP/2 multiplexing, ALPN     | n/a   | Built in                |
| Streaming progress callbacks     | Pull-based recv loop   | Push-based callbacks    |
| Conformance to RFC 7230+ family  | n/a                    | Reference-quality       |

The two are **complementary**, not overlapping:

1. **nng = "Eta talks to Eta"** (and to other nng peers): actor
   buses, distributed compute, pub/sub, internal RPC.
2. **libcurl = "Eta talks to the world"**: REST, webhooks (client),
   downloads, uploads, OAuth flows, S3-compatible blob stores.

Implementing HTTP on top of nng would require us to write a
production HTTP/1.1 + HTTP/2 + TLS client from scratch. That is
exactly the wrong direction for "best-of-breed" — it duplicates
~250 KLOC of curl that is already audited, fuzzed, and shipped on
every operating system.

The right call is **best-of-breed for each axis**:

1. Keep `std.net` for message-passing — nng owns that.
2. Add `net.http` (this plan) for client-side web — libcurl owns
   that.
3. **Server-side HTTP** is a *separate, later* decision. The
   leading C/C++ choices (`mongoose`, `civetweb`, `h2o`,
   `Boost.Beast`) are smaller scope than libcurl and not required
   for v0.1. When the time comes, ship a `net.http.server`
   sidecar — but do **not** try to make libcurl serve.

### 0.3 Why not roll our own TLS-aware HTTP client?

1. **Maintenance debt.** RFC 9110 alone is 200+ pages; adding
   HTTP/2 (RFC 9113), HTTP/3 (RFC 9114), and a TLS implementation
   (RFC 8446) puts us in the wrong business.
2. **Security.** A bespoke HTTP client without years of fuzzing
   is a CVE waiting to happen. libcurl gets a security advisory
   roughly every release; we benefit from that vigilance for free.
3. **Platform TLS roots.** libcurl can be built against
   Schannel on Windows, Secure Transport on macOS, and the system
   OpenSSL on Linux, automatically picking up the OS trust store.
   Replicating that is non-trivial and brittle.

---

## 1) Goals and non-goals

### Goals

1. Ship a first-class HTTP / HTTPS **client** to Eta programs as
   the package `net.http`, exposed under a new `packages/net/`
   group.
2. Sidecar (loadable native module), not core built-in: the
   interpreter remains buildable and runnable without libcurl.
3. Cover the working surface most users need: `GET`, `POST`, `PUT`,
   `PATCH`, `DELETE`, `HEAD`, custom verbs; request / response
   headers; query strings; URL encoding; body as bytes / string /
   file path; streaming download to file or callback; basic auth;
   bearer auth; TLS with system root store; configurable timeouts;
   redirects; gzip / brotli decoding; HTTP/2 by default.
4. Provide an **idiomatic Eta builder API** plus thin convenience
   wrappers (`http/get`, `http/post-json`, `http/download`).
5. Provide a **shared session / connection pool** (`make-session`)
   matching libcurl's `curl_multi` / `curl_share` so cookies and
   keepalive connections are reused across requests.
6. Match the existing native-package pattern (`ml.lightgbm`,
   planned `optim.highs`): one CMakeLists, one
   `FetchLibcurl.cmake`, one extension entrypoint, one Eta wrapper
   module, smoke + unit tests.
7. Once stable, **retire the `curl` shell-out** in
   `main_eta.cpp:1290`: the CLI links the same library directly
   for package downloads, removing the system-`curl` runtime
   dependency.

### Non-goals

1. **No HTTP server.** That belongs in a future
   `net.http.server` sidecar (probably backed by `mongoose` or
   `civetweb`). libcurl cannot serve.
2. **No WebSocket-as-RPC layer.** libcurl 7.86+ has experimental
   WebSocket support; we expose it as a **raw frame** API in v0.2,
   not as an RPC framework. Users who want actor-shaped messaging
   keep using `std.net` over nng.
3. **No FTP / SMTP / IMAP / MQTT helpers in v1.** libcurl supports
   them and the sidecar accidentally inherits them via the URL
   handler, but the documented surface is HTTP / HTTPS only.
   Other schemes work via `http/url-request` but are unsupported.
4. **No HTTP/3 (QUIC) in v1.** Requires linking nghttp3 + ngtcp2 +
   a QUIC-capable TLS library; defer to v0.2 once HTTP/2 is
   stable.
5. **No automatic JSON deserialisation in the core API.** The
   `post-json` / `get-json` helpers wrap `std.json` for ergonomics
   but the underlying API is bytes / string only.
6. **No prelude re-export.** Native packages are opt-in via
   `(import net.http)`.

---

## 2) Where this slots in the codebase

| Concern                                  | Path                                                                       | Action |
| ---------------------------------------- | -------------------------------------------------------------------------- | ------ |
| Package root                             | `packages/net/native/http/`                                                | NEW    |
| Package manifest                         | `packages/net/native/http/eta.toml`                                        | NEW    |
| Package CMake driver                     | `packages/net/native/http/CMakeLists.txt`                                  | NEW    |
| Upstream fetch helper                    | `packages/net/native/http/cmake/FetchLibcurl.cmake`                        | NEW    |
| Sidecar staging helper                   | `packages/net/native/http/cmake/StageHttpSidecar.cmake`                    | NEW    |
| Native extension entry                   | `packages/net/native/http/src/eta/http/http_extension.cpp`                 | NEW    |
| Native primitive bindings                | `packages/net/native/http/src/eta/http/http_primitives.{h,cpp}`            | NEW    |
| Session / handle wrappers (RAII)         | `packages/net/native/http/src/eta/http/http_session.{h,cpp}`               | NEW    |
| Async / multi-handle driver              | `packages/net/native/http/src/eta/http/http_multi.{h,cpp}`                 | NEW    |
| Eta module wrapper                       | `packages/net/native/http/src/net/http.eta`                                | NEW    |
| JSON convenience layer                   | `packages/net/native/http/src/net/http/json.eta`                           | NEW    |
| Pre-built artefacts                      | `packages/net/native/http/libs/{amd64,arm64}/`                             | NEW    |
| Eta smoke test                           | `packages/net/native/http/tests/eta/http_smoke.test.eta`                   | NEW    |
| Eta smoke test driver                    | `packages/net/native/http/tests/eta/run_http_eta_smoke.cmake`              | NEW    |
| Local test server fixture                | `packages/net/native/http/tests/fixtures/loopback_server.py`               | NEW    |
| C++ unit tests                           | `packages/net/native/http/tests/unit/http_session_tests.cpp`               | NEW    |
| Package README                           | `packages/net/native/http/README.md`                                       | NEW    |
| Stdlib reference doc                     | `docs/stdlib/net-http.md`                                                  | NEW    |
| Stdlib index                             | `docs/stdlib.md`                                                           | EDIT   |
| Cookbook entry — quickstart              | `cookbook/net/http-quickstart.eta`                                         | NEW    |
| Cookbook entry — REST client             | `cookbook/net/rest-client.eta`                                             | NEW    |
| Cookbook entry — streaming download      | `cookbook/net/download-large-file.eta`                                     | NEW    |
| Cookbook README index                    | `cookbook/net/README.md`                                                   | NEW    |
| CLI download path migration              | `eta/cli/src/eta/cli/main_eta.cpp` (line ~1290 today)                      | EDIT (M5) |
| Architecture doc — networking section    | `docs/architecture.md`                                                     | EDIT   |
| Build doc — optional packages            | `docs/build.md`                                                            | EDIT   |
| Release-notes entry                      | `docs/release-notes.md`                                                    | EDIT   |

The new top-level group `packages/net/` is created by this plan,
sibling to `packages/ml/`, `packages/numerics/`, `packages/db/`.

---

## 3) Upstream pin and fetch

### 3.1 Version

| Setting               | Value                                          |
| --------------------- | ---------------------------------------------- |
| `ETA_LIBCURL_VERSION` | `8.13.0` (latest stable as of 2026-05)        |
| `ETA_LIBCURL_GIT_TAG` | `curl-8_13_0`                                  |
| Source                | `https://github.com/curl/curl.git`             |
| License               | curl license (MIT-style, MIT-compatible)       |
| C standard required   | C99 (Eta is C++23, fine)                       |

### 3.2 TLS backend per platform

| Target                          | TLS backend         | Cert store                          |
| ------------------------------- | ------------------- | ----------------------------------- |
| `x86_64-pc-windows-msvc`        | Schannel            | Windows certificate store           |
| `aarch64-pc-windows-msvc`       | Schannel            | Windows certificate store           |
| `x86_64-apple-darwin`           | Secure Transport    | macOS Keychain                      |
| `aarch64-apple-darwin`          | Secure Transport    | macOS Keychain                      |
| `x86_64-unknown-linux-gnu`      | OpenSSL (system)    | `/etc/ssl/certs` via OpenSSL probing |
| `aarch64-unknown-linux-gnu`     | OpenSSL (system)    | same                                |

Picking the OS-native backend per platform avoids shipping our own
trust store or OpenSSL DLLs; the resulting sidecar uses the
machine's existing certificate management. The CMake fetch helper
sets `CURL_USE_SCHANNEL`, `CURL_USE_SECTRANSP`, `CURL_USE_OPENSSL`
accordingly.

### 3.3 `FetchLibcurl.cmake`

Mirrors `cmake/FetchEigen.cmake` and the lightgbm package's
`FetchLightGBM.cmake`. Disables every optional protocol Eta does
not document, to keep binary size and CVE surface minimal:

```cmake
include(FetchContent)

# Protocols — keep HTTP family only in v1.
set(CURL_DISABLE_FTP        ON  CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAP       ON  CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAPS      ON  CACHE BOOL "" FORCE)
set(CURL_DISABLE_TELNET     ON  CACHE BOOL "" FORCE)
set(CURL_DISABLE_TFTP       ON  CACHE BOOL "" FORCE)
set(CURL_DISABLE_DICT       ON  CACHE BOOL "" FORCE)
set(CURL_DISABLE_FILE       ON  CACHE BOOL "" FORCE)
set(CURL_DISABLE_GOPHER     ON  CACHE BOOL "" FORCE)
set(CURL_DISABLE_IMAP       ON  CACHE BOOL "" FORCE)
set(CURL_DISABLE_POP3       ON  CACHE BOOL "" FORCE)
set(CURL_DISABLE_RTSP       ON  CACHE BOOL "" FORCE)
set(CURL_DISABLE_SMB        ON  CACHE BOOL "" FORCE)
set(CURL_DISABLE_SMTP       ON  CACHE BOOL "" FORCE)
set(CURL_DISABLE_MQTT       ON  CACHE BOOL "" FORCE)

# Keep HTTP, HTTPS, HTTP/2.
set(USE_NGHTTP2             ON  CACHE BOOL "" FORCE)
set(USE_NGHTTP3             OFF CACHE BOOL "" FORCE)  # v1: no HTTP/3
set(USE_LIBIDN2             OFF CACHE BOOL "" FORCE)  # avoid libidn2 dep

# TLS backend per platform — wired by the package CMakeLists.

set(BUILD_SHARED_LIBS       ON  CACHE BOOL "" FORCE)
set(BUILD_TESTING           OFF CACHE BOOL "" FORCE)
set(BUILD_CURL_EXE          OFF CACHE BOOL "" FORCE)
set(ENABLE_CURL_MANUAL      OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    libcurl
    GIT_REPOSITORY https://github.com/curl/curl.git
    GIT_TAG        curl-8_13_0
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
    SYSTEM
)

set(_eta_prev_warn "${CMAKE_WARN_DEPRECATED}")
set(CMAKE_WARN_DEPRECATED OFF)
FetchContent_MakeAvailable(libcurl)
set(CMAKE_WARN_DEPRECATED "${_eta_prev_warn}")
unset(_eta_prev_warn)

message(STATUS "libcurl 8.13.0 fetched — CURL::libcurl target available")
```

### 3.4 System-install fallback

```cmake
find_package(CURL 8.0 QUIET)
if(NOT CURL_FOUND AND ETA_HTTP_FETCH_UPSTREAM)
    include(cmake/FetchLibcurl.cmake)
endif()
if(NOT TARGET CURL::libcurl)
    message(FATAL_ERROR
        "libcurl not found. Re-configure with -DETA_HTTP_FETCH_UPSTREAM=ON"
        " or install libcurl >=8.0 (with HTTP/2 + TLS) and ensure"
        " CURLConfig.cmake is on CMAKE_PREFIX_PATH.")
endif()
```

---

## 4) Native sidecar — C++ surface

### 4.1 File layout

```
src/eta/http/
    http_extension.cpp      # eta_register_http_extension_v1 entrypoint
    http_primitives.h       # primitive declarations + registration
    http_primitives.cpp     # arg unpack + dispatch
    http_session.h          # RAII over CURL* + curl_share + cookie jar
    http_session.cpp
    http_multi.h            # RAII over CURLM* for streaming + concurrent ops
    http_multi.cpp
```

### 4.2 Sidecar manifest (`eta.toml`)

```toml
[package]
name    = "eta-http"
version = "0.1.0"
license = "MIT"          # the wrapper code; libcurl ships under the curl license

[compatibility]
eta = ">=0.0, <0.8"

[native]
kind   = "sidecar"
abi    = "eta-native-v1"
id     = "eta.http.sidecar"
entry  = "eta_register_http_extension_v1"

[[native.targets]]
triple   = "x86_64-pc-windows-msvc"
artifact = "libs/amd64/eta_http.dll"
sha256   = "0000000000000000000000000000000000000000000000000000000000000000"

[[native.targets]]
triple   = "x86_64-unknown-linux-gnu"
artifact = "libs/amd64/libeta_http.so"
sha256   = "0000000000000000000000000000000000000000000000000000000000000000"

[[native.targets]]
triple   = "aarch64-unknown-linux-gnu"
artifact = "libs/arm64/libeta_http.so"
sha256   = "0000000000000000000000000000000000000000000000000000000000000000"

[[native.targets]]
triple   = "x86_64-apple-darwin"
artifact = "libs/amd64/libeta_http.dylib"
sha256   = "0000000000000000000000000000000000000000000000000000000000000000"

[[native.targets]]
triple   = "aarch64-apple-darwin"
artifact = "libs/arm64/libeta_http.dylib"
sha256   = "0000000000000000000000000000000000000000000000000000000000000000"
```

### 4.3 Extension entrypoint

`http_extension.cpp` exports the canonical sidecar symbol:

```cpp
extern "C" ETA_NATIVE_EXPORT
int eta_register_http_extension_v1(eta_native_registry_v1* reg);
```

Body:

1. Calls `curl_global_init(CURL_GLOBAL_DEFAULT)` exactly once
   (guarded by `std::call_once`).
2. Calls `eta::http::register_primitives(reg)`.
3. Returns `ETA_NATIVE_OK`.

On sidecar unload (the runtime calls a destructor hook), runs
`curl_global_cleanup()`. This matches libcurl's documented init/
cleanup contract.

### 4.4 Primitive surface

All primitives are namespaced `http/` in Scheme-land. They are
intentionally low-level; the user-facing ergonomic API lives in
`net/http.eta` (§5).

#### 4.4.1 Session and request lifecycle

| Primitive                      | Arity | Description |
| ------------------------------ | :---: | ----------- |
| `http/version`                 | 0     | Returns `(libcurl-version protocols-supported features)` triple. |
| `http/session-new`             | 0     | Allocates a new `http-session` foreign handle: owns a `curl_share`, default cookie jar, default option set. |
| `http/session-set-option!`     | 3     | `(session name value)` — typed dispatch on `value`. Names enumerated in §4.5. |
| `http/session-get-option`      | 2     | Read a session-level option. |
| `http/session-cookies`         | 1     | Returns the current cookie jar as a list of (domain path name value expires …) tuples. |
| `http/session-clear-cookies!`  | 1     | Empty the jar. |
| `http/request-new`             | 2     | `(session method)` → fresh `http-request` handle inheriting session defaults. `method` ∈ `{'get 'post 'put 'patch 'delete 'head 'options}` or any string for custom verbs. |
| `http/request-set-option!`     | 3     | Per-request override; same option vocabulary as session. |
| `http/request-set-url!`        | 2     | Set URL (post URL-encoding via `curl_url_*`). |
| `http/request-set-header!`     | 3     | `(request name value)`. Repeated calls accumulate; `value = #f` removes. |
| `http/request-set-body-bytes!` | 2     | `(request bytevector)`. |
| `http/request-set-body-string!`| 3     | `(request string charset)`. |
| `http/request-set-body-file!`  | 2     | `(request path)` — streams from file (no full read). |
| `http/request-set-body-form!`  | 2     | `(request alist)` — `application/x-www-form-urlencoded` builder. |
| `http/request-set-body-multipart!` | 2 | `(request parts)` — `multipart/form-data` builder; each part is `(name [filename] [content-type] (bytes …))`. |
| `http/perform`                 | 1     | Synchronously execute the request; returns an `http-response` handle. |
| `http/perform-stream`          | 3     | `(request on-headers on-chunk)` — async-style streaming with two callbacks. Returns `http-response` after the body completes. |

#### 4.4.2 Response accessors

| Primitive                            | Arity | Description |
| ------------------------------------ | :---: | ----------- |
| `http/response-status`               | 1     | Integer HTTP status code. |
| `http/response-effective-url`        | 1     | Final URL after redirects. |
| `http/response-headers`              | 1     | List of `(name . value)` pairs, in transmission order; duplicates preserved. |
| `http/response-header`               | 2     | First value for a given header name (case-insensitive); `#f` if absent. |
| `http/response-body-bytes`           | 1     | Bytevector of the full response body. |
| `http/response-body-string`          | 2     | `(response charset)` — decoded string; raises on charset mismatch. |
| `http/response-body-file`            | 2     | `(response path)` — write streamed body to file (only available if `perform-stream` was used or body was small enough to buffer). |
| `http/response-elapsed-ms`           | 1     | Total wall-clock for the request. |
| `http/response-redirect-count`       | 1     | Diagnostic. |
| `http/response-protocol`             | 1     | Symbol: `'http/1.0` `'http/1.1` `'http/2`. |
| `http/response?`                     | 1     | Predicate. |
| `http/session?` / `http/request?`    | 1     | Predicates. |

#### 4.4.3 Convenience primitives (one-shot API)

These are **thin shims** in C++ that bundle session + request +
perform in one call, for the (very common) use case where a user
just wants to GET a URL.

| Primitive                | Arity | Description |
| ------------------------ | :---: | ----------- |
| `http/get`               | 2     | `(url options-alist)` → `http-response`. Uses a per-call session. |
| `http/post`              | 3     | `(url body options-alist)` |
| `http/download`          | 3     | `(url path options-alist)` — streaming download, returns the response handle (without buffered body). |
| `http/url-encode`        | 1     | String → percent-encoded string. |
| `http/url-decode`        | 1     | Inverse. |
| `http/url-parse`         | 1     | URL string → record-shaped alist of components. |
| `http/url-build`         | 1     | Inverse. |

### 4.5 Option vocabulary

Options are Scheme symbols mapped to libcurl `CURLOPT_*` codes
inside the wrapper. Unknown option names raise. Every documented
option is listed in `docs/stdlib/net-http.md`.

| Symbol                          | libcurl option                 | Type     |
| ------------------------------- | ------------------------------ | -------- |
| `'follow-redirects`             | `CURLOPT_FOLLOWLOCATION`       | bool     |
| `'max-redirects`                | `CURLOPT_MAXREDIRS`            | int      |
| `'connect-timeout-ms`           | `CURLOPT_CONNECTTIMEOUT_MS`    | int      |
| `'timeout-ms`                   | `CURLOPT_TIMEOUT_MS`           | int      |
| `'user-agent`                   | `CURLOPT_USERAGENT`            | string   |
| `'accept-encoding`              | `CURLOPT_ACCEPT_ENCODING`      | string   |
| `'verify-tls`                   | `CURLOPT_SSL_VERIFYPEER` + `…VERIFYHOST` | bool |
| `'ca-bundle`                    | `CURLOPT_CAINFO`               | path     |
| `'ca-path`                      | `CURLOPT_CAPATH`               | path     |
| `'client-cert` / `'client-key`  | `CURLOPT_SSLCERT` / `…KEY`     | path     |
| `'http-version`                 | `CURLOPT_HTTP_VERSION`         | symbol   |
| `'username` / `'password`       | `CURLOPT_USERNAME` / `…PASSWORD` | string |
| `'bearer-token`                 | `CURLOPT_XOAUTH2_BEARER`       | string   |
| `'proxy`                        | `CURLOPT_PROXY`                | string   |
| `'cookie-jar`                   | `CURLOPT_COOKIEJAR`            | path     |
| `'cookie-file`                  | `CURLOPT_COOKIEFILE`           | path     |
| `'verbose`                      | `CURLOPT_VERBOSE`              | bool     |
| `'low-speed-limit-bps`          | `CURLOPT_LOW_SPEED_LIMIT`      | int      |
| `'low-speed-time-s`             | `CURLOPT_LOW_SPEED_TIME`       | int      |
| `'unix-socket-path`             | `CURLOPT_UNIX_SOCKET_PATH`     | path     |

Sensible defaults set on every new session:

1. `follow-redirects = #t`, `max-redirects = 10`.
2. `connect-timeout-ms = 30000`, `timeout-ms = 0` (no overall
   limit; users opt in).
3. `user-agent = "eta-http/<package-version> libcurl/<runtime>"`.
4. `accept-encoding = ""` (let libcurl advertise what it supports).
5. `verify-tls = #t`.
6. `http-version = 'http/2-tls` (HTTP/2 over TLS, fall back to 1.1).

### 4.6 Foreign handle representation

Three opaque foreign objects allocated through the existing
sidecar SDK foreign-pointer machinery (the same channel
`lgbm-dataset` and the planned `highs-model` use):

```cpp
struct HttpSession {
    CURLSH* share = nullptr;            // shared cookies / DNS / SSL session cache
    std::map<int, OptionValue> defaults;
    std::shared_ptr<HttpLogSink> log;
};

struct HttpRequest {
    std::shared_ptr<HttpSession> session;
    CURL* easy = nullptr;               // owned
    curl_slist* headers = nullptr;      // owned
    BodySource body = std::monostate{};
    std::map<int, OptionValue> overrides;
};

struct HttpResponse {
    long status = 0;
    std::string effective_url;
    std::vector<std::pair<std::string,std::string>> headers;
    std::vector<std::byte> body;        // empty if streamed to file/callback
    double elapsed_ms = 0;
    long redirect_count = 0;
    long protocol = 0;                  // CURL_HTTP_VERSION_*
};
```

Lifetime rules:

1. Closing a session invalidates outstanding requests *bound to it*.
   The wrapper keeps `shared_ptr<HttpSession>` so request handles
   stay valid until they are themselves released.
2. Bodies passed to `request-set-body-bytes!` are **copied** into
   the request handle (Eta strings/bytevectors are GC-managed and
   may move); large file uploads therefore go through
   `request-set-body-file!` to avoid copies.
3. `HttpResponse` is fully self-contained — no pointers back into
   the request — so it can outlive both the request and the
   session.

### 4.7 Threading

1. Each `http-session` carries a `CURLSH` configured with
   `CURL_LOCK_DATA_DNS`, `CURL_LOCK_DATA_SSL_SESSION`,
   `CURL_LOCK_DATA_CONNECT`, `CURL_LOCK_DATA_PSL`, and a
   `std::mutex` for the lock callbacks. Two requests against the
   same session can run in parallel from two Eta worker threads.
2. `http/perform` is **blocking** on the calling thread.
3. `http/perform-stream` runs the multi-handle event loop on a
   dedicated worker thread (one per call) and marshals callback
   invocations back to the calling thread via the existing actor
   mailbox plumbing, so user callbacks observe Eta semantics.
4. `curl_global_init` / `curl_global_cleanup` are bracketed in
   `http_extension.cpp` (§4.3). All other libcurl entrypoints are
   thread-safe under the documented init contract.

### 4.8 Error handling

Every primitive returns a `RuntimeError` with category
`RuntimeErrorCode::IoError` (or `TypeError` for arg-shape errors)
carrying the libcurl error string from `curl_easy_strerror` plus
the CURLcode integer. The Eta wrapper translates these to
`(error "net.http: <message>")` matching the rest of the stdlib.

For HTTP-level errors (4xx/5xx), the response is returned
**successfully** with the status code; the wrapper offers
`raise-for-status!` (§5) for users who want exception-on-error
semantics.

---

## 5) Eta-side surface — `net.http`

### 5.1 Public API

```scheme
(module net.http
  (export
    ;; Session
    make-session  session?  close-session!
    session-set-option!  session-get-option
    session-cookies  clear-cookies!
    ;; One-shot convenience
    get  post  put  patch  delete  head
    post-json  get-json
    download
    ;; Builder API
    make-request  request?
    request-set-url!  request-set-header!
    request-set-body!  request-set-form!  request-set-multipart!
    request-set-option!
    perform  perform-stream
    ;; Response
    response?  status  ok?  raise-for-status!
    headers  header  body-bytes  body-string  body-json
    body->file  effective-url  elapsed-ms  redirect-count  protocol
    ;; URL helpers
    url-encode  url-decode  url-parse  url-build
    ;; Re-export raw primitives for advanced use
    http))
```

### 5.2 Quickstart (intended cookbook entry)

```scheme
(import net.http std.json)

;; One-shot GET.
(let ((r (get "https://api.github.com/repos/curl/curl")))
  (println "status:" (status r))
  (println "stars: " (assoc-ref (body-json r) 'stargazers_count)))

;; Session-scoped REST client with bearer token.
(let ((s (make-session
           '((bearer-token . "ghp_…")
             (timeout-ms   . 30000)
             (user-agent   . "demo/0.1")))))
  (let ((issues (body-json (get "https://api.github.com/issues" '() s))))
    (for-each (lambda (issue)
                (println (assoc-ref issue 'title)))
              issues))
  (close-session! s))

;; Streaming download to file.
(download "https://example.com/big.tgz" "/tmp/big.tgz"
          '((timeout-ms . 0) (low-speed-limit-bps . 10000)))
```

### 5.3 Conventions

1. Every public function accepts an **optional trailing session
   argument**; absent ⇒ a transient session is created and closed
   automatically.
2. Mutators end in `!`; predicates end in `?`.
3. `ok?` is `(<= 200 status 299)`. `raise-for-status!` raises
   `(error "net.http: HTTP <status> at <url>")` for non-2xx.
4. Error message prefix is uniformly `"net.http: …"`.
5. `body-json` requires `(import std.json)` transitively; if
   `std.json` is not loaded the helper raises a clear error
   pointing to it.

### 5.4 Concurrency story

Direct interop with `std.net` actors:

1. Spawn one actor per outbound request; each actor owns its own
   request handle and `(send! parent (perform req))`. This is the
   recommended pattern for fan-out HTTP calls.
2. A future `http-pool` helper (§9 / M5) ships a worker-pool over
   a shared session so users do not have to wire actors by hand.

---

## 6) Test plan

### 6.1 C++ unit tests (`tests/unit/http_session_tests.cpp`)

Run against a loopback HTTP server fixture (Python
`http.server`-based, started by the test harness on a free
port). Mirrors the lightgbm unit-test pattern.

1. `version` — `http/version` returns a non-empty triple.
2. `get-200` — GET `/echo` returns 200 and a parsed JSON body.
3. `get-404` — returns 404; no exception raised.
4. `post-bytes` — body round-trips byte-for-byte.
5. `post-multipart` — server reflects the parts; assert names.
6. `headers-roundtrip` — custom header set on request appears in
   server-side echo.
7. `redirect-follow` — server returns 302; effective URL ends at
   the redirect target.
8. `redirect-cap` — `'max-redirects = 0` short-circuits.
9. `timeout` — server stalls; `'timeout-ms = 100` returns a
   `RuntimeError` with `CURLE_OPERATION_TIMEDOUT`.
10. `tls-verify` — point at `https://expired.badssl.com` (skipped
    in offline CI); assert verify failure.
11. `cookie-roundtrip` — server `Set-Cookie`, second request
    against same session sends `Cookie:` back.
12. `streaming-download` — server returns a 10 MB random
    payload; `perform-stream` writes the file with matching
    SHA-256.
13. `parallel-sessions` — two threads running independent
    sessions complete with no data race (TSAN-clean).

### 6.2 Eta smoke test (`tests/eta/http_smoke.test.eta`)

Runs through `run_http_eta_smoke.cmake`, starts the loopback
server, then exercises:

1. `make-session` returns a `session?`; `close-session!` works.
2. One-shot `get` against the loopback returns a 200 and the
   body string equals the known fixture.
3. `post-json` round-trips a JSON object via `std.json`.
4. `download` writes a 1 KB fixture file with matching bytes.
5. `raise-for-status!` raises on a 500 response.
6. URL helpers: `url-encode` / `url-decode` are mutual inverses.

### 6.3 Cookbook examples as integration tests

The three new files in `cookbook/net/` are added to the cookbook
smoke pass that already runs other examples. They run against
the loopback fixture (the public-internet examples in their
docstrings are illustrative only and gated off in CI).

### 6.4 Offline CI

Every test must pass with **no internet access**. Public-URL
examples live in documentation only. The loopback fixture is
launched and torn down by the test driver and binds to
`127.0.0.1:<random>`.

---

## 7) Documentation deliverables

### 7.1 `docs/stdlib/net-http.md`

Follows the shape of `docs/stdlib/net.md` plus the planned
`docs/stdlib/optim-highs.md`:

1. One-line summary, license note (curl license), link upstream.
2. **Synopsis** code block — the §5.2 quickstart.
3. Section per category (Session / Requests / Responses /
   Convenience / URL helpers / Options / Errors / Concurrency).
4. Full options table (§4.5).
5. **Relationship to `std.net`** sub-section pasted from §0.2 of
   this plan, condensed to a paragraph: client vs message-passing.
6. **Limitations** — no server, no HTTP/3 in v1, no WebSockets in
   v1, no FTP/SMTP helpers.
7. **Performance notes** — connection reuse via shared sessions;
   when to use streaming.
8. **TLS roots** — explains the per-platform backend choice and
   how to override with `'ca-bundle`.

### 7.2 `docs/stdlib.md`

New "Networking" subsection (or extend the existing one):

```
- [std.net](stdlib/net.md)        — Message-passing actors over nng.
- [net.http](stdlib/net-http.md)  — HTTP/HTTPS client (libcurl
  native sidecar). Opt-in via the `eta-http` package.
```

### 7.3 `docs/architecture.md`

Add one paragraph under the networking section: nng owns
peer-to-peer messaging; libcurl owns HTTP client; the two are
independent and complementary; future `net.http.server` will own
HTTP serving.

### 7.4 Cookbook entries

1. `cookbook/net/http-quickstart.eta` — minimal GET / POST /
   header / status example.
2. `cookbook/net/rest-client.eta` — auth + JSON + pagination
   pattern against the loopback fixture.
3. `cookbook/net/download-large-file.eta` — `download` with
   progress-callback usage and resume on disconnect.

---

## 8) Build / packaging touchpoints

1. **Top-level CMakeLists** — no change. Sidecar lives in its
   own out-of-tree CMakeLists.
2. **`scripts/build_packages.{ps1,sh}`** — add `net/http` to the
   package iteration list.
3. **`scripts/build-release.{ps1,sh,cmd}`** — include
   `eta_http.{dll,so,dylib}` in the release bundle when
   `ETA_INCLUDE_HTTP=1`. Default **on** once stable, since the CLI
   itself depends on it after M5.
4. **Bundle verification scripts** — whitelist the new artefact.
5. **`eta.lock`** — SHA-256s frozen by the existing locking flow.
6. **Top-level README** — extend the "Native packages" list.

---

## 9) Phased delivery roadmap

### M0 — Scaffolding (1 day)

1. Create `packages/net/native/http/`, copy the `eta-lightgbm`
   skeleton, rename symbols.
2. Land `eta.toml` with placeholder SHAs and a no-op
   `http_extension.cpp`.
3. Land package CMakeLists with `find_package(CURL)` only;
   confirm it builds against system libcurl.

Gate: `cmake --build` succeeds; loading the sidecar from `etai`
does not crash.

### M1 — Fetch + minimal GET (2 days)

1. Land `cmake/FetchLibcurl.cmake` with the per-platform TLS
   backend selection, gated on `ETA_HTTP_FETCH_UPSTREAM`.
2. Implement `http_session.{h,cpp}` and the primitives
   `http/version`, `http/session-new`, `http/request-new`,
   `http/request-set-url!`, `http/perform`, plus the response
   accessors `http/response-status`, `http/response-body-bytes`,
   `http/response-headers`, `http/response-effective-url`.
3. Land Eta wrappers `make-session`, `make-request`, `perform`,
   `status`, `body-bytes`, `body-string`, `headers`, `header`.
4. C++ unit tests §6.1 cases 1, 2, 3.

Gate: `(get "http://127.0.0.1:<port>/")` returns a 200 and the
expected bytes from the loopback fixture.

### M2 — Bodies, headers, options, redirects (2 days)

1. Implement `request-set-body-{bytes,string,file,form,multipart}!`
   and the option vocabulary in §4.5.
2. Implement `http/url-encode` / `decode` / `parse` / `build` via
   `curl_url_*`.
3. C++ unit tests §6.1 cases 4–8, 11. Eta smoke tests §6.2 cases
   1–3, 6.

Gate: `post-multipart` round-trips parts; redirects follow up to
the configured cap.

### M3 — Streaming and concurrency (2–3 days)

1. Implement `http_multi.{h,cpp}` and `http/perform-stream`,
   `http/download`.
2. Implement TLS option plumbing and the per-platform default
   backend selection.
3. C++ unit tests §6.1 cases 9, 10, 12, 13. Eta smoke tests §6.2
   cases 4, 5.
4. Land `cookbook/net/download-large-file.eta`.

Gate: 10 MB streaming download writes a file with matching
SHA-256; TSAN clean on the parallel-sessions test.

### M4 — Convenience, JSON, docs, packaging (1–2 days)

1. Implement `post-json` / `get-json` (depend on `std.json`).
2. Land all three cookbook examples; wire them into the cookbook
   smoke pass.
3. Write `docs/stdlib/net-http.md`; update `docs/stdlib.md`,
   `docs/architecture.md`, `docs/build.md`,
   `docs/release-notes.md`.
4. Wire into `scripts/build_packages.*` and
   `scripts/build-release.*`.
5. Build artefacts on Windows / Linux x64 (and Linux arm64 if
   runner available); freeze SHAs.

Gate: fresh checkout + `scripts/build_packages.ps1` produces a
loadable sidecar; cookbook smoke pass green.

### M5 — CLI migration (1 day, post-v0.1)

1. Replace the `run_external("curl", …)` call in
   `eta/cli/src/eta/cli/main_eta.cpp:~1290` with a direct call
   into a tiny statically linked variant of the same library
   (or invoke the sidecar in-process if it is loaded).
2. Drop `curl` from the documented runtime prerequisites.
3. Add a regression test that package downloads work on a
   machine **without** system `curl`.

Gate: `eta install <pkg>` works on a fresh container with no
`curl` binary on `PATH`.

### M6 *(optional, post-v0.1)* — Quality of life

1. WebSockets: expose libcurl's WS API as `http/ws-connect`,
   `http/ws-send!`, `http/ws-recv!` (raw frames; no RPC layer).
2. HTTP/3: enable `USE_NGHTTP3` once the QUIC TLS story is stable
   on every target.
3. Pluggable progress callbacks (bytes-uploaded, ETA).
4. `http-pool` worker-pool helper bridging to `std.net`.

---

## 10) Open questions and risks

1. **Windows DLL search path.** `eta_http.dll` may need
   `nghttp2.dll` next to it. The `StageHttpSidecar.cmake` helper
   copies all transitive DLLs into `libs/amd64/` so the OS loader
   resolves them. Verify with `dumpbin /dependents` during M1.
2. **TLS backend per-platform divergence.** Schannel does not
   support `'ca-bundle` (it always uses the OS store);
   documenting that gracefully matters. The wrapper raises a
   clear "option ignored on this backend" warning when
   `'ca-bundle` is set on Windows.
3. **HTTP/2 negotiation.** Some corporate proxies break ALPN.
   The default `'http-version = 'http/2-tls` falls back to 1.1
   automatically per libcurl semantics, but document the
   `'http/1.1` override clearly.
4. **License surface.** libcurl is curl-licensed (MIT-style,
   compatible). Transitive dependencies — nghttp2 (MIT), zlib
   (zlib license), Schannel/SecureTransport/OpenSSL — are all
   compatible. Audit the actual dependency tree on each target
   during M4 before freezing the bundle.
5. **CVE cadence.** libcurl ships a security advisory roughly
   every release. Plan to bump `ETA_LIBCURL_VERSION` on a
   monthly cadence; document the upgrade procedure in the
   package README.
6. **Binary size.** A statically linked HTTPS-capable libcurl is
   ~1.5–2.5 MB per target. Acceptable; the existing libtorch
   payload dwarfs this.
7. **Group naming.** This plan creates `packages/net/`, sibling
   to `packages/numerics/` (HiGHS plan). Decision rationale:
   matches `cookbook/`'s emerging organisation, leaves room for
   `net.http.server`, `net.grpc`, `net.s3` as siblings without
   re-organising.
8. **Loopback test fixture portability.** Python `http.server`
   needs Python on the test machine. CI already has Python (used
   by `scripts/build_stdlib_etac.py` and the docs build), so
   this is not a new dependency.
9. **`tls-verify = #f` foot-gun.** Easy to disable; document
   prominently and refuse it in `download` / `post-json` /
   `get-json` unless `'allow-insecure = #t` is also set, to
   make accidental disablement a two-step.

---

## 11) Acceptance checklist

- [ ] `packages/net/native/http/` exists with the file layout in
      §2.
- [ ] `cmake --build` succeeds with both
      `ETA_HTTP_FETCH_UPSTREAM=ON` and a system libcurl install
      on each documented target.
- [ ] `eta_register_http_extension_v1` is exported and registers
      every primitive in §4.4.
- [ ] `net.http` exports every symbol in §5.1.
- [ ] All thirteen C++ unit tests in §6.1 pass; the parallel-
      sessions test is TSAN clean.
- [ ] All six Eta smoke tests in §6.2 pass against the loopback
      fixture.
- [ ] `cookbook/net/http-quickstart.eta`,
      `rest-client.eta`, `download-large-file.eta` run green.
- [ ] `docs/stdlib/net-http.md` written; `docs/stdlib.md` lists
      the new module under "Networking".
- [ ] `eta.toml` lockfile carries real SHA-256s for at least
      Windows x64 and Linux x64.
- [ ] `scripts/build_packages.*` builds the package end-to-end.
- [ ] Bundle verification scripts pass with the new artefact
      present.
- [ ] **(M5)** `eta install <pkg>` succeeds on a machine with no
      system `curl` binary.

