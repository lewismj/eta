import { createRequire } from 'node:module';
import { copyFile as nodeCopyFile } from 'node:fs/promises';
import { copyFileSync as nodeCopyFileSync, constants as nodeConstants } from 'node:fs';
const require = createRequire(import.meta.url);
export const isMacOS = process.platform === 'darwin';
let binding;
if (isMacOS) {
    try {
        binding = require('../build/Release/vscode_fs.node');
    }
    catch {
        binding = require('../build/Debug/vscode_fs.node');
    }
}
export const copyFile = isMacOS ? binding.copyFile : nodeCopyFile;
export const copyFileSync = isMacOS ? binding.copyFileSync : nodeCopyFileSync;
export const isCloneSupported = isMacOS ? binding.isCloneSupported : () => false;
export const constants = isMacOS ? binding.constants : {
    COPYFILE_EXCL: nodeConstants.COPYFILE_EXCL,
    COPYFILE_FICLONE: nodeConstants.COPYFILE_FICLONE,
    COPYFILE_FICLONE_FORCE: nodeConstants.COPYFILE_FICLONE_FORCE,
};
