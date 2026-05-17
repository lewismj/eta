# Eta HTTP (libcurl) Sidecar

This package is the `net.http` native sidecar under
`packages/net/native/http`.

Current scope:

- package manifest with sidecar metadata (`eta.toml`)
- package-local CMake build with system-libcurl fallback and
  `ETA_HTTP_FETCH_UPSTREAM=ON` support
- fetched libcurl is linked statically into `eta_http` to keep runtime
  dependencies package-local
- fetched libcurl build now gracefully falls back to `CURL_ZLIB=OFF` when
  a local zlib development package is not available at configure time
- sidecar link against `eta_core` (in-tree target or prebuilt
  `ETA_CORE_LIBRARY`) with no local runtime stubs
- sidecar entrypoint export (`eta_register_http_extension_v1`) with
  `curl_global_init` / `curl_global_cleanup` lifecycle
- native primitives now include M3 request/session controls:
  - `http/version`
  - `http/session-new`
  - `http/session-close!`
  - `http/session?`
  - `http/request?`
  - `http/response?`
  - `http/session-set-option!`
  - `http/session-get-option`
  - `http/request-new`
  - `http/request-set-option!`
  - `http/request-set-url!`
  - `http/request-set-header!`
  - `http/request-set-body-bytes!`
  - `http/request-set-body-string!`
  - `http/request-set-body-file!`
  - `http/request-set-body-form!`
  - `http/request-set-body-multipart!`
  - `http/perform`
  - `http/perform-stream`
  - `http/download`
  - `http/response-status`
  - `http/response-body-bytes`
  - `http/response-headers`
  - `http/response-effective-url`
  - `http/url-encode`
  - `http/url-decode`
  - `http/url-parse`
  - `http/url-build`
- Eta wrappers in [`src/net/http.eta`](src/net/http.eta):
  `make-session`, `session?`, `close-session!`, `session-set-option!`,
  `session-get-option`, request builder mutators, `perform` /
  `perform-stream`, response accessors, one-shot `get`/`post`/`post-json` /
  `get-json`/`download`, `body-json`, `ok?`, `raise-for-status!`, and URL helpers
- cookbook examples under [`cookbook/net`](../../../../cookbook/net):
  `http-quickstart.eta`, `rest-client.eta`, `download-large-file.eta`
- package-local C++ unit tests and Eta smoke tests cover M3 plus M4 cases
  (timeout handling, TLS verify failure behavior, streaming download hash
  checks, parallel sessions, raise-for-status, URL encode/decode, JSON GET
  decode, and cookbook example execution) against the offline loopback
  fixture (`tests/fixtures/loopback_server.py`)

## Build and test

From repo root:

```powershell
cmake -S packages/net/native/http -B out/http-msvc `
  -DETA_CORE_LIBRARY="C:/Users/lewis/develop/eta/out/msvc-release/eta/core/eta_core.lib" `
  -DETA_ETAI_EXECUTABLE="C:/Users/lewis/develop/eta/out/msvc-release/eta/tools/interpreter/etai.exe" `
  -DETA_STDLIB_DIR="C:/Users/lewis/develop/eta/stdlib"
cmake --build out/http-msvc --config Release
ctest --test-dir out/http-msvc --output-on-failure
```

Host sidecar artifacts are emitted directly into `libs/<arch>/`:

- `libs/amd64/eta_http.dll` (Windows x64)
- `libs/amd64/libeta_http.so` (Linux x64)
- `libs/arm64/libeta_http.so` (Linux arm64)
- `libs/amd64/libeta_http.dylib` (macOS x64)
- `libs/arm64/libeta_http.dylib` (macOS arm64)
