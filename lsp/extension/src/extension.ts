import * as fs from 'fs';
import * as path from 'path';
import * as os from 'os';
import {
    workspace,
    window,
    debug,
    DebugAdapterDescriptor,
    DebugAdapterDescriptorFactory,
    DebugAdapterExecutable,
    DebugConfiguration,
    DebugConfigurationProvider,
    DebugSession,
    ExtensionContext,
    ProviderResult,
    TextDocumentChangeEvent,
    WorkspaceFolder,
} from 'vscode';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    TransportKind,
} from 'vscode-languageclient/node';
import { getExtendMarkdownIt } from './markdown-plugin';

let client: LanguageClient | undefined;

/*
    Try to locate the nari-lsp binary. Order of preference:
        1) nari.lsp.serverPath setting (absolute path)
        2) A debug build next to the workspace root
        3) nari-lsp on $PATH
 */
function findServerBinary(_context: ExtensionContext): string | undefined {
    const config = workspace.getConfiguration('nari');
    const configPath: string = config.get<string>('lsp.serverPath', '').trim();
    if (configPath && fs.existsSync(configPath)) {
        return configPath;
    }

    // Look for build/debug/nari-lsp relative to every workspace folder
    for (const folder of workspace.workspaceFolders ?? []) {
        const candidates = [
            path.join(folder.uri.fsPath, 'build', 'debug', 'nari-lsp'),
            path.join(folder.uri.fsPath, 'build', 'release', 'nari-lsp'),
            // Windows
            path.join(folder.uri.fsPath, 'build', 'debug', 'nari-lsp.exe'),
            path.join(folder.uri.fsPath, 'build', 'release', 'nari-lsp.exe'),
        ];
        for (const c of candidates) {
            if (fs.existsSync(c)) return c;
        }
    }

    // Fall back to PATH
    const pathDirs = (process.env.PATH ?? '').split(path.delimiter);
    for (const dir of pathDirs) {
        const bin = path.join(dir, os.platform() === 'win32' ? 'nari-lsp.exe' : 'nari-lsp');
        if (fs.existsSync(bin)) return bin;
    }

    return undefined;
}

/* 
    Locate the interpreter binary used for --dap debug sessions.
        Same search order as the LSP server but looking for `interpreter` instead of
        `nari-lsp`. `nari.debug.interpreterPath` is used to override auto-detection.
 */
function findInterpreterBinary(): string | undefined {
    const config = workspace.getConfiguration('nari');
    const configPath: string = config.get<string>('debug.interpreterPath', '').trim();
    if (configPath && fs.existsSync(configPath)) {
        return configPath;
    }

    for (const folder of workspace.workspaceFolders ?? []) {
        const candidates = [
            path.join(folder.uri.fsPath, 'build', 'release', 'interpreter'),
            path.join(folder.uri.fsPath, 'build', 'debug', 'interpreter'),
            path.join(folder.uri.fsPath, 'build', 'release', 'interpreter.exe'),
            path.join(folder.uri.fsPath, 'build', 'debug', 'interpreter.exe'),
        ];
        for (const c of candidates) {
            if (fs.existsSync(c)) return c;
        }
    }

    const pathDirs = (process.env.PATH ?? '').split(path.delimiter);
    for (const dir of pathDirs) {
        const bin = path.join(dir, os.platform() === 'win32' ? 'interpreter.exe' : 'interpreter');
        if (fs.existsSync(bin)) return bin;
    }

    return undefined;
}

/*
    Debug adapter descriptor factory for the `nari` debug type.
    Spawns `interpreter --dap` per session. Communication is stdin/stdout
    framed with Content-Length headers, identical to LSP format.
 */
class NariDebugAdapterFactory implements DebugAdapterDescriptorFactory {
    createDebugAdapterDescriptor(
        _session: DebugSession,
        _executable: DebugAdapterExecutable | undefined,
    ): ProviderResult<DebugAdapterDescriptor> {
        const bin = findInterpreterBinary();
        if (!bin) {
            window.showErrorMessage(
                'Nari Debugger: could not find the interpreter binary. ' +
                'Build the project or set "nari.debug.interpreterPath" in settings.',
            );
            return undefined;
        }
        return new DebugAdapterExecutable(bin, ['--dap']);
    }
}

/**
 * Resolve a (possibly bare) launch configuration into a fully-populated one.
 * Handles the "no launch.json present" case where the user just hits F5 on
 * an open Nari file, we synthesise a sensible default.
 */
class NariDebugConfigProvider implements DebugConfigurationProvider {
    resolveDebugConfiguration(
        _folder: WorkspaceFolder | undefined,
        config: DebugConfiguration,
    ): ProviderResult<DebugConfiguration> {
        if (!config.type && !config.request && !config.name) {
            const editor = window.activeTextEditor;
            if (editor && editor.document.languageId === 'nari') {
                config.type = 'nari';
                config.name = 'Launch';
                config.request = 'launch';
                config.program = editor.document.uri.fsPath;
                config.stopOnEntry = true;
            }
        }
        if (!config.program) {
            window.showInformationMessage('Nari Debugger: no `program` field in the launch configuration.',);
            return undefined;
        }
        return config;
    }
}

export function activate(context: ExtensionContext) {
    // Debugger registration happens unconditionally,
    // however there's no real cost until the user actually uses it
    context.subscriptions.push(
        debug.registerDebugAdapterDescriptorFactory(
            'nari',
            new NariDebugAdapterFactory(),
        ),
        debug.registerDebugConfigurationProvider(
            'nari',
            new NariDebugConfigProvider(),
        ),
    );

    const config = workspace.getConfiguration('nari');
    if (!config.get<boolean>('lsp.enable', true)) {
        return;
    }

    const serverBin = findServerBinary(context);
    if (!serverBin) {
        window.showWarningMessage(
            'Nari LSP: Could not find the nari-lsp binary. ' +
            'Build the project or set "nari.lsp.serverPath" in settings.'
        );
        return;
    }

    const serverOptions: ServerOptions = {
        command: serverBin,
        transport: TransportKind.stdio,
    };

    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: 'file', language: 'nari' }],
        synchronize: {
            fileEvents: workspace.createFileSystemWatcher('**/*.nari'),
        },
        middleware: {
            // Suppress inlay hints when the user has disabled them in settings.
            // Default is disabled, the user must opt in via nari.lsp.inlayHints.
            provideInlayHints: (document, viewPort, token, next) => {
                const cfg = workspace.getConfiguration('nari');
                if (!cfg.get<boolean>('lsp.inlayHints', false)) {
                    return [];
                }
                return next(document, viewPort, token);
            },
            // debounce didChange notifications so diagnostics don't fire on every keystroke mid-typing
            didChange: (() => {
                const timers = new Map<string, ReturnType<typeof setTimeout>>();
                return (event: TextDocumentChangeEvent, next: (e: TextDocumentChangeEvent) => Promise<void>): Promise<void> => {
                    const uri = event.document.uri.toString();
                    const existing = timers.get(uri);
                    if (existing !== undefined) clearTimeout(existing);
                    timers.set(uri, setTimeout(() => {
                        timers.delete(uri);
                        next(event);
                    }, 300));
                    // Resolve immediately; next() fires after the debounce window.
                    return Promise.resolve();
                };
            })(),
        },
    };

    client = new LanguageClient(
        'nariLanguageServer',
        'Nari Language Server',
        serverOptions,
        clientOptions,
    );

    client.start();

    return {
		extendMarkdownIt: getExtendMarkdownIt(context),
	};
}

export function deactivate(): Thenable<void> | undefined {
    return client?.stop();
}
