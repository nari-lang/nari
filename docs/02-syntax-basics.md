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

A `let` or `const` declaration takes no type annotation. The type comes from the
value:

```nari
let age = 25;
let name = "Bob";
let items = [1, 2, 3];
let person = { name: "Alice", age: 30 };
```

Annotations are accepted in three places: function parameters, a function return
type after `->`, and the fields of a `type` or `class` declaration.

```nari
type Person {
    name: string;
    age: number
}

func greet(who: string, times: number) -> string {
    return who @ "!";
}

class Point {
    public x: number = 0;
    public y: number = 0;
}
```

Annotations are documentation only. Nothing checks them at run time.

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

### Destructuring Assignment

Destructuring allows you to extract multiple values from arrays or objects into separate variables.

#### Array Destructuring

Extract values from arrays by position:

```nari
let [a, b, c] = [1, 2, 3];
print(a);  // 1
print(b);  // 2
print(c);  // 3
```

**With fewer variables than elements:**
```nari
let numbers = [10, 20, 30, 40, 50];
let [first, second] = numbers;
print(first);   // 10
print(second);  // 20
// Remaining elements are ignored
```

**With more variables than elements:**
```nari
let short = [100, 200];
let [x, y, z] = short;
print(x);  // 100
print(y);  // 200
print(z);  // null (missing elements become null)
```

**Destructuring function returns:**
```nari
func getCoordinates() {
    return [42, 84];
}

let [x, y] = getCoordinates();
print(x);  // 42
print(y);  // 84
```

#### Object Destructuring

Extract values from objects by key:

```nari
let person = { name: "Alice", age: 30, city: "NYC" };
let {name, age} = person;
print(name);  // Alice
print(age);   // 30
```

**With different variable names:**
```nari
let coords = { x: 100, y: 200 };
let {x: posX, y: posY} = coords;
print(posX);  // 100
print(posY);  // 200
```

**With missing properties:**
```nari
let partial = { foo: "bar" };
let {foo, baz} = partial;
print(foo);  // bar
print(baz);  // null (missing properties become null)
```

**Destructuring function returns:**
```nari
func getUserInfo() {
    return { username: "bob123", email: "bob@example.com" };
}

let {username, email} = getUserInfo();
print(username);  // bob123
print(email);     // bob@example.com
```

#### Global Destructuring

Destructuring works with `global` as well:

```nari
global [globalA, globalB] = [999, 888];
global {alpha, beta} = {alpha: "A", beta: "B"};
```

#### Requirements

> [!IMPORTANT]
> Destructuring assignments **must** be initialized. This is invalid:
> ```nari
> let [a, b];  // Error: requires initialization
> ```

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

Use double quotes:

```nari
let greeting = "Hello";
let name = "World";
```

> Note: string literals use double quotes (`"`). Single quotes are not string
> delimiters. Backticks are used for template strings (see below).

### String Interpolation

Use backticks for template strings with interpolation:

```nari
let name = "Alice";
let age = 30;
let msg = `My name is {name} and I am {age} years old.`;

// Expressions in interpolation
let x = 5;
let y = 10;
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
let, global, switch, case, default, spawn, import, type,
true, false, null
```

## Next Steps

- [Data Types](03-data-types.md) - Learn about Nari's data types
- [Operators](04-operators.md) - Understand operators
- [Control Flow](05-control-flow.md) - Control structures
