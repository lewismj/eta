# std.fs

Filesystem and path primitives. Re-exports of the runtime `%fs-*` built-ins.

```scheme
(import std.fs)
```

| Symbol | Description |
| --- | --- |
| `(fs:file-exists? path)` | True when `path` exists. |
| `(fs:directory? path)` | True when `path` is a directory. |
| `(fs:delete-file path)` | Delete a file. |
| `(fs:make-directory path)` | Create a directory. |
| `(fs:list-directory path)` | List directory entries (file names). |
| `(fs:path-join a b ...)` | Join path components using the platform separator. |
| `(fs:path-split path)` | Split into `(directory . basename)`. |
| `(fs:path-normalize path)` | Canonicalise separators and `.`/`..` segments. |
| `(fs:temp-file [prefix])` | Create a unique temporary file and return its path. |
| `(fs:temp-directory [prefix])` | Create a unique temporary directory. |
| `(fs:file-modification-time path)` | Modification time as Unix epoch seconds. |
| `(fs:file-size path)` | Size in bytes. |

