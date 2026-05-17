# Cookbook Net Examples

These examples use the `eta-http` sidecar package and module `(import net.http)`.

- `http-quickstart.eta`: basic GET/POST flow and request-header roundtrip.
- `rest-client.eta`: session options, bearer token option, and JSON helpers.
- `download-large-file.eta`: stream a response directly to disk.

Package-local smoke tests run these examples against the loopback fixture at
`packages/net/native/http/tests/fixtures/loopback_server.py`.
