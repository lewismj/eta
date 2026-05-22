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
| `(monitor-node node-name)` | Monitor one remote node name; returns monitor ref. |
| `(node-listen endpoint . opts)` | Start listening for one remote node on `endpoint`. |
| `(node-connect endpoint . opts)` | Connect to one remote node endpoint. |
| `(nodes)` | Return connected node entries `(node-name node-id endpoint)`. |
| `(disconnect-node node-name)` | Disconnect one node by name. |

Supported `node-listen` / `node-connect` options:

- `'node-name` or `'name` (string or symbol)
- `'cookie` (string or symbol)

When overriding node identity with options, provide both `node-name`/`name`
and `cookie` in the same call.

Node monitor messages:

```scheme
'(nodeup ref node-name node-id)
'(nodedown ref node-name reason)
```

Distributed monitor and netsplit semantics:

- Remote process monitors deliver one `'(DOWN ref process pid reason)` message.
- Node loss uses reason `'noconnection` for remote process `DOWN` delivery.
- Node monitor `nodedown` uses reason `'noconnection` on disconnect and
  `'bad-cookie` on rejected cookie handshakes.
- `demonitor` with flush (`#t`) removes queued monitor events for that ref,
  including remote `DOWN` and node monitor lifecycle messages.
