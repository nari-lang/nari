# Editor Support for Nari

This document describes the editor tooling that currently exists in this repository.

## Current Layout

The current editor-related files are:

```text
lsp/lsp_server.cpp  # C++ language server implementation
lsp/builtins.d.nari  # builtin declarations used by LSP/editor features
lsp/extension/  # VS Code extension
lsp/extension/src/extension.ts  # VS Code extension entrypoint
lsp/extension/syntaxes/  # TextMate grammar for .nari files
lsp/extension/package.json  # VS Code contribution metadata
.vscode/  # Repository-local VS Code workspace settings
```

Older documentation may mention top-level `tree-sitter-nari/`, `vscode-nari/`, or `zed-nari/` directories. Those are not the current layout in this repo.

## Built Editor Binaries

A normal native build can produce three useful binaries:

```bash
./build.sh
./build.sh --release
```

Debug build outputs are under `build/debug/` and release build outputs are under `build/release/`:

```text
build/debug/nari  # Nari interpreter and DAP server via --dap
build/debug/naric  # bytecode compiler
build/debug/nari-lsp  # language server
build/release/nari
build/release/naric
build/release/nari-lsp
```

The VS Code extension and other editor integrations currently discover these binaries from the workspace build directories or from `PATH`.

## VS Code Extension

The VS Code extension lives in `lsp/extension/`.

### Features

- Syntax highlighting for `.nari` files through the TextMate grammar in `lsp/extension/syntaxes/`.
- Language configuration: brackets, comments, quotes, and related editor behavior.
- `nari-lsp` integration over stdio.
- Completion, hover, diagnostics, go-to-definition, references, document/workspace symbols, signature help, semantic tokens, code actions, and inlay hints as implemented by `lsp/lsp_server.cpp`.
- Debugging through the interpreter's DAP mode: `nari --dap`.
- Breakpoint support for `.nari` files.

### Development Setup

From `lsp/extension/`:

```bash
npm install
npm run compile
```

Then open the repo in VS Code and run/debug the extension from that folder, or symlink/copy `lsp/extension` into your VS Code extensions directory while developing.

The extension looks for `nari-lsp` in this order:

1. The `nari.lsp.serverPath` setting.
2. `build/debug/nari-lsp` or `build/release/nari-lsp` under any workspace folder.
3. `nari-lsp` on `PATH`.

The debugger looks for `nari` in this order:

1. The `nari.debug.interpreterPath` setting.
2. `build/release/nari` or `build/debug/nari` under any workspace folder.
3. `nari` on `PATH`.

On Windows, the `.exe` variants are also checked.

### Useful Settings

```json
{
  "nari.lsp.enable": true,
  "nari.lsp.serverPath": "/absolute/path/to/nari-lsp",
  "nari.lsp.inlayHints": false,
  "nari.debug.interpreterPath": "/absolute/path/to/nari"
}
```

Leave the path settings empty to use auto-discovery.

### Example Debug Configuration

```json
{
  "type": "nari",
  "request": "launch",
  "name": "Debug current Nari file",
  "program": "${file}",
  "stopOnEntry": true,
  "args": []
}
```

## Zed

If you use Zed, point its language-server/debugger configuration at the built binaries:

- `build/debug/nari-lsp` or `build/release/nari-lsp`
- `build/debug/nari` or `build/release/nari`

I'm still working on finishing a Zed extension, currently it's not 100% functional, so I haven't packaged it yet.

## LSP Logging

The current language server writes debug logs to `/tmp/nari-lsp.log` when logging is initialized. This is a development convenience and is tracked for cleanup: logging should become opt-in and platform-aware.

## Troubleshooting

### VS Code does not highlight `.nari` files

- Check that the file extension is `.nari`.
- Confirm the extension is installed or running in an Extension Development Host.
- Reload the VS Code window.
- Check `Output -> Log (Extension Host)` for activation or grammar errors.

### LSP features do not work

- Build the project so `nari-lsp` exists.
- Set `nari.lsp.serverPath` to an absolute path if auto-discovery fails.
- Verify `nari.lsp.enable` is `true`.
- Check `/tmp/nari-lsp.log` on Linux for current development logs.

### Debugging does not start

- Build the interpreter.
- Set `nari.debug.interpreterPath` to an absolute path if auto-discovery fails.
- Make sure your launch configuration uses `type: "nari"` and points `program` at a `.nari` or `.naric` file.

## Packaging Status

The current VS Code extension is development-ready but not yet a polished marketplace distribution. The main missing pieces are:

- explicit `onLanguage:nari` activation,
- bundled or auto-installed `nari-lsp` / interpreter binaries,
- marketplace release automation,
- extension versioning tied to Nari release artifacts,
- and broader DAP/LSP integration tests.

## Resources

- [VS Code Extension API](https://code.visualstudio.com/api)
- [Language Server Protocol](https://microsoft.github.io/language-server-protocol/)
- [Debug Adapter Protocol](https://microsoft.github.io/debug-adapter-protocol/)
- [TextMate Grammar Guide](https://macromates.com/manual/en/language_grammars)
- [Zed Extension Guide](https://zed.dev/docs/extensions)
