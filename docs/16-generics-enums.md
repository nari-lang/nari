# Generics and Enums

Advanced type system features for expressing generic algorithms and sum types.

## Generic Type Parameters

Type declarations can include generic parameters for reusable data structures.

### Type Declaration Syntax

```nari
type Box<T> {
    value: T;
}

type Pair<A, B> {
    first: A;
    second: B;
}

type Result<T, E> {
    // Used with enum variants
}
```

Generic parameters are identifiers (conventionally single uppercase letters) that represent type variables.

**Note:** Generic parameters in type declarations are currently for documentation - runtime type checking is dynamic.

## Enum Types

Enums (enumerations) define sum types with multiple variants.

### Basic Enum Syntax

```nari
enum Direction {
    North,
    South,
    East,
    West
}

enum Status {
    Pending,
    InProgress,
    Completed,
    Failed
}
```

### Enums with Generic Parameters

```nari
enum Option<T> {
    Some(T),
    None
}

enum Result<T, E> {
    Ok(T),
    Err(E)
}
```

### Variant Types

Enums support three kinds of variants:

#### 1. Unit Variants

Simple variants with no associated data:

```nari
enum Bool {
    True,
    False
}
```

#### 2. Tuple Variants

Variants that hold unnamed data:

```nari
enum Option<T> {
    Some(T),      // Holds one value
    None          // Holds nothing
}

enum Result<T, E> {
    Ok(T),        // Holds success value
    Err(E)        // Holds error value
}
```

#### 3. Struct Variants

Variants with named fields (not yet fully implemented):

```nari
enum Message {
    Text { content: string },
    Image { url: string, width: number, height: number }
}
```

## Creating Enum Values

Enum variants are constructed using helper functions that create objects with special fields:

```nari
// Define constructors for Result<T, E>
func Ok(value) {
    return {
        __variant: "Ok",
        __data: value
    };
}

func Err(error) {
    return {
        __variant: "Err",
        __data: error
    };
}

// Use the constructors
let success = Ok(42);
let failure = Err("something went wrong");
```

**Convention:** Constructor function names match variant names.

**Note:** The standard library automatically provides `Ok`, `Err`, `Some`, and `None` constructors, so you don't need to define them yourself. They are shown here to illustrate how enum values work internally.

## Pattern Matching

Match expressions destructure enum values and bind variables.

### Basic Match Syntax

```nari
match expression {
    pattern1 => result1,
    pattern2 => result2,
    pattern3 => result3
}
```

### Pattern Types

#### Variant Patterns

Match and destructure enum variants:

```nari
let result = Ok(42);

match result {
    Ok(value) => print("Success: " @ toString(value)),
    Err(error) => print("Error: " @ error)
}
// Prints: "Success: 42"
```

The pattern `Ok(value)` matches Ok variants and binds the data to `value`.

#### Literal Patterns

Match specific values:

```nari
func classify(n) {
    return match n {
        0 => "zero",
        1 => "one",
        2 => "two",
        _ => "other"
    };
}

print(classify(1));  // "one"
print(classify(99)); // "other"
```

#### Binding Patterns

Bind any value to a variable:

```nari
let x = 42;

match x {
    value => print("The value is: " @ toString(value))
}
// Prints: "The value is: 42"
```

#### Wildcard Pattern

Match anything without binding:

```nari
match someValue {
    Ok(_) => print("Success (don't care about value)"),
    Err(_) => print("Error (don't care about details)")
}
```

### Match Expression Evaluation

- Match tries each arm in order
- First matching pattern wins
- Match expression returns the arm's result value
- All arms must be exhaustive (or use wildcard as last resort)

```nari
let result = match value {
    pattern1 => expr1,  // If pattern1 matches, return expr1
    pattern2 => expr2,  // Else if pattern2 matches, return expr2
    _ => exprDefault    // Else return exprDefault
};
```

## Common Patterns

### Option<T> Pattern

Represent optional values:

```nari
func Some(value) {
    return {
        __variant: "Some",
        __data: value
    };
}

func None() {
    return {
        __variant: "None"
    };
}

func find(arr, predicate) {
    for (item in arr) {
        if (predicate(item)) {
            let some = Some(item);
            return some;
        }
    }
    return None();
}

let numbers = [1, 2, 3, 4, 5];
let found = find(numbers, func(x) { return x > 3; });

match found {
    Some(value) => print("Found: " @ toString(value)),
    None => print("Not found")
}
// Prints: "Found: 4"
```

### Result<T, E> Pattern

Represent operations that can fail:

```nari
func Ok(value) {
    return { __variant: "Ok", __data: value };
}

func Err(error) {
    return { __variant: "Err", __data: error };
}

func safeDivide(a, b) {
    if (b == 0) {
        let err = Err("Division by zero");
        return err;
    }
    let ok = Ok(a / b);
    return ok;
}

let result = safeDivide(10, 2);

match result {
    Ok(value) => print("Result: " @ toString(value)),
    Err(error) => print("Error: " @ error)
}
// Prints: "Result: 5"
```

### Chaining Operations

Transform Result values while preserving errors:

```nari
func mapResult(result, transform) {
    return match result {
        Ok(value) => Ok(transform(value)),
        Err(error) => Err(error)
    };
}

func unwrapOr(result, defaultValue) {
    return match result {
        Ok(value) => value,
        Err(_) => defaultValue
    };
}

let result = Ok(5);
let doubled = mapResult(result, func(x) { return x * 2; });
print(unwrapOr(doubled, 0));  // 10

let failed = Err("error");
let attempted = mapResult(failed, func(x) { return x * 2; });
print(unwrapOr(attempted, -1));  // -1
```

## Practical Examples

### Input Validation

```nari
func parseNumber(str) {
    // Validation logic here
    let num = toNumber(str);
    if (isValid) {
        let ok = Ok(num);
        return ok;
    }
    let err = Err("Invalid format");
    return err;
}

func processInput(input) {
    let parseResult = parseNumber(input);
    
    return match parseResult {
        Ok(num) => validateRange(num),
        Err(error) => Err(error)
    };
}

let result = processInput("42");
match result {
    Ok(n) => print("Valid: " @ toString(n)),
    Err(e) => print("Invalid: " @ e)
}
```

### Safe Array Access

```nari
func at(arr, index) {
    if (index >= 0 && index < arr.length()) {
        let some = Some(arr[index]);
        return some;
    }
    return None();
}

let numbers = [10, 20, 30];
let item = at(numbers, 1);

match item {
    Some(value) => print("Item: " @ toString(value)),
    None => print("Index out of bounds")
}
// Prints: "Item: 20"
```

### Error Propagation

```nari
func readConfig(filename) {
    let fileResult = readFile(filename);
    
    return match fileResult {
        Ok(content) => parseConfig(content),
        Err(error) => Err("Failed to read: " @ error)
    };
}

func parseConfig(content) {
    // Parsing logic
    if (valid) {
        let ok = Ok(config);
        return ok;
    }
    let err = Err("Invalid format");
    return err;
}
```

## Limitations

### Current Limitations

1. **Generic constraints:** No way to constrain generic parameters
2. **Struct variants:** Named fields in enum variants not fully implemented
3. **Nested patterns:** Complex nested pattern matching limited
4. **Type inference:** Generic types not inferred, must be explicit in constructors
5. **Exhaustiveness checking:** No compile-time checks for missing match arms

### Workarounds

**Function call returns in if statements:**

Instead of:
```nari
if (condition) {
    return Ok(value);  // May not work correctly
}
```

Use:
```nari
if (condition) {
    let result = Ok(value);
    return result;  // Works reliably
}
```

## Best Practices

### 1. Use Result for Errors

Prefer Result<T, E> over exceptions for expected errors:

```nari
// Good
func safeDivide(a, b) {
    if (b == 0) {
        let err = Err("Division by zero");
        return err;
    }
    let ok = Ok(a / b);
    return ok;
}

// Avoid
func unsafeDivide(a, b) {
    if (b == 0) {
        throw "Division by zero";
    }
    return a / b;
}
```

### 2. Use Option for Nullable Values

Prefer Option<T> over null:

```nari
// Good
func findUser(id) {
    if (exists) {
        let some = Some(user);
        return some;
    }
    return None();
}

// Avoid
func findUser(id) {
    if (exists) {
        return user;
    }
    return null;
}
```

### 3. Handle All Cases

Always handle all variants:

```nari
// Good - handles all cases
match result {
    Ok(value) => processValue(value),
    Err(error) => handleError(error)
}

// Risky - might miss cases
match result {
    Ok(value) => processValue(value),
    _ => "ignored"
}
```

### 4. Keep Constructors Simple

Constructor functions should be simple object creators:

```nari
// Good
func Ok(value) {
    return { __variant: "Ok", __data: value };
}

// Avoid complex logic in constructors
func Ok(value) {
    validate(value);
    log(value);
    return { __variant: "Ok", __data: transformed(value) };
}
```

## Next Steps

- [Error Handling](10-error-handling.md) - Error handling strategies
- [Custom Types](07-custom-types.md) - Product types with struct-like data
- [C Foreign Function Interface](17-ffi.md) - Call C library functions directly from Nari
