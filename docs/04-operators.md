# Operators

## Arithmetic Operators

### Basic Arithmetic

```nari
let a = 10
let b = 3;

print(a + b);    // 13  - Addition
print(a - b);    // 7   - Subtraction
print(a * b);    // 30  - Multiplication
print(a / b);    // 3   - Division (integer division for integers)
print(a % b);    // 1   - Modulo (remainder)
```

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

Returns the first falsy value, or the last value if all are truthy:

```nari
print(true && true);         // true
print(true && false);        // false
print(false && true);        // false

// Short-circuit evaluation
print(false && expensive_function());  // false, function not called
```

### OR (||)

Returns the first truthy value, or the last value if all are falsy:

```nari
print(true || false);        // true
print(false || true);        // true
print(false || false);       // false

// Short-circuit evaluation
print(true || expensive_function());  // true, function not called
```

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

x += 5;      // x = x + 5  → 15
x -= 3;      // x = x - 3  → 12
x *= 2;      // x = x * 2  → 24
x /= 4;      // x = x / 4  → 6
x %= 4;      // x = x % 4  → 2
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
- `??` only checks for `null`
- `||` treats all falsy values (0, "", false, etc.) as trigger for right operand

```nari
let count = 0;
print(count || 10);    // 10 (0 is falsy)
print(count ?? 10);    // 0  (0 is not null)

let name = "";
print(name || "Guest");    // "Guest" ("" is falsy)
print(name ?? "Guest");    // "" ("" is not null)
```

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
13. Assignment: `=` `+=` `-=` `*=` `/=` `%=`

**Use parentheses for clarity:**

```nari
let result = (a + b) * c;
let condition = (x > 5) && (y < 10);
```

## Type Coercion in Operators

### Arithmetic Operations

```nari
print(5 + "3");      // 8 (string → number)
print(true + 1);     // 2 (true → 1)
print(false * 10);   // 0 (false → 0)
```

### String Concatenation

```nari
print("Count: " @ 42);        // "Count: 42"
print(true @ " or " @ false); // "true or false"
```

## Next Steps

- [Control Flow](05-control-flow.md) - if/else, loops, switch
- [Functions](06-functions.md) - Function operators and usage
