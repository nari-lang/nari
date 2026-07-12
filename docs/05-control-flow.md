# Control Flow

## Conditional Statements

### if Statement

```nari
let age = 20;

if (age >= 18) {
    print("Adult");
}
```

Single-statement form is also supported:

```nari
if (age >= 18) print("Adult");
```

### if-else Statement

```nari
let score = 75;

if (score >= 60) {
    print("Pass");
} else {
    print("Fail");
}
```

You can also use single statements on either side:

```nari
if (score >= 60) print("Pass");
else print("Fail");
```

### if-else if-else Chain

```nari
let grade = 85;

if (grade >= 90) {
    print("A");
} else if (grade >= 80) {
    print("B");
} else if (grade >= 70) {
    print("C");
} else if (grade >= 60) {
    print("D");
} else {
    print("F");
}
```

### Nested if Statements

```nari
let age = 25;
let hasLicense = true;

if (age >= 18) {
    if (hasLicense) {
        print("Can drive");
    } else {
        print("Need license");
    }
} else {
    print("Too young");
}
```

## Loops

### while Loop

```nari
let i = 0;
while (i < 5) {
    print(i);
    i++;
}
// Output: 0, 1, 2, 3, 4
```

### for Loop

Traditional C-style for loop:

```nari
for (let i = 0; i < 5; i++) {
    print(i);
}
// Output: 0, 1, 2, 3, 4
```

**Loop with let:**
```nari
for (let i = 1; i <= 10; i++) {
    print("Count: " @ i);
}
```

**Loop with assignment:**
```nari
let j = 0;
for (j = 0; j < 3; j++) {
    print(j);
}
```

### for-in Loop (for-each)

Iterate over array elements:

```nari
let fruits = ["apple", "banana", "cherry"];

for (fruit in fruits) {
    print(fruit);
}
// Output: apple, banana, cherry
```

**With indices:**
```nari
let numbers = [10, 20, 30];
let index = 0;

for (num in numbers) {
    print("Index " @ index @ ": " @ num);
    index++;
}
```

**Iterating over object values:**
```nari
let obj = {a: 1, b: 2, c: 3};
let vals = obj.values();

for (val in vals) {
    print(val);
}
```

## Loop Control

### break Statement

Exit a loop early:

```nari
for (let i = 0; i < 10; i++) {
    if (i == 5) {
        break;  // Exit loop when i is 5
    }
    print(i);
}
// Output: 0, 1, 2, 3, 4
```

**In nested loops:**
```nari
for (let i = 0; i < 3; i++) {
    for (let j = 0; j < 3; j++) {
        if (j == 2) {
            break;  // Only breaks inner loop
        }
        print("i=" @ i @ ", j=" @ j);
    }
}
```

### continue Statement

Skip to next iteration:

```nari
for (let i = 0; i < 5; i++) {
    if (i == 2) {
        continue;  // Skip when i is 2
    }
    print(i);
}
// Output: 0, 1, 3, 4
```

**Skipping even numbers:**
```nari
for (let i = 0; i < 10; i++) {
    if (i % 2 == 0) {
        continue;
    }
    print(i);  // Only odd numbers
}
```

## switch Statement

Multi-way branching:

```nari
let day = 2;

switch (day) {
    case 1:
        print("Monday");
    case 2:
        print("Tuesday");
    case 3:
        print("Wednesday");
    default:
        print("Other day");
}
```

**Note:** Nari's switch statement executes **only the first matching case**.
There is no C-style fall-through: after a matching case body runs, control jumps
past the rest of the switch. The `default` branch runs only when no case matches.
(The example above prints only `"Tuesday"`.)

**With strings:**
```nari
let command = "start";

switch (command) {
    case "start":
        print("Starting...");
    case "stop":
        print("Stopping...");
    case "restart":
        print("Restarting...");
    default:
        print("Unknown command");
}
```

**Multiple conditions:**
```nari
let value = 5;

switch (value) {
    case 1:
        print("One");
    case 2:
        print("Two");
    case 3:
        print("Three");
    case 4:
        print("Four");
    case 5:
        print("Five");
    default:
        print("Other");
}
// Prints: "Five"
```

## return Statement

Exit a function and optionally return a value:

```nari
func add(a, b) {
    return a + b;  // Return value and exit
}

func checkAge(age) {
    if (age < 18) {
        return;  // Return without value (returns null)
    }
    print("Adult");
}

// Early return pattern
func findValue(arr, target) {
    for (item in arr) {
        if (item == target) {
            return item;  // Exit early when found
        }
    }
    return null;  // Not found
}
```

## Conditional Expressions

### Ternary Operator

Shorthand for if-else:

```nari
let age = 20;
let status = age >= 18 ? "adult" : "minor";

// Inline usage
print(score > 60 ? "Pass" : "Fail");

// Nested
let grade = score >= 90 ? "A" : score >= 80 ? "B" : "C";
```

### Nullish Coalescing

Provide default values:

```nari
let username = null;
let display = username ?? "Guest";

// Chaining
let value = a ?? b ?? c ?? "default";
```

## Pattern Matching

Nari supports `match` expressions for concise pattern matching:

```nari
func classify(val) {
    return match val {
        0 => "zero",
        1 => "one",
        _ => "other"
    };
}

print(classify(1));   // "one"
print(classify(99));  // "other"
```

### Match with Enum Variants

```nari
let result = Ok(42);

let output = match result {
    Ok(value) => "Success: " @ to_string(value),
    Err(error) => "Error: " @ error
};

print(output);  // "Success: 42"
```

See [Generics and Enums](16-generics-enums.md) for more on pattern matching.
```

## Control Flow Best Practices

### 1. Prefer early returns

```nari
// Good
func validate(user) {
    if (!user) return false;
    if (!user.name) return false;
    if (user.age < 0) return false;
    return true;
}

// Less clear
func validate(user) {
    let valid = true;
    if (user) {
        if (user.name) {
            if (user.age >= 0) {
                valid = true;
            } else {
                valid = false;
            }
        } else {
            valid = false;
        }
    } else {
        valid = false;
    }
    return valid;
}
```

### 2. Use for-in for arrays

```nari
// Good
for (item in items) {
    process(item);
}

// More verbose
for (let i = 0; i < items.length(); i++) {
    process(items[i]);
}
```

### 3. Avoid deep nesting

```nari
// Good
func process(data) {
    if (!isValid(data)) return;
    
    let result = transform(data);
    if (!result) return;
    
    save(result);
}

// Deep nesting (avoid)
func process(data) {
    if (isValid(data)) {
        let result = transform(data);
        if (result) {
            save(result);
        }
    }
}
```

## Next Steps

- [Functions](06-functions.md) - Function declarations and usage
- [Error Handling](10-error-handling.md) - Result/Option and panic
