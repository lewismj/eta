# std.args

argparse-style command-line parser. Argument specifications are tuples of
`(name kind . opts)`.

```scheme
(import std.args)
```

| Symbol | Description |
| --- | --- |
| `(args:parse spec argv)` | Parse `argv` against `spec`. Returns a hash map of values. |
| `(args:parse-command-line spec)` | Convenience: parse the current process arguments. |
| `(args:get parsed name)` | Look up a parsed value by name. |
| `(args:help spec)` | Render a help string from `spec`. |

A spec entry has the form:

```scheme
(name kind 'help "..." 'default v 'short "x" 'choices '(...))
```

`kind` is one of `'string`, `'int`, `'real`, `'bool`, `'flag`, `'list`,
`'positional`.

