# C Foreign Function Interface (FFI)

The Nari language supports calling C library functions directly through its Foreign Function Interface (FFI). This allows you to use existing C libraries without needing to write wrapper code.

## Loading Libraries

Use the `import` syntax to load a shared library:

```nari
import libc from "libc.so.6"
import raylib from "libraylib.so.550"
```

The FFI will search for libraries in different directories depending on the platform, check the following for your platform:

### Windows
1. Current directory
2. Directories in `PATH`
3. `C:\\Windows`
4. `C:\\Windows\\System32`
5. `C:\\Windows\\SysWOW64`

### Linux
1. Current directory
2. `/lib`
3. `/usr/lib`
4. `/usr/local/lib`
5. `/lib64`
6. `/usr/lib64`
7. `/usr/local/lib64`

### macOS
1. Current directory
2. `/usr/lib`
3. `/usr/local/lib`
4. `/opt/homebrew/lib`

## Library Object

When a library is loaded, you get an object with:
- `loaded`: Boolean indicating if the library loaded successfully
- `path`: The path that was used to load the library
- `__symbols__`: Array of all exported symbol names
- Individual properties for each exported symbol

```nari
import libc from "libc.so.6"

if (libc.loaded) {
    print("Loaded " @ libc.path @ "\n")
    print("Found " @ libc.__symbols__.length() @ " symbols\n")
}
```

## Calling C Functions

Use the `__ffi_call` builtin to call C functions:

```nari
__ffi_call(library, "function_name", signature, arguments)
```

### Parameters

1. **library**: The library object from `import`
2. **function_name**: String name of the C function
3. **signature**: Object describing function signature (see below)
4. **arguments**: Array of arguments to pass to the function

### UTF-16 Helpers for Win32 APIs

For Windows `W` APIs (such as `MessageBoxW`) that expect UTF-16 pointers, use:

- `__ffi_utf16(str)` -> returns a pointer (as integer) to a null-terminated UTF-16 buffer
- `__ffi_alloc(sizeBytes)` -> allocates a zeroed writable buffer for output parameters
- `__ffi_utf16_read(ptr, maxUnits?)` -> reads UTF-16 memory back into a Nari UTF-8 string
- `__ffi_free(ptr)` -> frees memory previously allocated by `__ffi_utf16`

Example:

```nari
import user32 from "user32.dll"

let textW = __ffi_utf16("Hello")
let titleW = __ffi_utf16("Nari")

let result = __ffi_call(user32, "MessageBoxW", {
    returns: "int",
    params: ["pointer", "pointer", "pointer", "u32"]
}, [0, textW, titleW, 0])

__ffi_free(textW)
__ffi_free(titleW)
```

For output APIs like `GetClassNameW(HWND, LPWSTR, int)`:

```nari
let maxChars = 256
let buf = __ffi_alloc(maxChars * 2) // WCHAR is 2 bytes
let n = __ffi_call(user32, "GetClassNameW", {
    returns: "int",
    params: ["pointer", "pointer", "int"]
}, [hwnd, buf, maxChars])

let className = n > 0 ? __ffi_utf16_read(buf, n) : ""
__ffi_free(buf)
```

### Function Signature

The signature object has two fields:

- `return`: Return type as a string
- `params`: Array of parameter types as strings

**Supported Types:**
- `"void"` - No return value
- `"i8"` / `"int8"` / `"char"` - 8-bit signed integer
- `"u8"` / `"uint8"` / `"uchar"` / `"byte"` - 8-bit unsigned integer
- `"i16"` / `"int16"` / `"short"` - 16-bit signed integer
- `"u16"` / `"uint16"` / `"ushort"` / `"word"` - 16-bit unsigned integer (useful for Win32 `WORD`)
- `"int"` - 32-bit integer
- `"long"` - 64-bit integer
- `"float"` - Single-precision floating point
- `"double"` - Double-precision floating point
- `"string"` or `"pointer"` - Pointer (for strings or other pointers)
- `"uint"` / `"u32"` - 32-bit unsigned integer
- `"ulong"` / `"u64"` - 64-bit unsigned integer
- **Pointer syntax with `*`** - Any type ending with `*` (e.g., `"MSG*"`, `"u8*"`) is treated as a pointer
- **Struct objects** - For passing C structs by value (see Struct Passing section)

## Examples

### Simple String Function

```nari
import libc from "libc.so.6"

let str_sig = {
    returns: "int",
    params: ["string"]
}

let len = __ffi_call(libc, "strlen", str_sig, ["Hello, World!"])
print("Length: " @ len @ "\n")  // Output: Length: 13
```

### Math Functions

```nari
let abs_sig = {
    returns: "int",
    params: ["int"]
}

let result = __ffi_call(libc, "abs", abs_sig, [-42])
print("abs(-42) = " @ result @ "\n")  // Output: abs(-42) = 42
```

### Functions with Multiple Parameters

```nari
let strcmp_sig = {
    returns: "int",
    params: ["string", "string"]
}

let cmp = __ffi_call(libc, "strcmp", strcmp_sig, ["hello", "world"])
if (cmp < 0) {
    print("hello comes before world\n")
}
```

### Void Return Type

```nari
let puts_sig = {
    returns: "void",
    params: ["string"]
}

__ffi_call(libc, "puts", puts_sig, ["Hello from Nari!"])
```

### Getting Current Time

```nari
let time_sig = {
    returns: "long",
    params: ["pointer"]
}

let timestamp = __ffi_call(libc, "time", time_sig, [0])
print("Unix timestamp: " @ timestamp @ "\n")
```

## Complete Example

```nari
import libc from "libc.so.6"

// Define signatures
let str_len_sig = { returns: "int", params: ["string"] }
let abs_sig = { returns: "int", params: ["int"] }
let puts_sig = { returns: "int", params: ["string"] }

// Call functions
let message = "FFI is awesome!"
let len = __ffi_call(libc, "strlen", str_len_sig, [message])

__ffi_call(libc, "puts", puts_sig, ["Message: " @ message])
print("Length: " @ len @ "\n")

let numbers = [-10, -5, 0, 5, 10]
for (num in numbers) {
    let abs_val = __ffi_call(libc, "abs", abs_sig, [num])
    print("abs(" @ num @ ") = " @ abs_val @ "\n")
}
```

## Header Bindgen

Nari ships with a small header-to-FFI generator written in Nari itself:

```bash
./build/debug/nari tools/ffi_bindgen.nari \
  --input /path/to/header.h \
  --output bindings.nari
```

For more complex headers, a Clang AST backend is also available:

```bash
./build/debug/nari tools/ffi_bindgen.nari \
  --backend clang-json \
  --cc clang \
  --input /path/to/header.h \
  --output bindings.nari
```

### Multiple libraries

By default the generator emits a single `import lib from "<name>"` and calls
`__ffi_call(lib, ...)` directly in each wrapper. To spread symbols across several
shared libraries, pass `--module-name <Name>` and `--libs lib1,lib2,...`:

```bash
./build/debug/nari tools/ffi_bindgen.nari \
  --input /path/to/header.h \
  --output bindings.nari \
  --module-name Graphics \
  --libs libfoo.so,libbar.so
```

The generated loader imports each library in order and lazily routes each call
to whichever library first exports the requested symbol, caching the result.
Library names should include the platform extension (e.g. `libfoo.so`,
`user32.dll`, `libbar.dylib`) so the runtime loads them as native libraries
rather than treating a bare name as a package import.

### Generating npkg packages with `--npkg`

The generator can emit an [npkg](19-package-management.md)-compatible package
directory instead of a single `.nari` file. Pass `--npkg <name>` and point
`--output` at a directory:

```bash
./build/debug/nari tools/ffi_bindgen.nari \
  --input /path/to/mathlib.h \
  --output ./packages/mathlib \
  --npkg "ffi/mathlib" \
  --npkg-version "0.1.0" \
  --libs libmathlib.so
```

The generated layout is a single-module package:

```
packages/mathlib/
+-- nari.toml             # packageFormat = 1, name, version, [exports]
+-- src/
    +-- mathlib.nari      # types, structs, macros, signatures, wrapper object
```

The module name is derived from the tail of `--npkg` (e.g. `ffi/mathlib` ->
`mathlib`). Every generated symbol - the wrapper object, macro constants, and
enum constants - is re-exported via a trailing `export { ... }` block, and the
manifest's `[exports]` table maps both the package root (`"."`) and each named
symbol to the module file. Consumers can then import at whatever granularity
they want:

```nari
// The whole wrapper object
import { Mathlib } from "ffi/mathlib"
Mathlib.add(2, 3)

// A single constant
import { MATH_PI } from "ffi/mathlib"
```

Add the package to your project's `nari.toml`:

```toml
[dependencies]
"ffi/mathlib" = { path = "/path/to/packages/mathlib" }
```

### What gets generated

The generator preprocesses the header with `cc -E -P` (or `clang-cl /EP` when
`--cc` names a `clang-cl` driver), then emits:

- `type` aliases for supported `typedef`s
- `type Name { ... }` declarations for supported `struct`s
- pooled `global SIG_* = { returns: ..., params: [...] }` signature objects shared across matching function shapes
- a `load_<Name>()` helper plus `<Name> = { ... }` wrapper object that calls through `__ffi_call(...)` (single lib) or a `<Name>_call(name, sig, args)` indirection that resolves the right library lazily (multi-lib mode)
- object-like `#define` constants for supported macros, including integer aliases and simple `CLITERAL(Type){ ... }` struct literals

The `legacy` backend is fast and works well for simpler C headers. The
`clang-json` backend is better for macro-heavy or declaration-heavy headers
because it reads Clang's JSON AST instead of relying on the token parser alone.

Optional flags:

```bash
./build/debug/nari tools/ffi_bindgen.nari \
  --input header.h \
  --output bindings.nari \
  --cc clang \
  --abi lp64 \
  --cppflags "-I/path/to/includes -DSOME_MACRO"
```

Current supported subset:

- plain `typedef` aliases
- `typedef enum { ... } Name;` with integer-valued enumerators
- `typedef struct { ... } Name;`
- `typedef struct Tag { ... } Name, *PName;`
- plain `struct Name { ... };`
- fixed-size array fields in structs are flattened into repeated fields to preserve layout
- function pointer typedefs/callback aliases are treated as opaque `pointer` values
- object-like `#define`s with literal/arithmetic/identifier bodies
- `#define NAME CLITERAL(Type){ ... }` using previously parsed struct field order
- ordinary function prototypes
- variadic prototypes like `int printf(const char *fmt, ...);`

Current intentional limitations:

- unions are skipped
- bitfields are skipped
- function-like macros are skipped
- complex declarators are skipped
- unsupported declarations are emitted as comments in the generated file rather than guessed

For ambiguous C integer types like `long`, use the correct ABI model:

- `lp64`: typical Unix-like 64-bit targets
- `llp64`: Windows 64-bit targets
- `ilp32`: 32-bit targets

## Architecture Support

The FFI currently supports:
- **x86_64**: Full support with System V ABI calling convention
- **ARM64**: Not yet implemented
- **Others**: Not yet implemented

## Variadic Functions

Variadic functions (like `printf`, `TextFormat`) are supported using the `variadic` field in the signature:

```nari
// TextFormat from raylib - signature: const char* TextFormat(const char *text, ...)
let format_sig = {
    returns: "pointer",
    params: ["pointer"],  // Only the fixed parameter (format string)
    variadic: 1           // Number of fixed params before variadic args
}

// Call with variadic arguments
let result = __ffi_call(raylib, "TextFormat", format_sig, [
    "Player: %s, Score: %d, Health: %.1f",
    "Alice",
    1000,
    95.5
])
```

### How Variadic Works

1. **`params` array**: List only the **fixed** parameter types (before `...`)
2. **`variadic` field**: Number of fixed parameters (usually same as `params.length`)
3. **Arguments**: Pass all arguments including variadic ones - types are inferred automatically

### Type Inference for Variadic Args

Variadic argument types are automatically inferred:
- Nari strings -> `pointer` (C string)
- Nari integers -> `int` (32-bit)
- Nari floats -> `double` (64-bit float)

### Complete Example

```nari
import raylib from "libraylib.so"

// Wrapper for TextFormat with up to 3 variadic args
func TextFormat(format, arg1, arg2, arg3) {
    let sig = {
        returns: "pointer",
        params: ["pointer"],
        variadic: 1
    }
    
    if (arg3 != null) {
        return __ffi_call(raylib, "TextFormat", sig, [format, arg1, arg2, arg3])
    } else if (arg2 != null) {
        return __ffi_call(raylib, "TextFormat", sig, [format, arg1, arg2])
    } else if (arg1 != null) {
        return __ffi_call(raylib, "TextFormat", sig, [format, arg1])
    } else {
        return __ffi_call(raylib, "TextFormat", sig, [format])
    }
}

// Use it like printf
let text1 = TextFormat("FPS: %d", 60, null, null)
let text2 = TextFormat("Position: (%.2f, %.2f)", 10.5, 20.3, null)
let text3 = TextFormat("Player: %s, Score: %d", "Alice", 100, null)
```

## Struct Passing

You can now pass C structs by value through FFI by defining the struct layout in the signature. This is essential for working with libraries like raylib, Win32 API, and other C libraries that use structs.

### Defining Struct Parameters

Instead of a simple type string, use an object with `struct` and `fields`:

```nari
let rect_sig = {
    returns: "void",
    params: [
        {
            struct: "Rectangle",
            fields: {
                x: "float",
                y: "float",
                width: "float",
                height: "float"
            }
        },
        "int"  // color parameter
    ]
}
```

### Field Types

Struct fields support these types:
- `"int"` - 32-bit integer
- `"long"` - 64-bit integer
- `"float"` - Single-precision float
- `"double"` - Double-precision float
- `"pointer"` - Pointer/string

### Using `__ffi_membersof()` to Avoid Repetition

Instead of manually specifying struct fields, use the `__ffi_membersof()` builtin to automatically extract field information from a Nari type declaration:

```nari
// Declare the type once
type Rectangle {
    x: f32;
    y: f32;
    width: f32;
    height: f32;
}

// Use __ffi_membersof() in FFI signature
let draw_rect_sig = {
    returns: "void",
    params: [
        __ffi_membersof("Rectangle"),  // Automatically extracts struct definition
        "int"
    ]
}
```

The `__ffi_membersof()` function:
- Takes a type name as a string argument
- Returns an object with `struct` and `fields` properties
- Preserves field order from the type declaration
- Automatically maps Nari types to FFI types (f32->"float", i32->"int", etc.)

**Type Mapping:**
- `f32`, `float` -> `"float"`
- `f64`, `double` -> `"double"`
- `i32`, `int` -> `"int"`
- `i64`, `long` -> `"long"`
- `string`, `pointer` -> `"pointer"`

### Passing Struct Arguments

Create a Nari object with matching field names:

```nari
let rectangle = {
    x: 50.0,
    y: 100.0,
    width: 200.0,
    height: 150.0
}

__ffi_call(raylib, "DrawRectangleRec", rect_sig, [rectangle, color])
```

### Complete Raylib Example

```nari
import raylib from "libraylib.so"

// Define the Rectangle type once
type Rectangle {
    x: f32;
    y: f32;
    width: f32;
    height: f32;
}

// Use __ffi_membersof() in the signature - no repetition!
let draw_rect_sig = {
    returns: "void",
    params: [
        __ffi_membersof("Rectangle"),
        "int"
    ]
}

// Create a rectangle
let rect = {
    x: 50.0,
    y: 20.0,
    width: 100.0,
    height: 50.0
}

// Initialize window
let init_sig = { returns: "void", params: ["int", "int", "string"] }
__ffi_call(raylib, "InitWindow", init_sig, [800, 600, "Struct FFI Test"])

// Main loop
let should_close_sig = { returns: "int", params: [] }
let begin_sig = { returns: "void", params: [] }
let end_sig = { returns: "void", params: [] }
let clear_sig = { returns: "void", params: ["int"] }

while (!__ffi_call(raylib, "WindowShouldClose", should_close_sig, [])) {
    __ffi_call(raylib, "BeginDrawing", begin_sig, [])
    __ffi_call(raylib, "ClearBackground", clear_sig, [0x181818FF])
    
    // Draw rectangle using struct
    __ffi_call(raylib, "DrawRectangleRec", draw_rect_sig, [rect, 0xFF0000FF])
    
    __ffi_call(raylib, "EndDrawing", end_sig, [])
}

let close_sig = { returns: "void", params: [] }
__ffi_call(raylib, "CloseWindow", close_sig, [])
```

### Struct Return Values

Structs can also be returned from C functions. The FFI will automatically unpack them into Nari objects.

The recommended approach is to use `__ffi_membersof()`:

```nari
type Vector2 {
    x: f32;
    y: f32;
}

let get_mouse_pos_sig = {
    returns: __ffi_membersof("Vector2"),
    params: []
}

let pos = __ffi_call(raylib, "GetMousePosition", get_mouse_pos_sig, [])
print("Mouse: " @ pos.x @ ", " @ pos.y @ "\n")
```

Alternatively, you can manually specify the fields as an array:

```nari
let get_mouse_pos_sig = {
    returns: {
        struct: "Vector2",
        fields: [
            { name: "x", type: "float" },
            { name: "y", type: "float" }
        ]
    },
    params: []
}

let pos = __ffi_call(raylib, "GetMousePosition", get_mouse_pos_sig, [])
print("Mouse: " @ pos.x @ ", " @ pos.y @ "\n")
```

### How It Works

1. **Marshal**: Nari objects are packed into C struct layout (correct field order and alignment)
2. **Call**: libffi passes the struct by value to the C function
3. **Unmarshal**: Return structs are unpacked back into Nari objects

### Important Notes

- **Field order matters**: Fields are packed in the order they appear in the `fields` object
- **Alignment**: The FFI handles proper struct alignment automatically
- **Nested structs**: Not yet supported - use separate calls or pointers
- **Arrays in structs**: Not yet supported

## Working with Struct Pointers

For advanced FFI usage, you can work with struct pointers directly. This is essential for calling C functions that expect pointers to structs (output parameters) or for manual memory management.

### Struct Pointer Functions

#### `__ffi_sizeof(typename)`
Returns the size in bytes of a struct type, including proper alignment.

```nari
type Point {
    x: f32;
    y: f32;
}

let size = __ffi_sizeof("Point");  // Returns 8 (2 floats x 4 bytes)
```

#### `__ffi_alloc_struct(typename)`
Allocates zero-initialized memory for a struct and returns a pointer to it as an integer.

```nari
let ptr = __ffi_alloc_struct("Point");  // Returns pointer as int64
```

#### `__ffi_read_struct(ptr, typename)`
Reads a struct from memory and returns it as a Nari object.

```nari
let point = __ffi_read_struct(ptr, "Point");
print("x: " @ to_string(point.x) @ ", y: " @ to_string(point.y));
```

#### `__ffi_write_struct(ptr, typename, object)`
Writes a Nari object to struct memory.

```nari
let point_obj = { x: 10.5, y: 20.7 };
__ffi_write_struct(ptr, "Point", point_obj);
```

### Pointer Type Syntax

The FFI type parser supports a convenient pointer syntax using `*`:

```nari
// Old way (still supported)
let sig = { returns: "int", params: ["pointer", "pointer"] };

// New way - more descriptive
let sig = { returns: "int", params: ["MSG*", "HWND*"] };
```

Any type ending with `*` is treated as a pointer type (`FFIType::Pointer`). This makes signatures more readable and self-documenting.

### Complete Example: Win32 Message Loop

With struct pointer support, you can implement complex C APIs entirely in Nari:

```nari
// Define the MSG struct
type MSG {
    hwnd: pointer;
    message: u32;
    wParam: u64;
    lParam: i64;
    time: u32;
    pt_x: i32;
    pt_y: i32;
}

// Define signatures using pointer syntax
let SIG_GETMESSAGE_W = { returns: "int", params: ["MSG*", "pointer", "u32", "u32"] };
let SIG_TRANSLATEMESSAGE = { returns: "bool", params: ["MSG*"] };
let SIG_DISPATCHMESSAGE_W = { returns: "long", params: ["MSG*"] };

// Implement message loop in pure Nari
func MessageLoop() {
    let msgBuf = __ffi_alloc_struct("MSG");
    
    while (true) {
        let ret = __ffi_call(user32_lib, "GetMessageW", SIG_GETMESSAGE_W, [msgBuf, 0, 0, 0]);
        
        if (ret == -1) {
            __ffi_free(msgBuf);
            return -1;
        }
        
        if (ret == 0) {
            // WM_QUIT received, read wParam from MSG
            let msg = __ffi_read_struct(msgBuf, "MSG");
            __ffi_free(msgBuf);
            return msg.wParam;
        }
        
        __ffi_call(user32_lib, "TranslateMessage", SIG_TRANSLATEMESSAGE, [msgBuf]);
        __ffi_call(user32_lib, "DispatchMessageW", SIG_DISPATCHMESSAGE_W, [msgBuf]);
    }
}
```

### Memory Management

Always free allocated struct memory when done:

```nari
let ptr = __ffi_alloc_struct("MyStruct");
// ... use the struct ...
__ffi_free(ptr);  // Clean up
```

### Struct Pointer Implementation Details

- Uses libffi to compute proper struct sizes and alignment
- Supports all FFI types: i8, u8, i16, u16, i32, i64, u32, u64, float, double, bool, pointer
- Handles proper memory alignment automatically
- Marshals between Nari objects and C struct memory layout
- Works with any registered Nari type

## Callbacks

Nari supports creating native C function pointers from Nari functions, enabling you to pass callbacks to C libraries that expect them. This is essential for Windows API (WndProc), GUI frameworks, event handlers, and many other C APIs.

### Creating Callbacks

Use `__ffi_create_callback` to create a native function pointer:

```nari
func MyCallback(arg1, arg2, arg3) {
    print("Called from C with: " @ arg1 @ ", " @ arg2 @ ", " @ arg3 @ "\n");
    return 42;
}

let callback_sig = {
    returns: "int",
    params: ["int", "pointer", "float"]
}

let callback_ptr = __ffi_create_callback(callback_sig, MyCallback);
// callback_ptr is now a native function pointer (as an integer)
```

### Callback Signature

The signature object is the same format as `__ffi_call`:
- `returns`: Return type as a string
- `params`: Array of parameter types

**Supported Types:**
- All the same types as `__ffi_call`: `void`, `int`, `long`, `float`, `double`, `bool`, `pointer`, etc.
- Pointer syntax: `"MSG*"`, `"HWND*"`, etc.

### Freeing Callbacks

Always free callbacks when done to avoid memory leaks:

```nari
__ffi_free_callback(callback_ptr);
```

### Win32 Window Procedure Example

Here's a complete example using callbacks for Win32 GUI programming:

```nari
import user32 from "user32.dll";
import kernel32 from "kernel32.dll";

// Define window procedure
func MyWndProc(hwnd, msg, wParam, lParam) {
    if (msg == 0x0002) {  // WM_DESTROY
        __ffi_call(user32, "PostQuitMessage", {
            returns: "void",
            params: ["int"]
        }, [0]);
        return 0;
    }
    
    // Default window processing
    return __ffi_call(user32, "DefWindowProcW", {
        returns: "long",
        params: ["pointer", "u32", "u64", "i64"]
    }, [hwnd, msg, wParam, lParam]);
}

// Define WNDCLASSEXW structure
type WNDCLASSEXW {
    cbSize: u32;
    style: u32;
    lpfnWndProc: pointer;
    cbClsExtra: i32;
    cbWndExtra: i32;
    hInstance: pointer;
    hIcon: pointer;
    hCursor: pointer;
    hbrBackground: pointer;
    lpszMenuName: pointer;
    lpszClassName: pointer;
    hIconSm: pointer;
}

// Create callback for window procedure
let wndproc = __ffi_create_callback({
    returns: "long",
    params: ["pointer", "u32", "u64", "i64"]
}, MyWndProc);

// Get module handle
let hInstance = __ffi_call(kernel32, "GetModuleHandleW", {
    returns: "pointer",
    params: ["pointer"]
}, [0]);

// Create class name
let className = __ffi_utf16("NariWindow");

// Register window class
let wc = {
    cbSize: __ffi_sizeof("WNDCLASSEXW"),
    style: 0x0003,  // CS_HREDRAW | CS_VREDRAW
    lpfnWndProc: wndproc,  // Our callback!
    cbClsExtra: 0,
    cbWndExtra: 0,
    hInstance: hInstance,
    hIcon: 0,
    hCursor: 0,
    hbrBackground: 0,
    lpszMenuName: 0,
    lpszClassName: className,
    hIconSm: 0
};

let atom = __ffi_call(user32, "RegisterClassExW", {
    returns: "u16",
    params: [__ffi_membersof("WNDCLASSEXW")]
}, [wc]);

if (atom != 0) {
    print("Window class registered successfully!\n");
    
    // Create the window
    let windowName = __ffi_utf16("My Nari Window");
    let hwnd = __ffi_call(user32, "CreateWindowExW", {
        returns: "pointer",
        params: ["u32", "pointer", "pointer", "u32", 
                 "i32", "i32", "i32", "i32",
                 "pointer", "pointer", "pointer", "pointer"]
    }, [0, className, windowName, 0x00CF0000,  // WS_OVERLAPPEDWINDOW
        100, 100, 640, 480,
        0, 0, hInstance, 0]);
    
    if (hwnd != 0) {
        // Show window
        __ffi_call(user32, "ShowWindow", {
            returns: "bool",
            params: ["pointer", "int"]
        }, [hwnd, 5]);  // SW_SHOW
        
        // Message loop would go here...
    }
    
    __ffi_free(windowName);
}

__ffi_free(className);

// Clean up callback when done
__ffi_free_callback(wndproc);
```

### How Callbacks Work

1. **Create**: `__ffi_create_callback` allocates executable memory and sets up a trampoline
2. **Trampoline**: When native code calls the function pointer, it enters a bridge function
3. **Marshal**: C arguments are converted to Nari Values
4. **Execute**: The Nari function is called with the converted arguments
5. **Return**: The Nari return value is converted back to C and returned

### Callback Memory Management

- Callbacks remain valid until `__ffi_free_callback` is called
- The Nari function is kept alive (GC-safe) while the callback exists
- Always free callbacks to avoid memory leaks
- Be careful not to free callbacks that native code is still using

### Callback Limitations

1. **Variadic callbacks**: Not supported (callbacks with `...` args)
2. **Struct callbacks**: Passing/returning structs by value in callbacks is not yet tested
3. **Nested callbacks**: Callbacks calling back into Nari which then calls another callback should work but may have edge cases
4. **Performance**: Callbacks have overhead from marshalling between C and Nari

## Limitations

1. **Nested Structs**: Structs within structs are not yet supported
2. **Struct Arrays**: Arrays of structs or structs containing arrays are not yet supported
3. **Variadic Callbacks**: Cannot create callbacks with variadic parameters
4. **Variadic Structs**: Passing structs to variadic functions is not well-supported
5. **Struct Pointer Limitations**: When using `__ffi_read_struct` and `__ffi_write_struct`:
   - Struct fields must match the type declaration exactly
   - Missing fields in write operations will trigger warnings but continue
   - No support for nested structs or arrays within structs

## Safety Considerations

> [!WARNING]
> FFI calls bypass Nari's (somewhat minimal) safety checks. Incorrect usage can easily cause crashes!

- **Verify signatures match the C function exactly**
- **Ensure string arguments are valid**
- **Be careful with pointer arguments** Invalid pointers almost always lead to crashes or UB.
- **Check return values** for error conditions

## Error Handling

If a library fails to load, the returned object will have:
```nari
{
    loaded: false,
    error: "Error message",
    message: "Detailed error from dlopen/LoadLibrary"
}
```

If a symbol is not found, `__ffi_call` returns `<null>`.

## Platform-Specific Notes

### Linux
- Use `ldd` to find library dependencies
- Use `nm -D libname.so` to list exported symbols

### Finding Symbol Names

To see what functions are available in a library:
```bash
nm -D /usr/lib/libc.so.6 | grep " T "
```

Or within Nari itself:
```nari
import libc from "libc.so.6"
for (symbol in libc.__symbols__) {
    print(symbol @ "\n")
}
```

The above will work for both Linux and Windows, assuming it loads successfully.
