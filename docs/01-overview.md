# Language Overview

## What is Nari?

Nari is a dynamically-typed scripting language designed for rapid development with support for:
- Asynchronous programming
- First-class functions and closures
- Custom type declarations
- HTTP and network operations
- File I/O
- Event-driven programming

## Design Philosophy

1. **Simplicity** - Easy to learn syntax inspired by JavaScript and Python
2. **Flexibility** - Dynamic typing with optional type annotations
3. **Async-First** - Built-in support for concurrent operations
4. **Batteries Included** - Rich standard library for common tasks

## Key Features

### Dynamic Typing with Type Hints
```nari
let x = 42;                    // No type declaration needed
let name: string = "Alice";    // Optional type annotation
```

### First-Class Functions
```nari
let add = func(a, b) { return a + b; };
let result = add(5, 3);  // 8
```

### Closures
```nari
func makeCounter() {
    let count = 0;
    return func() {
        count = count + 1;
        return count;
    };
}
```

### Asynchronous Programming
```nari
let handle = spawn {
    let data = http.get("https://api.example.com/data");
    return data;
};

let result = handle.value;
```

### String Interpolation
```nari
let name = "World";
let age = 25;
print(`Hello, {name}! You are {age} years old.`);
```

### Custom Types
```nari
type User {
    id: number;
    username: string;
    email: string
}

func createUser(id: number, username: string) -> User {
    return { id: id, username: username, email: "" };
}
```

## Program Structure

Nari programs can use top-level code, a `start()` function as an entry point, or both. Top-level code runs first, then `start()` is called if it exists:

```nari
// Simple program with top-level code
print("Program begins here!");
```

```nari
// Using start() as entry point
func start() {
    print("Program begins here!");
}
```

Additional functions can be defined at the top level:

```nari
func helper(x) {
    return x * 2;
}

func start() {
    let result = helper(21);  // 42
    print(result);
}
```

## Comments

```nari
// Single-line comment

/* Multi-line
   comment */
```

## Execution Model

1. **Parsing** - Source code is parsed into an AST
2. **Top-level execution** - Global statements, type declarations, and top-level code are processed
3. **Entry point** - The `start()` function is called if it exists
4. **Event loop** - If async operations or timers exist, the event loop runs
5. **Completion** - Program exits when event loop has no pending work

## File Extension

Nari source files use the `.nari` extension.

## Running Programs

```bash
./interpreter program.nari
```

## Next Steps

- [Syntax Basics](02-syntax-basics.md) - Learn the fundamental syntax
- [Data Types](03-data-types.md) - Understand Nari's type system
- [Functions](06-functions.md) - Master function usage
