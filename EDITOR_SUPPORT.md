# Tree-sitter Grammar and Editor Support for Nari

This directory contains Tree-sitter grammar and editor integrations for Nari.

## Directory Structure

```
tree-sitter-nari/     # Tree-sitter grammar
vscode-nari/          # VSCode extension
zed-nari/             # Zed editor extension
```

## Setup Instructions

### Tree-sitter Grammar

The Tree-sitter grammar is located in `tree-sitter-nari/`.

#### Prerequisites
```bash
npm install -g tree-sitter-cli
```

#### Building the Grammar

```bash
cd tree-sitter-nari
npm install
tree-sitter generate
```

#### Testing the Grammar

```bash
tree-sitter test
tree-sitter parse ../tests/expect_pass/test_features.nari
```

### VSCode Extension

Located in `vscode-nari/`.

#### Installation

```bash
# Copy/symlink the extension folder to VSCode extensions directory

# Linux/Mac:
ln -s $(pwd)/vscode-nari ~/.vscode/extensions/nari-0.1.0

# Windows:
mklink /D "%USERPROFILE%\.vscode\extensions\nari-0.1.0" "%CD%\vscode-nari"

# Restart VSCode
```

#### Features
- Syntax highlighting
- Auto-closing brackets and quotes
- Comment toggling (Ctrl+/)
- Code folding

### Zed Editor Extension

Located in `zed-nari/`.

#### Installation

Ctrl + P -> 'install dev extension' -> pick `zed-nari`.

## Language Features

The grammar supports all Nari language features:

### Keywords
- `func`, `let`, `global`
- `if`, `else`, `while`, `for`, `in`
- `switch`, `case`, `default`
- `return`, `break`, `continue`
- `throw`, `try`, `catch`, `finally`
- `menu`, `import`

### Operators
- Arithmetic: `+`, `-`, `*`, `/`, `%`, `**`
- Comparison: `==`, `!=`, `<`, `>`, `<=`, `>=`
- Logical: `&&`, `||`, `!`
- Special: `@` (concatenation), `??` (nullish coalescing)
- Update: `++`, `--`

### Literals
- Numbers: `123`, `3.14`
- Strings: `"double"`, `'single'`, `` `interpolated {expr}` ``
- Booleans: `true`, `false`
- Null: `null`
- Arrays: `[1, 2, 3]`
- Objects: `{key: value}`

### Comments
- Line comments: `// comment`

## Customization

### Adding More Highlighting Rules

Edit `tree-sitter-nari/queries/highlights.scm` and add new patterns:

```scheme
; Example: Highlight built-in functions differently
(call_expression
  function: (identifier) @function.builtin
  (#match? @function.builtin "^(print|length|push|pop)$"))
```

### Adjusting Colors

For VSCode, create a `.vscode/settings.json`:

```json
{
  "editor.tokenColorCustomizations": {
    "textMateRules": [
      {
        "scope": "keyword.control.nari",
        "settings": {
          "foreground": "#C678DD"
        }
      }
    ]
  }
}
```

For Zed, edit your Zed settings and customize the theme.

## Publishing

### VSCode Marketplace

```bash
cd vscode-nari
vsce publish
```

### Zed Extensions

Submit a pull request to the Zed extensions repository or publish to your own repository.

## Development

### Testing Changes

1. Modify `grammar.js`
2. Run `tree-sitter generate`
3. Test with `tree-sitter parse <test-file.nari>`
4. Update highlight queries if needed
5. Reload your editor

### Common Issues

**VSCode not highlighting:**
- Check the file extension is `.nari`
- Reload window (Ctrl+Shift+P → "Reload Window")
- Check Output → Log (Extension Host) for errors

**Zed not working:**
- Ensure Tree-sitter grammar is compiled
- Check Zed logs: `~/.config/zed/logs/`
- Verify extension directory structure

## Resources

- [Tree-sitter Documentation](https://tree-sitter.github.io/)
- [VSCode Extension API](https://code.visualstudio.com/api)
- [Zed Extension Guide](https://zed.dev/docs/extensions)
- [TextMate Grammar Guide](https://macromates.com/manual/en/language_grammars)

## License

MIT
