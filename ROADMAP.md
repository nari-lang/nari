# Nari Roadmap

just a rough list of stuff that needs doing, in no particular order really

## language stuff

- optional chaining (`foo?.bar?.baz`) because null checks everywhere suck
- static class members
- property getters/setters on classes and objects
- the type annotations are parsed but never actually checked at runtime. at some point we should decide if we want real type checking or just keep them as docs. probably worth at least doing basic checks eventually
- enum struct variants with named fields are documented but not fully working yet
- exhaustiveness checking for match arms so you know if you missed a case
- generic constraints (like `<T: Comparable>` or whatever)

## parser / compiler

- the parser just dies on errors (`error_and_exit` calls `std::exit(1)`). need actual error recovery so we can report multiple errors at once instead of bailing on the first one. theres a TODO about this already
- string interpolation re parses expressions from source text every single time they're evaluated which is... not great. should parse them once and store the AST
- constant folding is decent but could be much better

## builtins and stdlib

### array methods we're missing (the big ones)
- `sort` (this one is really needed)
- `map`
- `filter`
- `reduce`
- `find` / `findIndex`
- `reverse`
- `includes`
- `every` / `some`
- `forEach`
- `splice`
- `indexOf` (the array version)

### string methods we're missing
- `padStart` / `padEnd`
- `repeat`
- `includes` (string version)
- regex support in general (no regex type at all right now)

### object methods
- `entries`
- `assign`
- `freeze` would be cool

## FFI

- ARM64 support. right now its x86_64 only which is fine for now but ARM is everywhere these days
- nested structs (structs inside structs)
- arrays inside structs
- struct callbacks (passing/returning structs by value in callbacks is untested)
- the FFI symbols are stored as placeholder strings like `"FFI::symbolname"`, we should do something with this or remove it.
- FFI error handling should use the Result type instead of whatever its doing now

## runtime / internals

- the whole area around closure environment sync needs to be way less fragile
- division by zero silently returns 0.0 instead of throwing or returning NaN or literally anything else. same with modulo
- global mutable state in the parser makes it non reentrant, not a huge deal right now but would matter if we ever wanted parallel compilation or a language server

## GC

- maybe dont expose GC internals to scripts? theres a comment about this being potentially bad
- the gc uses weak_ptr tracking instead of root set tracing which works but might want to revisit if performance becomes an issue

## build / tooling

- remove the Python dependency for stdlib embedding (theres a TODO for this, the embed script could just be done in C++ or as part of the meson build)
- the emscripten build works but its pretty limited. no threading, no HTTP, IO is all synchronous stubs
- SSL cert verification is just disabled on Windows which is noted as needing to be configurable

## nice to haves (someday maybe)

- REPL. right now you can only run files
- a debugger or at least better stepping support beyond `--trace-level` and `--dump-ast`
- spread operator in function calls and array literals (we have rest params but not spread)
- iterators / generators
- a package manager or at least some kind of dependency resolution beyond `import "file.nari"`
- visitor pattern for the AST instead of dynamic_cast chains everywhere (would make adding new node types way less painful)
- Bytecode maybe? Walking tree still performs pretty well, but bytecode would definitely be faster, and it would be nice for more "commercial" use cases to have a way of *somewhat* protecting their code.