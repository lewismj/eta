# eta-ui (bundled stdlib sidecar)

Native support for `std.ui`: a localhost HTTP + WebSocket server, built on
the NNG already linked into the runtime, that serves the front-end bundle and
carries the Eta UI protocol.

| Primitive | Purpose |
|-----------|---------|
| `(%ui-start app-id)` | Start the server (once per process); returns the page URL |
| `(%ui-send value retain?)` | Serialise `value` and send it to every page |
| `(%ui-recv timeout-ms)` | Next message from a page (JSON string) or `#f` |
| `(%ui-open url)` | Open the URL in the system browser |
| `(%ui-stop)` | Close all pages and stop listening |
| `(%ui-client-count)` | Number of connected pages |

User code should use `std.ui` and `std.chart`, not these primitives.

## Behaviour

- **Localhost only**, with a random session token in the URL; WebSocket
  connections without the token are closed.
- **Stable port per app id.** The port and token are remembered in the UI
  config directory (`%APPDATA%\eta\ui` or `~/.config/eta/ui`). A page left
  open from a previous run reconnects to the next run and animates to its
  data. If the port is still held by a previous run, the new process asks it
  to quit and takes the port over.
- **Discovery:** `<config>/eta/ui/<app-id>.url` holds the current URL; the
  VS Code command *Eta: Open App* reads it.
- **Binary buffers:** vectors of 16 or more numbers are sent as Float64
  buffers instead of JSON.

## Front end

`web/` is built from `eta/tools/ui/frontend` by the CMake target
`eta_ui_frontend`. Set `ETA_UI_DEV_DIR` to serve a different build.
