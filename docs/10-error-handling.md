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

Fallible operations return a `Result`. You inspect it instead of catching an exception:

```nari
let result = json.parse(text);
if (result.is_ok()) {
    let data = result.unwrap();
    print(data);
} else {
    print("Parse error: " @ result.unwrap_err());
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
    print("Error: " @ r.unwrap_err());  // Error: Division by zero
}
```

### Result methods

| Method            | Description                                                        |
| ----------------- | ------------------------------------------------------------------ |
| `is_ok()`         | `true` when the value is `Ok`                                       |
| `is_err()`        | `true` when the value is `Err`                                      |
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
    return None;
}

let found = find([1, 2, 3], 2);
if (found.is_some()) {
    print("Found: " @ found.unwrap());
}
```

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

Return the `Result` up the call stack and let the caller decide:

```nari
func loadConfig(path) {
    let raw = fs.read_file_sync(path);   // returns Result
    if (raw.is_err()) {
        return raw;                       // propagate the Err
    }
    return json.parse(raw.unwrap());      // also a Result
}

let cfg = loadConfig("config.json");
if (cfg.is_err()) {
    print("Could not load config: " @ cfg.unwrap_err());
}
```

`and_then` expresses the same chaining more compactly:

```nari
func loadConfig(path) {
    return fs.read_file_sync(path).and_then(func(raw) {
        return json.parse(raw);
    });
}
```

### Fallback values

```nari
func getConfigValue(config, key, fallback) {
    let v = config.get(key);   // returns Option
    return v.unwrap_or(fallback);
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
    print(e.type @ ": " @ e.message);
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
        print("Attempt " @ attempts @ " failed: " @ r.unwrap_err());
    }
    return Err("Max retries exceeded");
}
```

## Async errors

Async I/O handles report failure through their own members rather than throwing:

```nari
let handle = fs.read_file("data.txt");
while (!handle.ready) {}
if (handle.failed) {
    print("Read failed: " @ handle.error);
} else {
    print(handle.value);
}
```

A handle exposes `.ready`, `.failed`, `.error`, and `.value`. Reading `.value`
on a failed handle panics, so check `.failed` first when the failure is
expected.

## Best Practices

- Return `Result`/`Option` for anything that can fail as part of normal
  operation; reserve `panic` for bugs and unrecoverable states.
- Prefer `is_ok()`/`is_err()` checks or `unwrap_or` over bare `unwrap()` so an
  expected error does not turn into a panic.
- Put contextual information in the `Err` payload (type, message, offending
  value) to make failures easier to diagnose.
- Propagate errors with `and_then`/early `return` instead of swallowing them.

## Next Steps

- [Builtins](11-builtins.md) - Built-in functions and their `Result` returns
- [Custom Types](07-custom-types.md) - Using custom types for structured errors
