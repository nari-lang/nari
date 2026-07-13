# Editor Support for Nari

This document describes the editor tooling that currently exists in this repository.

## Current Layout

The current editor-related files are:

```text
.vscode/  # Repository-local VS Code workspace settings
lsp/lsp_server.cpp  # C++ language server implementation
lsp/builtins.d.nari  # builtin declarations used by LSP/editor features
```

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

## Visual Studio Code

The current VS Code extension is development-ready but not yet a polished marketplace distribution. It is available for use and has the setup instructions
at [this repo](https://github.com/nari-lang/vscode-nari).

## Zed

If you use Zed, point its language-server/debugger configuration at the built binaries:

- `build/debug/nari-lsp` or `build/release/nari-lsp`
- `build/debug/nari` or `build/release/nari`

The Zed Extension is still being worked on and is not 100% functional, so it has not been packagedyet.

## LSP Logging

The current language server writes debug logs to `/tmp/nari-lsp.log` when logging is initialized. This is a development convenience and is tracked for cleanup: logging should become opt-in and platform-aware.