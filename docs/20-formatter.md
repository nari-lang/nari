# Code Formatter (`nari fmt`)

`nari fmt` is the official code formatter for Nari. It rewrites source files with
canonical spacing and indentation while preserving your line structure and comments.

## Usage

```bash
nari fmt file.nari            # print formatted source to stdout
nari fmt -w file.nari         # format in place
nari fmt --check src/*.nari   # CI mode: exit 1 if any file needs formatting
nari fmt --stdin < file.nari  # read stdin, write stdout (editor integration)
```

## Style Rules

- **Indentation**: 4 spaces per block level, plus one level per open `(`/`[`
  on continuation lines.
- **Blank lines**: runs of blank lines collapse to at most one; no leading blank
  lines; the file ends with exactly one newline.
- **Operators**: spaces around binary operators and assignments
  (`a + b`, `x ?? y`, `s @ t`, `->`, `=>`); no spaces around unary operators
  (`-x`, `!flag`), member access (`a.b`, `a?.b`, `m::n`), or postfix `++`/`--`.
- **Calls and indexing**: `foo(a, b)`, `arr[0]` — no space before `(`/`[`,
  one space after each comma. Control keywords get a space: `if (x)`, `return (x)`.
- **Braces**: blocks keep the author's line placement; single-line blocks print as
  `{ stmt; }`, empty ones as `{}`.
- **Colons**: no space before, one space after (`let x: number`, `{ key: 1 }`,
  `case 1:`); ternary colons get spaces on both sides (`a ? b : c`).
- **Switch bodies**: statements under a `case`/`default` label are indented one
  extra level.
- **Generics**: `Box<T>` stays tight when written tight in the source
  (see limitations).
- **Comments**: preserved verbatim at their original position (line or trailing).
  `//`, `#`, and `/* */` forms are all supported.
- **Strings**: escape sequences are normalized (e.g. literal newlines inside
  string literals become `\n`). Interpolated strings keep expressions verbatim:
  `` `hi \{raw} {name:.3f}` ``.

## What It Does Not Do

`nari fmt` works on the token stream rather than the AST (the parser desugars
imports/enums and runs optimization passes, so the AST loses source fidelity).
That keeps formatting total and safe, but means a few things are out of scope:

- **No line re-wrapping**: long lines are not split, and hand-split lines are
  not joined. Your line breaks are kept as-is (blank runs are only clamped).
- **No brace style normalization**: `{` stays on the line you put it on.
- **No import sorting or dead-code removal**: output is token-for-token faithful
  to the input; only whitespace changes.
- **Generic `<` heuristic**: `Name<T>` written without a space is treated as
  generic arguments. A less-than written the same way (`a<b`) is preserved as
  `a<b` — still valid, just not re-spaced.
