# Error Handling

Nari does **not** have `try`/`catch`/`finally`. Recoverable errors are modeled
as values using the `Result<T, E>` and `Option<T>` enums from the prelude, and
the `panic(value)` builtin is reserved for **unrecoverable failures** that abort
the program (similar to Rust's `panic!`).

## Result and Option

The prelude defines two enums that are always available:

```nari
enum Result<T, E> { Ok(T), Err(E) }
enum Option<T>    { Some(T), None }
```

`Ok`, `Err`, `Some` and `None` are all constructor **functions**. Call `None()`
with parentheses. A bare `None` is the function itself, not an empty `Option`.

Fallible operations return a `Result`. You inspect it instead of catching an exception:

```nari
let text = "{\"id\": 7}";
let result = JSON.parse(text);
if (result.is_ok()) {
    let data = result.unwrap();
    print(data);
} else {
    print("Parse error: " + result.unwrap_err());
}
```

### Constructing Results

Use the `Ok` and `Err` constructors:

```nari
func safeDivide(a, b) {
    if (b == 0) {
        return Err("Division by zero");
    }
    return Ok(a / b);
}

let r = safeDivide(10, 0);
if (r.is_err()) {
    print("Error: " + r.unwrap_err());  // Error: Division by zero
}
```

### Result methods

| Method            | Description                                                        |
| ----------------- | ------------------------------------------------------------------ |
| `is_ok()`         | `true` when the value is `Ok`                                      |
| `is_err()`        | `true` when the value is `Err`                                     |
| `expect(msg)`     | Returns the `Ok` value, or **panics** with `msg` if it is `Err`    |
| `unwrap()`        | Returns the `Ok` value, or **panics** if it is `Err`               |
| `unwrap_err()`    | Returns the `Err` value, or **panics** if it is `Ok`               |
| `unwrap_or(def)`  | Returns the `Ok` value, or `def` when `Err`                        |
| `map(f)`          | Transforms the `Ok` value, leaving `Err` untouched                 |
| `map_err(f)`      | Transforms the `Err` value, leaving `Ok` untouched                 |
| `and_then(f)`     | Chains another `Result`-returning operation on the `Ok` value      |
| `or_else(f)`      | Provides a fallback `Result` when `Err`                            |

### Option

`Option` represents a value that may be absent:

```nari
func find(list, target) {
    for (item in list) {
        if (item == target) {
            return Some(item);
        }
    }
    return None();
}

let found = find([1, 2, 3], 2);
if (found.is_some()) {
    print("Found: " + found.unwrap());
}
```

## Error propagation with `?`

Attach `?` to a `Result` expression to unwrap `Ok(value)` or return the original
`Err(error)` from the current function. The operand is evaluated once, and an
error skips the rest of the current expression and function.

```nari
func loadConfig(path) {
    let text = fs.read_file(path).await?;
    let config = JSON.parse(text)?;
    return Ok(config);
}
```

Wrap a successful return in `Ok(...)` when the caller expects a `Result`; `?`
does not add that wrapper. Error payloads and the original `Err` are preserved.
You can add context before propagation with `operation().map_err(...)?`.

`?` works inside named functions, anonymous functions, methods, arrow functions
and `spawn` blocks. In a closure or `spawn` it returns from that body, rather
than the function that created it. It also works in default parameter values
and string interpolation. A propagated return follows the same strict-mode
return-type checks as an explicit `return`.

The operator expects a `Result`. Applying it to an `Option`, an I/O handle or
another value is a type error. Wait for asynchronous results explicitly:
`operation().await?`.

At the program entry point there is no caller to receive a propagated `Err`,
so the runtime reports it instead of silently discarding it, it prints `Error: <payload>` to stderr and exits nonzero.

```nari
// Either entry form reports a failed parse and exits nonzero:
let config = JSON.parse(text)?;          // top-level

func start() {
    let config = JSON.parse(text)?;      // auto-called entry point
}
```

Whitespace distinguishes propagation from a ternary:

```nari
let value = operation()?;              // attached: propagate
let selected = condition ? yes : no;   // space before ?: ternary
```

Compact ternaries such as `condition?yes:no` need a space before `?`.
Propagation has postfix precedence, so `result? + 1` adds to the unwrapped
value. Existing `?.` optional chaining and `??` null coalescing keep their
meanings. Use `(result?).field` to access a propagated payload's property,
and `(nested?)?` to propagate two nested Results.

## Unrecoverable panics

`panic(value)` raises an **uncatchable** panic. There is no way to recover from
it in Nari code: it unwinds all the way to the top and stops the program. It is
a builtin function and can be used anywhere an expression is accepted. Use it
only for programmer errors and truly unrecoverable conditions.

```nari
func mustBePositive(n) {
    if (n <= 0) {
        panic("expected a positive number");  // aborts the program
    }
    return n;
}
```

`unwrap()` and `unwrap_err()` call `panic`, so calling
`unwrap()` on an `Err` panics. Prefer explicit `is_ok()`/`is_err()` checks or
`unwrap_or` when the error is expected and recoverable.

## Patterns

### Propagating errors

Use `?` to forward errors and let the caller decide how to handle them.
The explicit equivalent is:

```nari
func loadConfig(path) {
    let raw = fs.read_file(path).await;   // gives a Result
    if (raw.is_err()) {
        return raw;                       // propagate the Err
    }
    return JSON.parse(raw.unwrap());      // also a Result
}

let cfg = loadConfig("config.json");
if (cfg.is_err()) {
    print("Could not load config: " + cfg.unwrap_err());
}
```

`and_then` expresses the same chaining more compactly:

```nari
func loadConfig(path) {
    return fs.read_file(path).await.and_then(func(raw) {
        return JSON.parse(raw);
    });
}
```

### Fallback values

```nari
func getConfigValue(config, key, fallback) {
    if (config.has_key(key)) {
        return config[key];
    }
    return fallback;
}
```

Objects have no `get` method and never give an `Option`. If you want the
`Option` shape, build it yourself:

```nari
func lookup(config, key) {
    if (config.has_key(key)) {
        return Some(config[key]);
    }
    return None();
}
```

### Structured errors

The `Err` payload can be any value, including an object with context:

```nari
func processRequest(request) {
    if (!request) {
        return Err({ type: "ValueError", message: "Request is null" });
    }
    if (!request.url) {
        return Err({ type: "ValueError", message: "Missing URL" });
    }
    return Ok(request);
}

let r = processRequest({ method: "DELETE" });
if (r.is_err()) {
    let e = r.unwrap_err();
    print(e.type + ": " + e.message);
}
```

### Retry logic

```nari
func retry(operation, maxAttempts) {
    let attempts = 0;
    while (attempts < maxAttempts) {
        let r = operation();          // returns Result
        if (r.is_ok()) {
            return r;
        }
        attempts = attempts + 1;
        print("Attempt " + attempts + " failed: " + r.unwrap_err());
    }
    return Err("Max retries exceeded");
}
```

## Best Practices

- Return `Result`/`Option` for anything that can fail as part of normal
  operation; reserve `panic` for bugs and unrecoverable states.
- Prefer `is_ok()`/`is_err()` checks or `unwrap_or` over bare `unwrap()` so an
  expected error does not turn into a panic.
- Put contextual information in the `Err` payload (type, message, offending
  value) to make failures easier to diagnose.
- Propagate errors with `?`, `and_then`, or an explicit early `return`.

## Next Steps

- [Builtins](11-builtins.md) - Built-in functions and their `Result` returns
- [Custom Types](07-custom-types.md) - Using custom types for structured errors
