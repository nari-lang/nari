# Operators

## Arithmetic Operators

### Basic Arithmetic

```nari
let a = 10
let b = 3;

print(a + b);    // 13  - Addition
print(a - b);    // 7   - Subtraction
print(a * b);    // 30  - Multiplication
print(a / b);    // 3.3333... - Division (float unless evenly divisible)
print(a % b);    // 1   - Modulo (remainder)
```

Division returns an integer only when the operands are integers and divide
evenly (e.g. `9 / 3` is `3`); otherwise the result is a float
(e.g. `10 / 3` is `3.3333...`).

### Exponentiation

```nari
print(2 ** 3);     // 8   - 2 to the power of 3
print(4 ** 0.5);   // 2.0 - Square root
print(10 ** 2);    // 100 - 10 squared
```

### Unary Operators

```nari
let x = 5;
print(-x);         // -5  - Negation
print(+x);         // 5   - Unary plus
```

## Comparison Operators

```nari
let a = 5
let b = 10;

print(a == b);     // false - Equal
print(a != b);     // true  - Not equal
print(a < b);      // true  - Less than
print(a > b);      // false - Greater than
print(a <= b);     // true  - Less than or equal
print(a >= b);     // false - Greater than or equal
```

## Logical Operators

### AND (&&)

Evaluates operands left to right, short-circuiting on the first falsy operand.
The result is always a boolean (not the operand value):

```nari
print(true && true);         // true
print(true && false);        // false
print(false && true);        // false

// Short-circuit evaluation
print(false && expensive_function());  // false, function not called
```

### OR (||)

Evaluates operands left to right, short-circuiting on the first truthy operand.
The result is always a boolean (not the operand value):

```nari
print(true || false);        // true
print(false || true);        // true
print(false || false);       // false

// Short-circuit evaluation
print(true || expensive_function());  // true, function not called
```

> Note: `&&` and `||` always produce a boolean. To supply a default value
> based on an operand, use the nullish coalescing operator `??` (see below).
> For example, `count || 10` is `true` when `count` is `0`, whereas
> `count ?? 10` is `0`.

### NOT (!)

```nari
print(!true);                // false
print(!false);               // true
print(!!42);                 // true (double negation converts to boolean)
```

### Logical Combinations

```nari
let age = 25;
let hasLicense = true;

if (age >= 18 && hasLicense) {
    print("Can drive");
}

if (age < 18 || !hasLicense) {
    print("Cannot drive");
}
```

## Assignment Operators

### Simple Assignment

```nari
let x = 10;
x = 20;
```

### Compound Assignment

```nari
let x = 10;

x += 5;      // x = x + 5  -> 15
x -= 3;      // x = x - 3  -> 12
x *= 2;      // x = x * 2  -> 24
x /= 4;      // x = x / 4  -> 6
x %= 4;      // x = x % 4  -> 2
```

Bitwise compound assignment operators are also supported:

```nari
let f = 12;

f &= 10;     // f = f & 10  (bitwise AND)
f |= 1;      // f = f | 1   (bitwise OR)
f ^= 3;      // f = f ^ 3   (bitwise XOR)
f <<= 2;     // f = f << 2  (left shift)
f >>= 1;     // f = f >> 1  (right shift)
```

## Increment/Decrement Operators

### Post-increment/decrement

Returns the old value, then increments/decrements:

```nari
let x = 5;
print(x++);    // Prints 5, x becomes 6
print(x);      // Prints 6

let y = 5;
print(y--);    // Prints 5, y becomes 4
print(y);      // Prints 4
```

### Pre-increment/decrement

Increments/decrements first, then returns the new value:

```nari
let x = 5;
print(++x);    // Prints 6, x is now 6

let y = 5;
print(--y);    // Prints 4, y is now 4
```

### In Loops

```nari
for (let i = 0; i < 5; i++) {
    print(i);  // 0, 1, 2, 3, 4
}

let count = 10;
while (count > 0) {
    print(count);
    count--;
}
```

## String Operators

### Concatenation (@)

```nari
let first = "Hello";
let second = "World";
let result = first @ " " @ second;  // "Hello World"

// Chaining
let msg = "Age: " @ 25 @ " years";  // "Age: 25 years"
```

### Concatenation with +=

```nari
let str = "Hello";
str += " World";     // "Hello World"
```

Note: The `+` operator also works for concatenation but `@` is preferred for clarity.

## Ternary Operator

Conditional expression: `condition ? valueIfTrue : valueIfFalse`

```nari
let age = 20;
let status = age >= 18 ? "adult" : "minor";
print(status);  // "adult"

// Inline usage
print(5 > 3 ? "yes" : "no");  // "yes"

// Nested ternary
let score = 85;
let grade = score >= 90 ? "A" : 
            score >= 80 ? "B" :
            score >= 70 ? "C" : "F";
print(grade);  // "B"
```

## Nullish Coalescing Operator (??)

Returns the right operand when the left operand is `null`, otherwise returns the left operand:

```nari
let name = null;
let displayName = name ?? "Guest";
print(displayName);  // "Guest"

let actualName = "Alice";
displayName = actualName ?? "Guest";
print(displayName);  // "Alice"

// Chaining
let a = null;
let b = null;
let c = "Default";
let value = a ?? b ?? c;
print(value);  // "Default"
```

**Difference from OR (||):**
- `??` returns the left operand's value unless it is `null`, otherwise the right operand.
- `||` always returns a boolean and treats all falsy values (0, "", false, etc.) as triggering the right operand.

```nari
let count = 0;
print(count || 10);    // true (|| yields a bool; 0 is falsy so the right side is taken)
print(count ?? 10);    // 0    (?? returns the value; 0 is not null)

let name = "";
print(name || "Guest");    // true (|| yields a bool; "" is falsy)
print(name ?? "Guest");    // ""   (?? returns the value; "" is not null)
```

Because `||` yields a boolean, `??` is the correct operator for
default-value patterns.

## Member Access Operators

### Dot Notation

```nari
let obj = { name: "Alice", age: 30 };
print(obj.name);     // "Alice"
obj.age = 31;        // Set property
```

### Bracket Notation

```nari
let obj = { name: "Alice" };
print(obj["name"]);  // "Alice"

// Dynamic property access
let key = "age";
obj[key] = 30;
print(obj[key]);     // 30
```

### Array Indexing

```nari
let arr = [10, 20, 30];
print(arr[0]);       // 10
arr[1] = 25;         // Modify
print(arr[1]);       // 25
```

## Operator Precedence

From highest to lowest:

1. Member access: `.` `[]`
2. Function call: `()`
3. Unary: `!` `-` `+` `++` `--`
4. Exponentiation: `**`
5. Multiplicative: `*` `/` `%`
6. Additive: `+` `-` `@`
7. Comparison: `<` `>` `<=` `>=`
8. Equality: `==` `!=`
9. Logical AND: `&&`
10. Logical OR: `||`
11. Nullish coalescing: `??`
12. Ternary: `?:`
13. Assignment: `=` `+=` `-=` `*=` `/=` `%=` `&=` `|=` `^=` `<<=` `>>=`

**Use parentheses for clarity:**

```nari
let result = (a + b) * c;
let condition = (x > 5) && (y < 10);
```

## Type Coercion in Operators

### Arithmetic Operations

```nari
print(5 + "3");      // "53" (+ concatenates when either operand is a string)
print(true + 1);     // 2 (true -> 1)
print(false * 10);   // 0 (false -> 0)
```

Note: `+` performs string concatenation whenever either operand is a string;
it does not coerce strings to numbers. Use `to_number(...)` for explicit
numeric conversion.

### String Concatenation

```nari
print("Count: " @ 42);        // "Count: 42"
print(true @ " or " @ false); // "true or false"
```

## Next Steps

- [Control Flow](05-control-flow.md) - if/else, loops, switch
- [Functions](06-functions.md) - Function operators and usage
