# std.os

Operating-system primitives.

```scheme
(import std.os)
```

| Symbol | Description |
| --- | --- |
| `(os:getenv name [default])` | Read an environment variable. |
| `(os:setenv! name value)` | Set an environment variable for the process. |
| `(os:unsetenv! name)` | Remove an environment variable. |
| `(os:environment-variables)` | Alist of all environment variables. |
| `(os:command-line-arguments)` | List of arguments passed after the script. |
| `(os:exit [code])` | Exit the process with `code` (default 0). |
| `(os:current-directory)` | Working directory as a string. |
| `(os:change-directory! path)` | Change the working directory. |

