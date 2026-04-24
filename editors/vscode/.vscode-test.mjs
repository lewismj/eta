// .vscode-test.mjs — config for `vscode-test` CLI (C1 extension test suite).
import { defineConfig } from '@vscode/test-cli';

export default defineConfig({
    files: 'out/test/suite/**/*.test.js',
    workspaceFolder: 'test/fixtures',
    extensionDevelopmentPath: '.',
    mocha: {
        ui: 'bdd',
        timeout: 30000,
        color: true,
    },
});

