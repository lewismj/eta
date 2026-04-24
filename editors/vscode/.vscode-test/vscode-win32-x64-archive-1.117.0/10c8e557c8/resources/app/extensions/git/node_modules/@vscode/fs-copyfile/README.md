# vscode-fs

Native `fs.copyFile` replacement for macOS with copy-on-write support using [`fclonefileat(2)`](https://www.manpagez.com/man/2/fclonefileat/).

When `COPYFILE_FICLONE_FORCE` is set, files are cloned via APFS copy-on-write. For all other modes, falls back to libuv `uv_fs_copyfile`.

## Install

```
npm install vscode-fs
```

Requires Node.js >= 22.6.0.

On macOS 12+, `COPYFILE_FICLONE_FORCE` uses native APFS copy-on-write cloning.
On Windows and Linux, the module defaults to Node.js `fs.copyFile` and `fs.cp`.

## Usage

```typescript
import { copyFile, copyFileSync, cp, isCloneSupported, constants } from 'vscode-fs';

// Async
await copyFile('src.txt', 'dst.txt', constants.COPYFILE_FICLONE_FORCE);

// Sync
copyFileSync('src.txt', 'dst.txt', constants.COPYFILE_FICLONE_FORCE);

// With EXCL (fail if dst exists)
copyFileSync('src.txt', 'dst.txt', constants.COPYFILE_FICLONE_FORCE | constants.COPYFILE_EXCL);

// Falls back to libuv for non-FICLONE_FORCE modes
copyFileSync('src.txt', 'dst.txt', constants.COPYFILE_FICLONE);
copyFileSync('src.txt', 'dst.txt');

// Recursive directory copy with CoW
await cp('src-dir', 'dst-dir', { recursive: true });

// Check if a path's volume supports CoW cloning
isCloneSupported('/path/to/file'); // true on APFS
```

## API

### `copyFile(src, dst, mode?): Promise<void>`

### `copyFileSync(src, dst, mode?): void`

### `cp(src, dest, options?): Promise<void>`

Recursively copy files and directories, using CoW cloning on APFS volumes. Based on Node.js `fs.cp`. Falls back to regular copy on cross-device or non-APFS volumes.

#### Options

| Option | Type | Default | Description |
|---|---|---|---|
| `recursive` | `boolean` | `false` | Copy directories recursively |
| `force` | `boolean` | `true` | Overwrite existing files |
| `errorOnExist` | `boolean` | `false` | Throw if dest exists and `force` is `false` |
| `preserveTimestamps` | `boolean` | `false` | Preserve timestamps (CoW clones always preserve regardless) |
| `dereference` | `boolean` | `false` | Dereference symlinks |
| `verbatimSymlinks` | `boolean` | `false` | Don't resolve relative symlink targets |
| `filter` | `(src, dest) => boolean \| Promise<boolean>` | — | Filter function; return `true` to copy |
| `mode` | `number` | `0` | `copyFile` mode flags (e.g. `COPYFILE_EXCL`) |

### `isCloneSupported(path): boolean`

Returns `true` if the volume containing `path` supports APFS cloning (`VOL_CAP_INT_CLONE`). Returns `false` for non-existent paths or non-APFS volumes.

### `constants`

| Constant | Value | Behavior |
|---|---|---|
| `COPYFILE_EXCL` | 1 | Fail if destination exists |
| `COPYFILE_FICLONE` | 2 | Best-effort clone (libuv fallback) |
| `COPYFILE_FICLONE_FORCE` | 4 | CoW clone via `fclonefileat`, fails if not supported |

## Benchmarks

Run benchmarks locally:

```
git lfs pull --include test/fixtures/node_modules.tar.gz --exclude ""
npm run bench
```

## License

MIT
