# Nari

Nari is a dynamically typed scripting language designed for rapid development with support for asynchronous programming, first-class functions, and seamless C interoperability through its foreign function interface. The language combines modern features with a straightforward syntax, making it suitable for both quick scripts and more complex applications.

## Features

* **Dynamic Typing with Optional Annotations**: Write code quickly without type declarations, or add optional type hints for better documentation and clarity.
* **Asynchronous Programming**: Built-in support for concurrent operations using spawn blocks and cooperative multitasking with an integrated event loop.
* **First-Class Functions and Closures**: Functions are values that can be passed around, returned, and capture their surrounding scope.
* **C Foreign Function Interface**: Call C library functions directly without writing wrapper code, enabling use of existing native libraries.
* **Class System**: Object-oriented programming with classes, inheritance, private members, and constructors.
* **Generics and Enums**: Type-safe generics and algebraic data types with pattern matching support.
* **Rich Standard Library**: Comprehensive built-in modules for common tasks including HTTP operations, file I/O, mathematical operations, and system interactions.
* **String Interpolation**: Expressive string formatting using backtick strings with embedded expressions.
* **Error Handling**: Value-based error handling with `Result`/`Option` enums; `panic(value)` for unrecoverable failures.
* **Module System**: Organize code across multiple files with the import system.
* **Cross-Platform**: Supports Linux, Windows, and WebAssembly via Emscripten.

## Quick Start

### Hello World

```nari
print("Hello, World!");
```

### Asynchronous HTTP Request

```nari
let handle = spawn {
  let response = await http.fetch("https://example.com/data");
  print(response)
  return response.body;
};

print("Response:", handle.value);
```

### Working with Classes

```nari
class Person {
  public name: string;
  public age: number;
  init(name, age) {
      this.name = name;
      this.age = age;
  }
  
  greet() {
      print(`Hello, my name is {this.name}! I am {this.age} years old.`);
  }
}

let person = new Person("Alice", 30);
person.greet();
```

### C Foreign Function Interface

```nari
import libc from "libc.so.6"

let str_sig = {
  returns: "int",
  params: ["string"]
}

let len = __ffi_call(libc, "strlen", str_sig, ["Hello, World!"])
print("Length: " @ len @ "\n")  // Output: Length: 13
```

## Installation

### Linux and macOS

Install the latest release with the bootstrap script (never uses sudo; installs
into `~/.nari` and adds it to your `PATH`):

```bash
curl -fsSL https://raw.githubusercontent.com/nari-lang/nari/main/install.sh | sh
```

Pass `--with-npkg` to also install the [npkg](https://github.com/nari-lang/npkg)
package manager. To uninstall, run `uninstall.sh` from the same location.

### Windows (manual)

There's no Windows installer yet, so install from the release archive:

1. Download `nari-<version>-windows-x86_64.zip` (or `-arm64.zip` on ARM) from
   the [Releases page](https://github.com/nari-lang/nari/releases).
2. Extract it somewhere stable, e.g. `C:\Program Files\Nari`.
3. Add that folder's `bin\` directory to your `PATH` so `nari` and `naric` are
   available from any terminal.

> A proper Windows installer (NSIS or similar) is planned to replace this manual
> step.

## Building from Source

### Prerequisites

* Meson build system and Ninja
* Conan for third-party dependency installation
* C++20 compatible compiler (Clang or GCC recommended)
* Python 3 (for the stdlib/builtins embedding script; this is planned to be removed as a normal build dependency)
* Platform SDK/toolchain for your target OS

### Linux Build

```bash
./build_deps.sh
./build.sh
```

By default this creates a debug build in `build/debug/`:

* `build/debug/nari`
* `build/debug/naric`
* `build/debug/nari-lsp` when LSP support is enabled

For a release build:

```bash
./build.sh --release
```

Release artifacts are placed in `build/release/`.

### System Dependencies (Distro Packaging)

To build against distro packages instead of Conan:

```bash
./build.sh --system-deps --release
```

Dependencies are resolved via pkg-config / the default linker paths, and the
build uses the system toolchain and standard library.

Preferred (system) packages:
- `libcurl`
- `mbedtls`
- `libffi`
- `libarchive`
- `zlib`
- `asmjit`
- `replxx` (plus their headers).

Any dependency not found on the system is fetched and built automatically from
a Meson [wrap](https://mesonbuild.com/Wrap-dependency-system-manual.html) in
`subprojects/` (WrapDB for curl/zlib/libffi/libarchive; pinned Git + CMake for
asmjit/replxx/mbedtls). A normal `--system-deps` build therefore succeeds even
on distros that do not package `asmjit` or `replxx`.

`asmjit` and `replxx` are also optional at the feature level: configure with
`-Ddisable_jit=true` / `-Ddisable_repl=true` to drop them entirely.

Packagers who need reproducible, network-isolated builds can control the
fallback with Meson's `--wrap-mode`:
- `--wrap-mode=nofallback` — never build subprojects; require system copies
  (missing `asmjit` errors, missing `replxx` just disables the REPL).
- `--wrap-mode=nodownload` — only use sources already vendored under
  `subprojects/` / `subprojects/packagecache/` (no network fetch).

Packaging scripts can also call Meson directly:

```bash
meson setup build --buildtype=release -Dsystem_deps=true
meson compile -C build
```

Artifacts land in `build/sysdeps-release/` when using `build.sh`.

### Minimal Build (Embedded Systems)

For reduced binary size and memory usage, you can disable FFI and HTTP networking:

```bash
./build_minimal.sh
```

### Windows Build

Use the PowerShell build script from a normal PowerShell prompt:

```powershell
.\build.ps1  # debug
.\build.ps1 -Release  # release
.\build.ps1 -ClangCl  # clang-cl instead of MSVC
```

See [BUILD_WINDOWS.md](BUILD_WINDOWS.md) for detailed native Windows and Wine-based cross-build instructions.

### WebAssembly Build

```bash
./build.sh --emscripten
```

This creates a WebAssembly build suitable for running in web browsers.

## Running Programs

Execute a Nari script file with whichever build you created:

```bash
./build/debug/nari script.nari
./build/release/nari script.nari
```

Compile a script to bytecode:

```bash
./build/release/naric script.nari -o script.naric
./build/release/nari script.naric
```

Run the test suite:

```bash
./run_tests.sh  # release build, default bytecode VM path
./run_tests.sh --debug  # debug build
./run_tests.sh --tree-walk
```

## Documentation

Comprehensive documentation is available in the [docs/](docs/) directory:

* [Language Overview](docs/01-overview.md): Introduction to Nari and its design philosophy
* [Syntax Basics](docs/02-syntax-basics.md): Variables, comments, and fundamental syntax
* [Data Types](docs/03-data-types.md): Primitives, arrays, objects, and type checking
* [Operators](docs/04-operators.md): Arithmetic, logical, comparison, and special operators
* [Control Flow](docs/05-control-flow.md): Conditionals, loops, and flow control
* [Functions](docs/06-functions.md): Function declarations, closures, and advanced patterns
* [Custom Types](docs/07-custom-types.md): Type declarations and annotations
* [Asynchronous Programming](docs/08-async.md): Spawn blocks and event loop usage
* [Modules](docs/09-modules.md): Import system and code organization
* [Error Handling](docs/10-error-handling.md): Value-based error handling with Result/Option
* [Built-in Functions](docs/11-builtins.md): Core globally available functions
* [Standard Library](docs/12-stdlib.md): System, math, I/O, HTTP, and networking modules
* [String Methods](docs/13-string-methods.md): String manipulation operations
* [Array Methods](docs/14-array-methods.md): Array utilities and transformations
* [Object Methods](docs/15-object-methods.md): Working with objects and maps
* [Generics and Enums](docs/16-generics-enums.md): Generic types and algebraic data types
* [C FFI](docs/17-ffi.md): Foreign function interface for calling C libraries
* [Classes](docs/18-classes.md): Object-oriented programming with classes
* [Bytecode](docs/19-bytecode.md): `.naric` files, verifier guarantees, and compiled module imports

## Examples

The [examples/](examples/) directory contains sample programs demonstrating various language features:

* `class_character.nari`: Character class implementation
* `class_library.nari`: Library management system using classes
* `generics_enums_example.nari`: Generic types and enum usage
* `http_client.nari`: HTTP client example
* `http_server.nari`: Simple HTTP server implementation
* `raylib_bouncing_ball.nari`: Graphics demo using Raylib via FFI
* `winapi_showcase.nari`: Windows API integration examples

## Project Structure

* `src/`: Core interpreter, runtime, bytecode VM/compiler, debugger, DAP server, JIT, GC, and builtins
* `src/stdlib/`: Standard library implemented in Nari and embedded into normal builds
* `docs/`: Language documentation
* `examples/`: Example programs
* `tests/`: Passing, expected-failing, dependency, and robustness tests
* `lsp/`: C++ language server plus the VS Code extension under `lsp/extension/`
* `npkg-frontend/`: Package-manager CLI, registry server, and web frontend experiments
* `esp_idf_project/`: ESP-IDF / embedded experiments
* `tools/`: Build and code-generation utilities
* `toolchain/`: Meson cross/native toolchain files
* `thirdparty/`: Vendored or manually managed third-party sources; most normal dependencies are installed through Conan
* `workspace/`: Local experiments and scratch files; this directory is gitignored

## Implementation Details

Nari is implemented in C++20 and includes:

* Custom parser and abstract syntax tree representation
* Tree-walking interpreter and bytecode VM with runtime type checking
* Garbage collector for automatic memory management
* Class system with inheritance and encapsulation (see [Classes](docs/18-classes.md))
* Integration with libffi for dynamic C function calls
* Embedded standard library compiled into the interpreter binary

## Editor Support

See [EDITOR_SUPPORT.md](EDITOR_SUPPORT.md) for information on the VS Code extension, `nari-lsp`, debugging, syntax highlighting, and editor integration.

## License

This project is licensed under the GNU General Public License v3.0. See [LICENSE](LICENSE) for details.

However, despite being licensed under the GPL, you are more than welcome to contact me if you're interested in using Nari for a proprietary project. I'm open to discussing alternative licensing arrangements on a case-by-case basis, and I would love to see Nari used in a wide variety of projects, both open source and commercial!
