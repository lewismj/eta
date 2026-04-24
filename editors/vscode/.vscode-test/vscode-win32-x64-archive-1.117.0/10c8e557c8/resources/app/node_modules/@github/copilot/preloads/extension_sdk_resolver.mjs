/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Microsoft Corporation. All rights reserved.
 *--------------------------------------------------------------------------------------------*/

// ESM loader hook for extension subprocesses.
// Intercepts `@github/copilot-sdk` imports and redirects them to the
// CLI's bundled copies.

import { resolve as resolvePath } from "node:path";
import { pathToFileURL } from "node:url";

// COPILOT_SDK_PATH points to the directory containing the bundled SDK files.
const sdkPath = process.env.COPILOT_SDK_PATH;

export async function resolve(specifier, context, nextResolve) {
    if (!sdkPath) {
        return nextResolve(specifier, context);
    }

    if (specifier === "@github/copilot-sdk") {
        const resolved = pathToFileURL(resolvePath(sdkPath, "index.js")).href;
        return { url: resolved, shortCircuit: true };
    }

    if (specifier === "@github/copilot-sdk/extension") {
        const resolved = pathToFileURL(resolvePath(sdkPath, "extension.js")).href;
        return { url: resolved, shortCircuit: true };
    }

    return nextResolve(specifier, context);
}
