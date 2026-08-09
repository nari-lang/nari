# Standard Library

The Nari standard library provides high-level modules for common tasks. It is automatically loaded and available without imports.

## Implicit globals

Every program gets these globals without an import:

| Global | Purpose |
|---|---|
| `math` | Numeric functions and constants |
| `fs` | Files and directories (async) |
| `http` | HTTP requests (async) |
| `net` | TCP and UDP sockets |
| `JSON` | Parse and serialise JSON |
| `platform` | Host machine facts |
| `process` | Arguments, standard input, exit, shell |
| `Spawn` | Combinators over spawn handles |
| `Object`, `Array`, `String` | Extra helpers for those types |
| `Ok`, `Err`, `Some`, `None` | `Result` and `Option` constructors |
| `stdlib_version` | Standard library version string |

There is no `system` global and no `io` global. Reach the modules by their own
names, as shown below.

```nari
print(stdlib_version);   // "0.0.3"
```

Everything else needs an explicit import, for example
`import { Date } from "std/date";`. Refer to
[stdlib-reference.md](stdlib-reference.md) for the importable modules.

## math Module

Mathematical functions and utilities.

### Constants

- `math.PI` - 3.141592653589793
- `math.E` - 2.718281828459045

### math.pow

Raise a number to a power.

```nari
math.pow(2, 3);      // 8
math.pow(10, 2);     // 100
```

**Note:** Can also use the `**` operator: `2 ** 3`

### math.square

Square a number.

```nari
math.square(5);      // 25
math.square(10);     // 100
```

### math.sqrt

Calculate square root.

```nari
math.sqrt(16);       // 4
math.sqrt(25);       // 5
math.sqrt(2);        // 1.414...
```

### math.abs

Get absolute value.

```nari
math.abs(-5);        // 5
math.abs(10);        // 10
math.abs(-3.14);     // 3.14
```

### math.min

Get the minimum of two numbers.

```nari
math.min(5, 10);     // 5
math.min(-3, 2);     // -3
```

### math.max

Get the maximum of two numbers.

```nari
math.max(5, 10);     // 10
math.max(-3, 2);     // 2
```

### Additional functions

- `math.clamp(n, lo, hi)` - Constrain `n` to the range `[lo, hi]`.
- `math.round(n)` - Round to the nearest integer.
- `math.floor(n)` - Round down to the nearest integer.
- `math.ceil(n)` - Round up to the nearest integer.
- `math.rand()` - Random float in `[0, 1)`.
- `math.sin(x)`, `math.cos(x)`, `math.tan(x)` - Trigonometric functions (radians).
- `math.atan(x)`, `math.atan2(y, x)` - Inverse tangent functions.
- `math.exp(x)` - `e` raised to the power `x`.
- `math.log(x)` - Natural logarithm.
- `math.log10(x)`, `math.log2(x)` - Base-10 and base-2 logarithms.

```nari
print(math.clamp(15, 0, 10));  // 10
print(math.round(2.6));        // 3
print(math.floor(2.9));        // 2
print(math.ceil(2.1));         // 3
print(math.log2(8));           // 3
```

### Example: Using math module

```nari
func calculateDistance(x1, y1, x2, y2) {
    let dx = x2 - x1;
    let dy = y2 - y1;
    return math.sqrt(math.pow(dx, 2) + math.pow(dy, 2));
}

let dist = calculateDistance(0, 0, 3, 4);
print("Distance: " @ dist);  // 5
```

## fs Module

File system operations.

> **Important:** Most `fs` operations are **asynchronous**. The call returns an
> IO handle. Write `await (...)`, or the postfix `.await`, to wait for it. Do not
> read `.value`; a handle has no such member and you get `null`.
>
> Most of them then give a `Result`, so call `.unwrap()` to get the value. The
> two exceptions are `fs.file_exists` and `fs.exists`, which give a plain
> boolean. `fs.is_directory` and `fs.mkdir_all` are synchronous and return a
> boolean directly.

| Call | Waiting gives |
|---|---|
| `fs.read_file(path)` | `Result<string, error>` |
| `fs.write_file(path, text)` | `Result` |
| `fs.append_file(path, text)` | `Result` |
| `fs.delete_file(path)` | `Result` |
| `fs.list_dir(path)` | `Result<array, error>` |
| `fs.file_exists(path)` / `fs.exists(path)` | `bool` |
| `fs.is_directory(path)` | `bool`, synchronous |
| `fs.mkdir_all(path)` | `bool`, synchronous |

### fs.read_file

Read entire file contents.

```nari
fs.write_file("data.txt", "hello\n").await.unwrap();

let content = (await fs.read_file("data.txt")).unwrap();
print(content);
```

**Parameters:** `path` - File path
**Returns:** Handle resolving to `Result<string, error>`

### fs.write_file

Write content to a file (overwrites existing).

```nari
fs.write_file("output.txt", "Hello, World!").await.unwrap();
```

**Parameters:**
- `path` - File path
- `content` - Content to write

**Returns:** Handle resolving to a `Result`

### fs.append_file

Append content to a file.

```nari
fs.append_file("log.txt", "New log entry\n").await.unwrap();
```

**Returns:** Handle resolving to a `Result`

### fs.file_exists / fs.exists

Check if a file exists. `fs.exists` is an alias of `fs.file_exists`. Waiting
gives a plain boolean, not a `Result`.

```nari
if (await fs.file_exists("config.json")) {
    let config = (await fs.read_file("config.json")).unwrap();
    print("Config loaded");
}
```

**Returns:** Handle resolving to a boolean

### fs.is_directory

Check if a path is a directory. **Synchronous** (returns a boolean directly).

```nari
if (fs.is_directory("/home/user/")) {
    print("It's a directory");
}
```

**Returns:** Boolean

### fs.mkdir_all

Recursively create a directory (and any missing parents). **Synchronous.**

```nari
fs.mkdir_all("/tmp/a/b/c");
```

**Returns:** Boolean (success)

### fs.delete_file

Delete a file.

```nari
fs.delete_file("temp.txt").await.unwrap();
```

**Returns:** Handle resolving to a `Result`

### fs.list_dir

List directory contents.

```nari
let files = (await fs.list_dir(".")).unwrap();
for (file in files) {
    print(file);
}
```

**Parameters:** `path` - Directory path
**Returns:** Handle resolving to `Result<array, error>`

## Standard Input

There is no `io` global. Standard input lives on `process.stdin`, and file
operations live on `fs`.

### process.stdin.read

Read everything until end of input.

```nari
let input = process.stdin.read();
print("You entered: " @ input);
```

**Returns:** String

### process.stdin.read_line

Read one line, without the newline.

```nari
print("Enter your name: ");
let name = process.stdin.read_line();
print("Hello, " @ name @ "!");
```

**Returns:** String

### Example: File Processing

```nari
func processLogFile(filename) {
    if (!(await fs.file_exists(filename))) {
        print("File not found");
        return;
    }

    // read_file gives a Result, so unwrap it before you use the text
    let content = (await fs.read_file(filename)).unwrap();
    let lines = content.split("\n");

    print("Processing " @ to_string(lines.length()) @ " lines");

    for (line in lines) {
        if (line.index_of("ERROR") != -1) {
            fs.append_file("errors.log", line @ "\n").await;
        }
    }
}

processLogFile("app.log");
```

## platform Module

Information about the host platform.

- `platform.arch` - CPU architecture (e.g. `"x86_64"`).
- `platform.os` - Operating system (e.g. `"linux"`).
- `platform.endianness` - `"little"` or `"big"`.
- `platform.hostname` - The machine's hostname.
- `platform.getenv(name)` - Read an environment variable (returns `null` if unset).

```nari
print(platform.os @ "/" @ platform.arch);
let home = platform.getenv("HOME");
```

## process Module

Process-level utilities.

- `process.argc` - Number of command-line arguments.
- `process.argv` - Array of command-line arguments.
- `process.exit(code)` - Terminate the process with an exit code.
- `process.exec(command)` - Run a shell command. The child writes straight to
  this program's output. The return value is the **exit code**, not the output.
- `process.stdin.read()` / `process.stdin.read_line()` - Read standard input.

```nari
let code = process.exec("echo hello");   // prints: hello
print(code);                             // 0
```

```nari
if (process.argc < 2) {
    print("usage: script <arg>");
    process.exit(1);
}
print(process.argv[1]);
```

## http Module

HTTP client functionality.

### http.fetch

Perform an HTTP request. Accepts a URL string and an optional options object
`{ method?, headers?, body? }`.

The call returns a handle. Waiting on it gives a `Result`. Unwrap that to get the
response object, which has `status_code`, `body` and `headers`.

```nari
let result = await http.fetch("https://api.example.com/data");
if (result.is_err()) {
    print("Request failed: " @ to_string(result.unwrap_err()));
    return;
}

let response = result.unwrap();
print("Status: " @ to_string(response.status_code));
print("Body: " @ response.body);
```

`JSON.stringify` also gives a `Result`, so unwrap it before you use it as a body:

```nari
let posted = (await http.fetch("https://api.example.com/items", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
        name: "nari"
    }).unwrap()
})).unwrap();
```

**Parameters:** `url` - URL string, and an optional options object
**Returns:** Handle resolving to `Result<response, error>`, where response has:
- `status_code` - HTTP status code
- `body` - Response body as string
- `headers` - Response headers

### Example: API Client

```nari
func fetchUser(userId) {
    let url = "https://api.example.com/users/" @ to_string(userId);

    let result = await http.fetch(url);
    if (result.is_err()) {
        return Err("Request failed: " @ to_string(result.unwrap_err()));
    }

    let response = result.unwrap();
    if (response.status_code == 200) {
        return Ok(response.body);
    }
    return Err("Failed to fetch user: " @ to_string(response.status_code));
}

let userData = fetchUser(123);
if (userData.is_ok()) {
    print("User data: " @ userData.unwrap());
} else {
    print("Error: " @ userData.unwrap_err());
}
```

## net Module

TCP and UDP networking.

The modern API is non-blocking and handle-based:

**TCP client/server:**
- `net.connect(host, port)` - Connect to a TCP server. Returns a handle
  resolving to a connection `{ fd, ip, port }`.
- `net.listen(port)` - Create a TCP listener (pass `null`/`0` for an ephemeral
  port). Returns a handle resolving to `{ fd, port }`.
- `net.accept(server)` - Wait for one inbound connection. Returns a handle
  resolving to a connection object. Loop to keep accepting.
- `net.read(conn, cb)` - Read one chunk; `cb(err, data)`.
- `net.write(conn, data)` - Write data. Returns a handle; wait on it to be sure
  the write finished.
- `net.close(conn)` - Close a connection.
- `net.close_server(server)` - Close a listener.

**UDP:**
- `net.udp_socket(port)` - Bind a UDP socket (`null`/`0` for ephemeral). Returns
  a handle resolving to `{ fd, port }`.
- `net.udp_send(sock, host, port, data)` - Send a datagram. Resolves to bytes sent.
- `net.udp_recv(sock, timeout_ms)` - Receive one datagram. Resolves to
  `{ data, ip, port }`. `timeout_ms` is optional (`null`/`0` blocks until shutdown).
- `net.udp_close(sock)` - Close a UDP socket.

### net.create_server

Create a TCP server. **Legacy blocking API** - it runs an internal accept and
yield loop on the main task until shutdown is requested. Prefer `net.listen` with
`net.accept` for new code.

Read and write go through the module functions `net.read` and `net.write`. A
connection object is plain data (`{ fd, ip, port }`) and has no methods.

```nari
func handleConnection(conn) {
    net.read(conn, func(err, data) {
        if (err != null) {
            print("Read error: " @ to_string(err));
            net.close(conn);
            return;
        }

        print("Received: " @ data);

        net.write(conn, "Echo: " @ data).await;
        net.close(conn);
    });
}

net.create_server(8080, handleConnection);
print("Server listening on port 8080");
```

**Parameters:**
- `port` - Port number to listen on
- `callback` - Function called for each connection

**Connection object:** plain data with `fd`, `ip` and `port`. It has no methods.
Use the module functions `net.read(conn, cb)`, `net.write(conn, data)` and
`net.close(conn)`.

### Example: Echo Server

```nari
func on_connection(conn) {
    net.read(conn, func(err, data) {
        if (err != null) {
            print("Read error: " @ to_string(err));
            net.close(conn);
            return;
        }

        net.write(conn, "Echo: " @ data).await;
        net.close(conn);
    });
}

print("Echo server running on port 3000");
net.create_server(3000, on_connection);
```

## JSON Module

Parse and serialize JSON. Both functions give a `Result`, so unwrap before use.

### JSON.parse

Parse a JSON string into `Result<value, string>`. `Ok` holds the value (object,
array, string, number, bool or null). Malformed input gives `Err` with a
message. It does not throw.

```nari
let obj = JSON.parse("{\"name\": \"Alice\", \"age\": 30}").unwrap();
print(obj.name);  // "Alice"

let bad = JSON.parse("{oops");
print(bad.is_err());       // true
print(bad.unwrap_err());   // "SyntaxError: JSON.parse failed: ..."
```

### JSON.stringify

Serialize a Nari value to `Result<string, string>`. An optional second `indent`
argument (integer) enables pretty-printing.

```nari
print(JSON.stringify({ a: 1, b: [2, 3] }).unwrap());   // {"a":1,"b":[2,3]}
print(JSON.stringify({ a: 1 }, 2).unwrap());           // 2-space indent
```

## Spawn Module

Asynchronous operation utilities. See [Asynchronous Programming](08-async.md) for details.

### Spawn.map

Create spawn handles from an array.

```nari
let urls = ["https://api1.com", "https://api2.com"];
let handles = Spawn.map(urls, func(url) {
    return http.fetch(url);
});
```

### Spawn.all

Wait for every handle and give an array of their results.

```nari
let results = Spawn.all(handles);
```

### Spawn.race

Wait for the first handle to finish, whether it succeeded or failed. The result
is `{ index, duration, handle }`, where `index` points into the array you passed
and `duration` is in milliseconds.

```nari
let urls = ["https://example.com", "https://www.example.com"];
let handles = Spawn.map(urls, func(url) { return http.fetch(url); });

let winner = Spawn.race(handles);
print(urls[winner.index]);
print(winner.duration);                        // milliseconds
print(winner.value.unwrap().status_code);      // resolved value
```

### Spawn.any

Wait for the first handle that succeeds. The result has `index` and `duration`.
It panics if every handle fails.

```nari
let firstSuccess = Spawn.any(handles);
print(urls[firstSuccess.index]);
```

## yield Function

Yield control to the event loop.

```nari
func longTask() {
    for (let i = 0; i < 1000000; i++) {
        if (i % 10000 == 0) {
            yield();  // Allow other tasks to run
        }
        // Do work...
    }
}
```

## Complete Example

```nari
// Math operations
print("sqrt(16) = " @ to_string(math.sqrt(16)));
print("5^2 = " @ to_string(math.pow(5, 2)));

// File I/O. fs calls are async, and most of them give a Result.
if (await fs.file_exists("data.txt")) {
    let data = (await fs.read_file("data.txt")).unwrap();
    let lines = data.split("\n");
    print("File has " @ to_string(lines.length()) @ " lines");
}

// HTTP request (async)
spawn {
    let result = await http.fetch("https://api.example.com/status");
    if (result.is_ok()) {
        print("API Status: " @ to_string(result.unwrap().status_code));
    }
};

// Timers keep the event loop running until they are cleared
set_interval(func() {
    print("Tick...");
}, 5000);
```

## Next Steps

- [Built-in Functions](11-builtins.md) - Core global functions
- [Asynchronous Programming](08-async.md) - Spawn and async patterns
- [Modules](09-modules.md) - Creating your own modules
