// Based on Node.js lib/internal/fs/cp/cp.js, backed by native CoW clone.
//
// When the source volume supports cloning (APFS), files are copied via
// fclonefileat, falls back to regular copy on cross-device or non-APFS volumes.
import { cp as nodeCp, stat, lstat, mkdir, opendir, readlink, symlink, unlink, utimes, chmod, } from 'node:fs/promises';
import { dirname, isAbsolute, join, parse, resolve, sep } from 'node:path';
import { copyFile as nativeCopyFile, isCloneSupported, constants, isMacOS, } from "./native.js";
function areIdentical(a, b) {
    return Boolean(b.ino) && Boolean(b.dev) &&
        b.ino === a.ino && b.dev === a.dev;
}
function isSrcSubdir(src, dest) {
    const srcArr = resolve(src).split(sep).filter(Boolean);
    const destArr = resolve(dest).split(sep).filter(Boolean);
    return srcArr.every((cur, i) => destArr[i] === cur);
}
function cpError(code, message, path) {
    const err = new Error(`${code}: ${message}`);
    err.code = code;
    err.path = path;
    err.syscall = 'cp';
    return err;
}
const DIR_COPY_CONCURRENCY = 64;
function canUseNativeDirectoryClone(opts, useClone) {
    // Native directory clone preserves symlink payloads as-is.
    // That is only semantically compatible when verbatimSymlinks is enabled.
    return useClone
        && opts.recursive
        && !opts.filter
        && !opts.dereference
        && opts.verbatimSymlinks
        && !opts.preserveTimestamps
        && opts.mode === 0;
}
async function tryNativeCloneDirectory(src, dest) {
    try {
        await nativeCopyFile(src, dest, constants.COPYFILE_FICLONE_FORCE);
        return true;
    }
    catch (err) {
        if (err.code === 'EXDEV' || err.code === 'ENOTSUP' || err.code === 'EINVAL' || err.code === 'ENOENT') {
            return false;
        }
        throw err;
    }
}
function defaults(options) {
    return {
        force: true,
        errorOnExist: false,
        preserveTimestamps: false,
        recursive: false,
        dereference: false,
        verbatimSymlinks: false,
        mode: 0,
        ...options,
    };
}
export async function cp(src, dest, options) {
    if (!isMacOS) {
        return nodeCp(src, dest, options);
    }
    const opts = defaults(options);
    const useClone = isCloneSupported(src);
    const { srcStat, destStat, skipped } = await asyncCheckPaths(src, dest, opts);
    if (skipped)
        return;
    if (srcStat.isDirectory() && !destStat && canUseNativeDirectoryClone(opts, useClone)) {
        if (await tryNativeCloneDirectory(src, dest)) {
            return;
        }
    }
    await asyncCheckParentPaths(src, srcStat, dest);
    await asyncCheckParentDir(srcStat, destStat, src, dest, opts, useClone);
}
async function asyncGetStats(src, dest, opts, skipDestStat = false) {
    const statFn = opts.dereference
        ? (f) => stat(f, { bigint: true })
        : (f) => lstat(f, { bigint: true });
    const srcStat = await statFn(src);
    let destStat;
    if (skipDestStat) {
        return [srcStat, null];
    }
    try {
        destStat = await statFn(dest);
    }
    catch (err) {
        if (err.code === 'ENOENT')
            destStat = null;
        else
            throw err;
    }
    return [srcStat, destStat];
}
async function asyncCheckPaths(src, dest, opts, skipDestStat = false) {
    if (opts.filter && !(await opts.filter(src, dest))) {
        return { srcStat: null, destStat: null, skipped: true };
    }
    const [srcStat, destStat] = await asyncGetStats(src, dest, opts, skipDestStat);
    if (destStat) {
        if (areIdentical(srcStat, destStat)) {
            throw cpError('ERR_FS_CP_EINVAL', 'src and dest cannot be the same', dest);
        }
        if (srcStat.isDirectory() && !destStat.isDirectory()) {
            throw cpError('ERR_FS_CP_DIR_TO_NON_DIR', `cannot overwrite non-directory ${dest} with directory ${src}`, dest);
        }
        if (!srcStat.isDirectory() && destStat.isDirectory()) {
            throw cpError('ERR_FS_CP_NON_DIR_TO_DIR', `cannot overwrite directory ${dest} with non-directory ${src}`, dest);
        }
    }
    if (srcStat.isDirectory() && isSrcSubdir(src, dest)) {
        throw cpError('ERR_FS_CP_EINVAL', `cannot copy ${src} to a subdirectory of self ${dest}`, dest);
    }
    return { srcStat, destStat, skipped: false };
}
async function asyncCheckParentPaths(src, srcStat, dest) {
    const srcParent = resolve(dirname(src));
    const destParent = resolve(dirname(dest));
    if (destParent === srcParent || destParent === parse(destParent).root)
        return;
    let destStat;
    try {
        destStat = await stat(destParent, { bigint: true });
    }
    catch (err) {
        if (err.code === 'ENOENT')
            return;
        throw err;
    }
    if (areIdentical(srcStat, destStat)) {
        throw cpError('ERR_FS_CP_EINVAL', `cannot copy ${src} to a subdirectory of self ${dest}`, dest);
    }
    return asyncCheckParentPaths(src, srcStat, destParent);
}
async function asyncCheckParentDir(srcStat, destStat, src, dest, opts, useClone) {
    const destParent = dirname(dest);
    let exists;
    try {
        await stat(destParent);
        exists = true;
    }
    catch (err) {
        exists = err.code !== 'ENOENT' ? true : false;
        if (err.code !== 'ENOENT')
            throw err;
    }
    if (!exists)
        await mkdir(destParent, { recursive: true });
    return asyncGetStatsForCopy(srcStat, destStat, src, dest, opts, useClone, Boolean(destStat));
}
async function asyncGetStatsForCopy(srcStat, destStat, src, dest, opts, useClone, destExistsKnown) {
    if (srcStat.isDirectory()) {
        if (!opts.recursive) {
            throw cpError('ERR_FS_EISDIR', `${src} is a directory (not copied)`, src);
        }
        return asyncOnDir(srcStat, destStat, src, dest, opts, useClone, destExistsKnown);
    }
    if (srcStat.isFile() || srcStat.isCharacterDevice() || srcStat.isBlockDevice()) {
        return asyncOnFile(srcStat, destStat, src, dest, opts, useClone);
    }
    if (srcStat.isSymbolicLink()) {
        return asyncOnLink(destStat, src, dest, opts);
    }
    if (srcStat.isSocket()) {
        throw cpError('ERR_FS_CP_SOCKET', `cannot copy a socket file: ${dest}`, dest);
    }
    if (srcStat.isFIFO()) {
        throw cpError('ERR_FS_CP_FIFO_PIPE', `cannot copy a FIFO pipe: ${dest}`, dest);
    }
    throw cpError('ERR_FS_CP_UNKNOWN', `cannot copy an unknown file type: ${dest}`, dest);
}
async function asyncOnFile(srcStat, destStat, src, dest, opts, useClone) {
    if (!destStat)
        return asyncCopyFile(srcStat, src, dest, opts, useClone);
    return asyncMayCopyFile(srcStat, src, dest, opts, useClone);
}
async function asyncMayCopyFile(srcStat, src, dest, opts, useClone) {
    if (opts.force) {
        await unlink(dest);
        return asyncCopyFile(srcStat, src, dest, opts, useClone);
    }
    if (opts.errorOnExist) {
        throw cpError('ERR_FS_CP_EEXIST', `${dest} already exists`, dest);
    }
}
async function asyncCopyFile(srcStat, src, dest, opts, useClone) {
    if (useClone) {
        try {
            await nativeCopyFile(src, dest, constants.COPYFILE_FICLONE_FORCE);
            return;
        }
        catch (err) {
            if (err.code !== 'EXDEV')
                throw err;
            // Cross-device: fall through to regular copy.
        }
    }
    await nativeCopyFile(src, dest, opts.mode);
    if (opts.preserveTimestamps) {
        await asyncHandleTimestampsAndMode(srcStat.mode, src, dest);
    }
    else {
        await chmod(dest, Number(srcStat.mode));
    }
}
async function asyncHandleTimestampsAndMode(srcMode, src, dest) {
    const mode = Number(srcMode);
    if ((mode & 0o200) === 0) {
        await chmod(dest, mode | 0o200);
    }
    const updatedSrcStat = await stat(src);
    await utimes(dest, updatedSrcStat.atime, updatedSrcStat.mtime);
    await chmod(dest, mode);
}
async function asyncOnDir(srcStat, destStat, src, dest, opts, useClone, destExistsKnown) {
    if (!destStat) {
        await mkdir(dest);
        await asyncCopyDir(src, dest, opts, useClone, false);
        await chmod(dest, Number(srcStat.mode));
        return;
    }
    if (opts.errorOnExist && !opts.force) {
        throw cpError('ERR_FS_CP_EEXIST', `${dest} already exists`, dest);
    }
    return asyncCopyDir(src, dest, opts, useClone, destExistsKnown);
}
async function asyncCopyDir(src, dest, opts, useClone, destExistsKnown) {
    const dir = await opendir(src);
    const copyOne = async (entry) => {
        const srcItem = join(src, entry.name);
        const destItem = join(dest, entry.name);
        if (!destExistsKnown) {
            if (opts.filter && !(await opts.filter(srcItem, destItem))) {
                return;
            }
            if (!opts.dereference) {
                await asyncCopyDirentFast(entry, srcItem, destItem, opts, useClone);
                return;
            }
            const [srcStat] = await asyncGetStats(srcItem, destItem, opts, true);
            await asyncGetStatsForCopy(srcStat, null, srcItem, destItem, opts, useClone, false);
            return;
        }
        const { srcStat, destStat, skipped } = await asyncCheckPaths(srcItem, destItem, opts);
        if (!skipped) {
            await asyncGetStatsForCopy(srcStat, destStat, srcItem, destItem, opts, useClone, Boolean(destStat));
        }
    };
    if (opts.filter) {
        for await (const entry of dir) {
            await copyOne(entry);
        }
        return;
    }
    const inFlight = new Set();
    for await (const entry of dir) {
        let task;
        task = copyOne(entry).finally(() => {
            inFlight.delete(task);
        });
        inFlight.add(task);
        if (inFlight.size >= DIR_COPY_CONCURRENCY) {
            await Promise.race(inFlight);
        }
    }
    await Promise.all(inFlight);
}
async function asyncCopyDirentFast(entry, src, dest, opts, useClone) {
    if (entry.isDirectory()) {
        await mkdir(dest);
        await asyncCopyDir(src, dest, opts, useClone, false);
        const srcStat = await lstat(src, { bigint: true });
        await chmod(dest, Number(srcStat.mode));
        return;
    }
    if (entry.isFile() || entry.isCharacterDevice() || entry.isBlockDevice()) {
        if (useClone) {
            try {
                await nativeCopyFile(src, dest, constants.COPYFILE_FICLONE_FORCE);
                return;
            }
            catch (err) {
                if (err.code !== 'EXDEV')
                    throw err;
            }
        }
        await nativeCopyFile(src, dest, opts.mode);
        const srcStat = await lstat(src, { bigint: true });
        if (opts.preserveTimestamps) {
            await asyncHandleTimestampsAndMode(srcStat.mode, src, dest);
        }
        else {
            await chmod(dest, Number(srcStat.mode));
        }
        return;
    }
    if (entry.isSymbolicLink()) {
        await asyncOnLink(null, src, dest, opts);
        return;
    }
    if (entry.isSocket()) {
        throw cpError('ERR_FS_CP_SOCKET', `cannot copy a socket file: ${dest}`, dest);
    }
    if (entry.isFIFO()) {
        throw cpError('ERR_FS_CP_FIFO_PIPE', `cannot copy a FIFO pipe: ${dest}`, dest);
    }
    const [srcStat] = await asyncGetStats(src, dest, opts, true);
    await asyncGetStatsForCopy(srcStat, null, src, dest, opts, useClone, false);
}
async function asyncOnLink(destStat, src, dest, opts) {
    let resolvedSrc = await readlink(src);
    if (!opts.verbatimSymlinks && !isAbsolute(resolvedSrc)) {
        resolvedSrc = resolve(dirname(src), resolvedSrc);
    }
    if (!destStat) {
        return symlink(resolvedSrc, dest).then(() => { });
    }
    let resolvedDest;
    try {
        resolvedDest = await readlink(dest);
    }
    catch (err) {
        if (err.code === 'EINVAL' || err.code === 'UNKNOWN') {
            return symlink(resolvedSrc, dest).then(() => { });
        }
        throw err;
    }
    if (!isAbsolute(resolvedDest)) {
        resolvedDest = resolve(dirname(dest), resolvedDest);
    }
    let srcIsDir;
    try {
        srcIsDir = (await stat(src)).isDirectory();
    }
    catch {
        srcIsDir = false;
    }
    if (srcIsDir && isSrcSubdir(resolvedSrc, resolvedDest)) {
        throw cpError('ERR_FS_CP_EINVAL', `cannot copy ${resolvedSrc} to a subdirectory of self ${resolvedDest}`, dest);
    }
    const srcStat = await stat(src);
    if (srcStat.isDirectory() && isSrcSubdir(resolvedDest, resolvedSrc)) {
        throw cpError('ERR_FS_CP_SYMLINK_TO_SUBDIRECTORY', `cannot overwrite ${resolvedDest} with ${resolvedSrc}`, dest);
    }
    await unlink(dest);
    return symlink(resolvedSrc, dest).then(() => { });
}
