# std.actor.node

Distributed actor-node transport helpers over NNG.

```scheme
(import std.actor.node)
```

`std.actor.node` manages node handshake and routing. It is complementary to:

- `std.actor` for PID/mailbox actor APIs
- `std.net` for raw transport socket workflows

| Symbol | Description |
| --- | --- |
| `(node-name)` | Return local node name used in handshake metadata. |
| `(node-listen endpoint . opts)` | Start listening for one remote node on `endpoint`. |
| `(node-connect endpoint . opts)` | Connect to one remote node endpoint. |
| `(nodes)` | Return connected node entries `(node-name node-id endpoint)`. |
| `(disconnect-node node-name)` | Disconnect one node by name. |

Supported `node-listen` / `node-connect` options:

- `'node-name` or `'name` (string or symbol)
- `'cookie` (string or symbol)

When overriding node identity with options, provide both `node-name`/`name`
and `cookie` in the same call.
