# Nari Language Documentation

Welcome to the Nari language documentation! Nari is a lightweight, dynamically-typed scripting language with features for asynchronous programming, closures, and custom type annotations.

## Documentation Structure

### Getting Started
- [**Language Overview**](01-overview.md) - Introduction to Nari and its features
- [**Syntax Basics**](02-syntax-basics.md) - Variables, comments, and basic syntax

### Core Language Features
- [**Data Types**](03-data-types.md) - Primitives, arrays, objects, and type checking
- [**Operators**](04-operators.md) - Arithmetic, logical, comparison, and special operators
- [**Control Flow**](05-control-flow.md) - if/else, loops, switch, and flow control
- [**Functions**](06-functions.md) - Function declarations, expressions, closures, and advanced patterns
- [**Custom Types**](07-custom-types.md) - Type declarations and annotations

### Advanced Features
- [**Asynchronous Programming**](08-async.md) - spawn blocks, Spawn methods, and event loop
- [**Modules**](09-modules.md) - Import system and code organization
- [**Error Handling**](10-error-handling.md) - Result/Option enums and unrecoverable panics
- [**Generics and Enums**](16-generics-enums.md) - Generic types, enum variants, and pattern matching
- [**C Foreign Function Interface**](17-ffi.md) - Call C library functions directly from Nari

### Standard Library
- [**Built-in Functions**](11-builtins.md) - Core functions available globally
- [**Standard Library**](12-stdlib.md) - system, math, io, http, and net modules
- [**Stdlib API Reference**](stdlib-reference.md) - Auto-generated reference for every prelude global and `std/*` module (regenerate with `tools/gen_stdlib_reference.py docs/stdlib-reference.md src/stdlib/std`)

### Reference
- [**String Methods**](13-string-methods.md) - String manipulation functions
- [**Array Methods**](14-array-methods.md) - Array operations and utilities
- [**Object Methods**](15-object-methods.md) - Working with objects/maps

## Quick Examples

### Hello World
```nari
print("Hello, World!");
```

### Custom Types
```nari
type Person {
    name: string;
    age: number
}

func greet(person: Person) {
    print("Hello, " @ person.name @ "!");
}
```

### Async HTTP Request
```nari
let handle = spawn {
    let response = await http.fetch("https://example.com");
    return response.status_code;
};

print("Status:", handle.value);
```

## Language Features at a Glance

- **Dynamic Typing** - No compile-time type checking
- **Type Annotations** - Optional type hints for documentation
- **First-Class Functions** - Functions as values, closures
- **Lexical Scoping** - Block-scoped variables
- **Asynchronous** - spawn blocks with cooperative multitasking
- **String Interpolation** - Backtick strings with `{expressions}`
- **Operators** - Ternary `?:`, nullish coalescing `??`, exponentiation `**`
- **Module System** - Import external files
- **Error Handling** - Result/Option enums and unrecoverable panics
- **Event Loop** - set_timeout, set_interval for scheduling

## Contributing

See the main repository for information on contributing to Nari.
