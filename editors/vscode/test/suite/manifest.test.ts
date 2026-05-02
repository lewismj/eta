import * as assert from 'assert';
import * as vscode from 'vscode';

const EXTENSION_ID = 'eta-schema-lang.eta-scheme-lang';

type EtaManifest = {
    activationEvents?: string[];
    contributes?: {
        breakpoints?: Array<{ language?: string }>;
        commands?: Array<{ command?: string }>;
        configuration?: {
            properties?: Record<string, { default?: unknown }>;
        };
    };
};

describe('Eta manifest contributions', function () {
    this.timeout(10000);

    it('declares eta breakpoint contribution at root contributes level', () => {
        const ext = vscode.extensions.getExtension(EXTENSION_ID);
        assert.ok(ext, `extension ${EXTENSION_ID} not found`);

        const manifest = ext!.packageJSON as EtaManifest;
        const breakpoints = manifest.contributes?.breakpoints ?? [];
        assert.ok(
            breakpoints.some(bp => bp.language === 'eta'),
            'missing contributes.breakpoints entry for eta',
        );
    });

    it('activates on eta language and eta debug resolve', () => {
        const ext = vscode.extensions.getExtension(EXTENSION_ID);
        assert.ok(ext, `extension ${EXTENSION_ID} not found`);

        const manifest = ext!.packageJSON as EtaManifest;
        const activationEvents = manifest.activationEvents ?? [];
        assert.ok(
            activationEvents.includes('onLanguage:eta'),
            'missing activation event onLanguage:eta',
        );
        assert.ok(
            activationEvents.includes('onDebugResolve:eta'),
            'missing activation event onDebugResolve:eta',
        );
    });

    it('contributes environment inspector command and setting', () => {
        const ext = vscode.extensions.getExtension(EXTENSION_ID);
        assert.ok(ext, `extension ${EXTENSION_ID} not found`);

        const manifest = ext!.packageJSON as EtaManifest;
        const commands = manifest.contributes?.commands ?? [];
        assert.ok(
            commands.some(cmd => cmd.command === 'eta.showEnvironmentInspector'),
            'missing eta.showEnvironmentInspector command contribution',
        );

        const properties = manifest.contributes?.configuration?.properties ?? {};
        assert.ok(
            Object.prototype.hasOwnProperty.call(properties, 'eta.debug.autoShowEnvironment'),
            'missing eta.debug.autoShowEnvironment setting',
        );
        assert.strictEqual(
            properties['eta.debug.autoShowEnvironment']?.default,
            false,
            'eta.debug.autoShowEnvironment default should be false',
        );
    });
});
