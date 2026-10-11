# Eta formatter

`eta_fmt` formats reader-valid Eta source using the shared lossless CST. It
changes whitespace only. The compiler reader independently validates the input
and output, including reader state, raw tokens, comments and discarded datums.
Malformed or unsupported input returns no formatted output and is never written.

Build `eta_all` or `eta_formatter`. CMake installs `eta_fmt` in `bin` with the
normal Eta runtime dependency bundle. The formatter and CST libraries are
internal static libraries, not a public SDK.

```text
eta_fmt --check stdlib cookbook
eta_fmt --diff cookbook/basics/args.eta
eta_fmt --stdout cookbook/basics/args.eta
eta_fmt --stdin --stdin-filename scratch.eta
eta_fmt --max-width 80 --indent 2 cookbook
eta_fmt --exclude '**/generated/**' --jobs 4 .
```

Without a mode, files are updated in place. Without paths, discovery starts at
the current directory. Directories select `.eta` files recursively; explicit
files may have any suffix. `.git`, `out`, `build` and `.eta` directory trees are
excluded. Repeatable exclude globs are relative to the invocation directory,
with `/` separators, `*` within one component and `**` across components.
Explicit files obey excludes too. No project style configuration is discovered.

`--check`, `--diff` and `--stdout` are exclusive. Stdout requires exactly one
explicit file. `--stdin` accepts no paths and writes to stdout unless check/diff
is selected. Its optional filename supplies diagnostic/diff identity and never
becomes a write target. Check produces no source output; diff produces ordered
whole-file unified hunks. Diagnostics and passthrough coverage go to stderr.
`--no-color` selects the plain diagnostics already used by this MVP.

| Exit | Meaning |
| --- | --- |
| 0 | Successful output/update, or no differences |
| 1 | Check/diff found differences |
| 2 | Read, syntax, unsupported-input or file-operation failure |
| 64 | Invalid usage; processing has not begun |
| 70 | Internal error, including a preservation invariant failure |

Processing aggregates `70 > 2 > 1 > 0`. Independent input failures do not stop
other files. Internal failures stop further writes. Atomicity is per file, so
earlier successful replacements are not rolled back. Worker concurrency is
bounded by `min(jobs, selected files, 64)`; publication and commits are ordered
by normalized relative display path. For aliases, the lexicographically first
display path wins after canonical file-identity deduplication.

Source must be valid UTF-8 with uniform LF or CRLF. Bare CR and mixed endings
are refused, including inside comments. Final-newline presence is preserved;
new breaks use LF if the input has no newlines. The current compiler rejects an
initial BOM, so BOM input is refused rather than stripped. Long raw tokens and
comments may exceed the width target. See [the style contract](eta_fmt_style.md)
for layouts and reported passthrough regions.

In-place operations never truncate the original. The formatter prepares and
validates the entire replacement first. Identical output performs no write and
preserves modification/creation times, including read-only inputs. A read-only
file requiring changes fails. Mutating file symlinks/reparse points and multiply
hard-linked files are refused. Read-only operations may resolve/deduplicate file
links. Directory symlinks/junctions are never traversed.

The shared file layer captures original bytes, file/parent identity and required
metadata. It rechecks them before replacement, writes and flushes a unique
temporary sibling, and uses the platform replacement operation. It reports
`not committed`, `committed but durability unconfirmed`, or `outcome unknown`
when an operation fails. Cleanup removes only temporary files whose captured
identity still matches. There is no truncating or copy-over fallback.

| Profile | Commit and metadata contract | Validation |
| --- | --- | --- |
| Windows local NTFS | Reopen without write sharing; retain read handle during `ReplaceFileW`; preserve DACL/ownership, creation time and named streams through required metadata merge; refuse special attributes | MSVC Release tests include injected failures and stale snapshots |
| Linux local ext4 | Retained parent descriptor, `openat`/`O_NOFOLLOW`, advisory lock, same-directory `renameat`, file/directory `fsync`; retain mode/uid/gid and ACL/xattrs | Implementation present; requires platform execution |
| macOS local APFS | Same descriptor/lock/rename protocol; retain mode/uid/gid and ACL/xattrs; portable birth-time preservation is not promised | Implementation present; requires platform execution |
| Other/network profiles | Mutation refused; read-only modes remain available | No replacement fallback |

Windows delete sharing leaves a narrow concurrent-path-replacement race; Unix
locks protect cooperating writers only. These protocols reject observed stale
snapshots and provide complete replacement visibility on the supported profile;
they do not promise a portable destination compare-and-swap or immunity to
uncooperative writers. Windows `ReplaceFileW` does not promise power-loss
durability. Unix directory-sync failure after rename reports that commit occurred.

The library takes explicit reader configuration, limits and shared cancellation.
Default limits remain 16 MiB input, 32 MiB output, depth 256, one million
tokens/nodes and 256 MiB accounted tooling memory. It returns `formatted`,
`passthrough`, or `refused`, resolved options and ordered coverage/diagnostics.
No library operation writes files, executes macros or loads native sidecars.

## Editor integration (LSP)

`eta_lsp` links `eta_format_lib` directly and owns editor formatting; it never
shells out to `eta_fmt`. The server advertises both `documentFormattingProvider`
and `documentRangeFormattingProvider`, so VS Code's standard `editor.formatOnSave`
and *Format Selection* route through the same library the CLI uses. There is no
competing subprocess save hook and no silent fallback to a standalone binary.

- Full formatting returns a whole-document replacement, or no edits when the
  document is already canonical or the result is `refused`. Syntax diagnostics
  continue to flow through the diagnostic channel.
- Range formatting is strict (§8.2.1): it only rewrites whole top-level reader
  units that lie entirely inside the requested range and begin at column zero.
  Each unit is replaced by exactly its own concrete-token extent, so outside
  indentation, inter-unit whitespace and trailing comments are byte-identical.
  Partial selections, state-sensitive slices and anything that would disturb a
  neighbour yield no edit for that unit.
- Formatting options map the editor `tabSize` to the canonical indent; the
  canonical style is space-only, so `insertSpaces=false` produces no edits.
  `eta.format.maxWidth` and `eta.format.indent` supply the server defaults
  through `initializationOptions`.

## Bootstrap

The Phase-4 formatter bootstrap is a separate formatting-only commit for the
`stdlib` and `cookbook` corpora (the checkout has no `demo` tree). It carries no
lint fixes or semantic changes. Its hash is recorded in `.git-blame-ignore-revs`
and referenced here once landed. CI (`.github/workflows/lint.yml`) keeps those
corpora formatter-clean afterwards with `eta_fmt --check stdlib cookbook`, and
`scripts/pre-commit-eta.sh` enforces the same contract on staged index bytes.


