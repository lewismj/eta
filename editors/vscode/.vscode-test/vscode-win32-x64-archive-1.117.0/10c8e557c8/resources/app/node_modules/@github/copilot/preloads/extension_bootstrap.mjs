/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Microsoft Corporation. All rights reserved.
 *--------------------------------------------------------------------------------------------*/

// Bootstrap script for extension subprocesses.
// This is the fork target — it sets up the SDK resolver in-process,
// then dynamically imports the user's extension via the EXTENSION_PATH env var.

import Module from "node:module";
import { resolve as resolvePath } from "node:path";
import { pathToFileURL } from "node:url";

// Register the ESM SDK resolver hook before loading the extension
Module.register("./extension_sdk_resolver.mjs", import.meta.url);

// Register a CJS require hook so that CommonJS extensions can
// `require("@github/copilot-sdk")` and have it resolve to the bundled SDK.
const sdkPath = process.env.COPILOT_SDK_PATH;
if (sdkPath) {
    const originalResolveFilename = Module._resolveFilename;
    Module._resolveFilename = function (request, parent, isMain, options) {
        if (request === "@github/copilot-sdk") {
            return resolvePath(sdkPath, "index.js");
        }
        if (request === "@github/copilot-sdk/extension") {
            return resolvePath(sdkPath, "extension.js");
        }
        return originalResolveFilename.call(this, request, parent, isMain, options);
    };
}

// Load the user's extension
const extensionPath = process.env.EXTENSION_PATH;
if (!extensionPath) {
    console.error("[extension-bootstrap] No extension path provided");
    process.exit(1);
}

try {
    await import(pathToFileURL(extensionPath).href);
} catch (err) {
    console.error(`[extension-bootstrap] Failed to load extension: ${err}`);
    process.exit(1);
}
