import * as assert from 'assert';
import * as vscode from 'vscode';

const EXTENSION_ID = 'eta-schema-lang.eta-scheme-lang';

type EtaManifest = {
    activationEvents?: string[];
    contributes?: {
        breakpoints?: Array<{ language?: string }>;
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
});
