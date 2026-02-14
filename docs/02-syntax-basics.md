# Syntax Basics

## Variables

### Declaration and Assignment

Variables are declared with `let` for local scope or `global` for global scope:

```nari
let x = 10;           // Local variable
let name = "Alice";   // Local string
global config = {};   // Global variable
```

Variables without initializers are `null`:

```nari
let uninitialized;    // null
```

### Type Annotations (Optional)

Variables can have optional type annotations:

```nari
let age: number = 25;
let name: string = "Bob";
let items: number[] = [1, 2, 3];
let person: Person = { name: "Alice", age: 30 };
```

Type annotations are for documentation and future validation - they don't enforce types at runtime in the current implementation.

### Assignment

```nari
let x = 5;
x = 10;              // Reassignment

let obj = { a: 1 };
obj.a = 2;           // Property assignment
obj["b"] = 3;        // Bracket notation

let arr = [1, 2, 3];
arr[0] = 10;         // Array element assignment
```

### Compound Assignment

```nari
let x = 10;
x += 5;    // x = 15
x -= 3;    // x = 12
x *= 2;    // x = 24
x /= 4;    // x = 6
x %= 4;    // x = 2
```

### Increment and Decrement

```nari
let i = 0;

i++;       // Post-increment (returns old value)
++i;       // Pre-increment (returns new value)
i--;       // Post-decrement
--i;       // Pre-decrement
```

## Scope

### Local Scope (let)

Variables declared with `let` are local to their block:

```nari
func example() {
    let x = 10;        // Function scope
    
    if (true) {
        let y = 20;    // Block scope
        print(x);      // 10 - can access outer scope
    }
    
    // print(y);       // Error: y not accessible here
}
```

### Global Scope (global)

Variables declared with `global` are accessible everywhere:

```nari
global appName = "MyApp";

func start() {
    print(appName);    // Accessible
}

func other() {
    print(appName);    // Also accessible
}
```

### Lexical Scoping

Nari uses lexical (static) scoping - variables are resolved based on where they're defined in the source code:

```nari
let outer = "outer";

func makeFunc() {
    let inner = "inner";
    
    return func() {
        print(outer);   // Accesses outer scope
        print(inner);   // Accesses enclosing function scope
    };
}

let f = makeFunc();
f();  // Prints: outer, inner
```

## Comments

### Single-line Comments

```nari
// This is a single-line comment
let x = 42;  // Comment after code
```

### Multi-line Comments

```nari
/*
 * This is a multi-line comment
 * It can span multiple lines
 */
let y = 100;
```

## Semicolons

Semicolons are **optional** in Nari:

```nari
let x = 10;     // With semicolon
let y = 20      // Without semicolon (both valid)
```

However, they're required to separate multiple statements on the same line:

```nari
let a = 1; let b = 2; let c = 3;
```

## String Literals

### Regular Strings

Use single or double quotes:

```nari
let single = 'Hello';
let double = "World";
```

### String Interpolation

Use backticks for template strings with interpolation:

```nari
let name = "Alice";
let age = 30;
let msg = `My name is {name} and I am {age} years old.`;

// Expressions in interpolation
let x = 5, y = 10;
print(`Sum: {x + y}`);              // Sum: 15
print(`Result: {x > 3 ? "big" : "small"}`);
```

### String Concatenation

Use the `@` operator to concatenate strings:

```nari
let first = "Hello";
let second = "World";
let result = first @ " " @ second;  // "Hello World"
```

### Escape Sequences

```nari
let newline = "Line 1\nLine 2";
let tab = "Col1\tCol2";
let quote = "She said \"Hello\"";
let backslash = "Path: C:\\Users";
```

## Identifiers

Identifier rules:
- Start with a letter or underscore
- Contain letters, numbers, and underscores
- Case-sensitive

```nari
let myVariable = 1;      // Valid
let _private = 2;        // Valid
let count123 = 3;        // Valid
let MyClass = 4;         // Valid (different from myClass)
// let 123abc = 5;       // Invalid: starts with number
```

## Reserved Keywords

Keywords that cannot be used as identifiers:

```
if, else, while, for, in, func, return, break, continue,
let, global, switch, case, default, try, catch, finally,
throw, spawn, import, type, true, false, null
```

## Next Steps

- [Data Types](03-data-types.md) - Learn about Nari's data types
- [Operators](04-operators.md) - Understand operators
- [Control Flow](05-control-flow.md) - Control structures
