# Data Types

Nari is dynamically typed, meaning variables can hold values of any type and types are determined at runtime.

## Primitive Types

### Number

Numbers in Nari can be integers or floating-point:

```nari
let integer = 42;
let negative = -17;
let float = 3.14159;
let zero = 0;
let hexLower = 0xff;
let hexUpper = 0xDEADBEEF;
```

All numbers are stored as either 64-bit integers or double-precision floats internally.

### String

Strings represent text:

```nari
let str1 = "Hello";
let str2 = 'World';
let template = `Value: {42}`;
let empty = "";
```

Strings are immutable. See [String Methods](13-string-methods.md) for operations.

### Boolean

Boolean values represent true/false:

```nari
let isTrue = true;
let isFalse = false;
```

### Null

Represents the absence of a value:

```nari
let empty = null;
let uninitialized;  // Also null
```

## Composite Types

### Arrays

Ordered collections of values:

```nari
let numbers = [1, 2, 3, 4, 5];
let mixed = [42, "text", true, null];
let nested = [[1, 2], [3, 4]];
let empty = [];
```

**Array Access:**
```nari
let arr = [10, 20, 30];
print(arr[0]);      // 10
print(arr[1]);      // 20
arr[2] = 40;        // Modify element
```

**Array Type Annotation:**
```nari
let numbers: number[] = [1, 2, 3];
let names: string[] = ["Alice", "Bob"];
```

See [Array Methods](14-array-methods.md) for operations.

### Objects

Key-value maps (also called dictionaries or hash maps):

```nari
let person = {
    name: "Alice",
    age: 30,
    city: "Boston"
};

let empty = {};
```

**Object Access:**
```nari
// Dot notation
print(person.name);     // "Alice"
person.age = 31;        // Modify

// Bracket notation
print(person["city"]);  // "Boston"
let key = "name";
print(person[key]);     // "Alice"
```

**Nested Objects:**
```nari
let config = {
    server: {
        host: "localhost",
        port: 8080
    },
    database: {
        name: "mydb"
    }
};

print(config.server.port);  // 8080
```

See [Object Methods](15-object-methods.md) for operations.

### Functions

Functions are first-class values:

```nari
let add = func(a, b) {
    return a + b;
};

let greet = func(name) {
    return "Hello, " @ name;
};

print(typeof(add));  // "function"
```

See [Functions](06-functions.md) for details.

## Custom Types

Define structured types with type declarations:

```nari
type Person {
    name: string;
    age: number;
    email: string
}

let user: Person = {
    name: "Alice",
    age: 25,
    email: "alice@example.com"
};
```

See [Custom Types](07-custom-types.md) for details.

## Type Checking

### typeof Operator

Get the type of a value as a string:

```nari
print(typeof(42));          // "int"
print(typeof(3.14));        // "float"
print(typeof("text"));      // "string"
print(typeof(true));        // "bool"
print(typeof([1, 2]));      // "array"
print(typeof({a: 1}));      // "object"
print(typeof(func(){}));    // "function"
print(typeof(null));        // "null"
```

### Type Checking Functions

Built-in functions for type checking:

```nari
let value = 42;

is_number(value);    // true
is_string(value);    // false
is_bool(value);      // false
is_array(value);     // false
is_object(value);    // false
is_function(value);  // false

// Examples
is_number(42);               // true
is_string("hello");          // true
is_bool(false);              // true
is_array([1, 2, 3]);         // true
is_object({a: 1});           // true
is_function(func(){});       // true
```

## Type Conversion

### To Number

```nari
let n1 = to_number("42");        // 42
let n2 = to_number("3.14");      // 3.14
let n3 = to_number(true);        // 1
let n4 = to_number(false);       // 0
let n5 = to_number("abc");       // 0 (invalid)
```

### To String

```nari
let s1 = to_string(42);          // "42"
let s2 = to_string(3.14);        // "3.14"
let s3 = to_string(true);        // "true"
let s4 = to_string([1, 2]);      // "[1, 2]"
let s5 = to_string({a: 1});      // "{a: 1}"
```

### To Boolean

```nari
let b1 = to_bool(42);            // true (non-zero)
let b2 = to_bool(0);             // false
let b3 = to_bool("text");        // true (non-empty)
let b4 = to_bool("");            // false (empty)
let b5 = to_bool(null);          // false
let b6 = to_bool([]);            // false (empty array)
let b7 = to_bool([1]);           // true (non-empty)
```

## Truthiness and Falsiness

Values that evaluate to `false` in boolean context:
- `false`
- `null`
- `0`
- `""` (empty string)
- `[]` (empty array)
- `{}` (empty object)

All other values are truthy:

```nari
if (42) {           // truthy
    print("yes");
}

if ("") {           // falsy
    print("no");
}

if ([1, 2]) {       // truthy (non-empty)
    print("yes");
}
```

## Type Coercion

### Automatic Coercion

Some operators automatically coerce types:

```nari
// Arithmetic coerces to number
print("5" + 3);         // 8 (string "5" -> number 5)
print(true + 1);        // 2 (true -> 1)

// String concatenation with @
print(42 @ " items");   // "42 items"
```

### Comparison

```nari
print(5 == "5");        // Type coercion in comparison
print(0 == false);      // true
print(null == null);    // true
```

## Handles (Advanced)

Handles represent asynchronous operations (spawn blocks):

```nari
let handle = spawn {
    return 42;
};

print(typeof(handle));  // "handle"
```

See [Asynchronous Programming](08-async.md) for details.

## Next Steps

- [Operators](04-operators.md) - Learn about operators
- [Custom Types](07-custom-types.md) - Define structured types
- [Array Methods](14-array-methods.md) - Array operations
- [Object Methods](15-object-methods.md) - Object operations
