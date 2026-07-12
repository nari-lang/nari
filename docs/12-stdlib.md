# Standard Library

The Nari standard library provides high-level modules for common tasks. It is automatically loaded and available without imports.

## system Module

The `system` object bundles references to the other prelude modules plus a few
extras: `version`, `print`, `math`, `fs`, `io`, `net`, `http`, `platform`,
`json`, and `exec`.

### system.version

Get the standard library version.

```nari
print(system.version);  // "0.0.3"
```

### system.print

Alias for the global `print` function.

```nari
system.print("Hello!");
```

### system.math / system.fs / system.io / system.net / system.http / system.platform / system.json

References to the corresponding prelude modules.

```nari
let result = system.math.sqrt(16);
let files = system.fs.list_dir(".");
let parsed = system.json.parse("{\"a\":1}");
```

### system.exec

Run a shell command and return its output.

```nari
let output = system.exec("echo hello");
```

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

> **Important:** Most `fs` operations are **asynchronous** and return an IO
> handle rather than the value directly. Use `await (...)` (or read
> `.value`) to obtain the resolved result. The exceptions are `fs.is_directory`
> and `fs.mkdir_all`, which are synchronous.

### fs.read_file

Read entire file contents.

```nari
let content = await fs.read_file("data.txt");
print(content);
```

**Parameters:** `path` - File path  
**Returns:** Handle resolving to the file contents (string), or an error on failure

### fs.write_file

Write content to a file (overwrites existing).

```nari
await fs.write_file("output.txt", "Hello, World!");
```

**Parameters:**
- `path` - File path
- `content` - Content to write

**Returns:** Handle (resolves when the write completes)

### fs.append_file

Append content to a file.

```nari
await fs.append_file("log.txt", "New log entry\n");
```

**Parameters:**
- `path` - File path
- `content` - Content to append

**Returns:** Handle (resolves when the append completes)

### fs.file_exists / fs.exists

Check if a file exists. `fs.exists` is an alias of `fs.file_exists`.

```nari
if ((await fs.file_exists("config.json"))) {
    let config = await fs.read_file("config.json");
    print("Config loaded");
}
```

**Parameters:** `path`  
**Returns:** Handle resolving to a boolean

### fs.is_directory

Check if a path is a directory. **Synchronous** (returns a boolean directly).

```nari
if (fs.is_directory("/home/user/")) {
    print("It's a directory");
}
```

**Parameters:** `path`  
**Returns:** Boolean

### fs.mkdir_all

Recursively create a directory (and any missing parents). **Synchronous.**

```nari
fs.mkdir_all("/tmp/a/b/c");
```

**Parameters:** `path`  
**Returns:** Boolean (success)

### fs.delete_file

Delete a file.

```nari
await fs.delete_file("temp.txt");
```

**Parameters:** `path`  
**Returns:** Handle (resolves when the delete completes)

### fs.list_dir

List directory contents.

```nari
let files = await fs.list_dir("/home/user/docs");
for (file in files) {
    print(file);
}
```

**Parameters:** `path` - Directory path  
**Returns:** Handle resolving to an array of filenames

## IO Module

IO operations for reading from standard input. Note: file operations live on
the `fs` module, not `io`.

### io.stdin.read

Read all from standard input.

```nari
let input = io.stdin.read();
print("You entered: " @ input);
```

**Returns:** String

### io.stdin.read_line

Read a line from standard input.

```nari
print("Enter your name: ");
let name = io.stdin.read_line();
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
    
    let content = await fs.read_file(filename);
    let lines = content.split("\n");
    
    print("Processing " @ lines.length() @ " lines");
    
    for (line in lines) {
        if (line.index_of("ERROR") != -1) {
            await fs.append_file("errors.log", line @ "\n");
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

Perform an HTTP GET request. Accepts either a URL string and an optional options object
`{ method?, headers?, body? }`. Returns a handle that resolves to the
response, so await it before use.

```nari
let response = await http.fetch("https://api.example.com/data");

print("Status: " @ response.status_code);
print("Body: " @ response.body);

// With an options object
let posted = await http.fetch("https://api.example.com/items", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
        name: "nari"
    });
});
```

**Parameters:** `url` - URL string or options object  
**Returns:** Handle resolving to a response object with:
- `status_code` - HTTP status code
- `body` - Response body as string

### Example: API Client

```nari
func fetchUser(userId) {
    let url = "https://api.example.com/users/" @ to_string(userId);
    
    let response = await http.fetch(url);
    
    if (response.status_code == 200) {
        return Ok(response.body);
    } else {
        return Err("Failed to fetch user: " @ response.status_code);
    }
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
- `net.write(conn, data, cb)` - Write data; `cb(err)`.
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

Create a TCP server. **Legacy blocking API** — it runs an internal accept/yield
loop on the main task until shutdown is requested. Prefer `net.listen` +
`net.accept` for new code.

```nari
func handleConnection(conn) {
    conn.read(conn, func(err, data) {
        if (err) {
            print("Read error: " @ err);
            conn.close(conn);
            return;
        }
        
        print("Received: " @ data);
        
        conn.write(conn, "Echo: " @ data, func(writeErr) {
            if (writeErr) {
                print("Write error: " @ writeErr);
            }
            conn.close(conn);
        });
    });
}

net.create_server(8080, handleConnection);
print("Server listening on port 8080");```

**Parameters:**
- `port` - Port number to listen on
- `callback` - Function called for each connection

**Connection object methods:**
- `conn.read(conn, callback)` - Read data
- `conn.write(conn, data, callback)` - Write data
- `conn.close(conn)` - Close connection

### Example: Echo Server

```nari
func on_connection(conn) {
    conn.read(conn, func(err, data) {
        if (!err) {
            conn.write(conn, "Echo: " @ data, func(writeErr) {
                conn.close(conn);
            });
        } else {
            conn.close(conn);
        }
    });
}

print("Echo server running on port 3000");
net.create_server(3000, on_connection);
```

## JSON Module

Parse and serialize JSON.

### JSON.parse

Parse a JSON string into a Nari value (object, array, string, number, bool, or
null). Throws on malformed input.

```nari
let obj = JSON.parse("{\"name\": \"Alice\", \"age\": 30}");
print(obj.name);  // "Alice"
```

### JSON.stringify

Serialize a Nari value to a JSON string. An optional second `indent` argument
(integer) enables pretty-printing.

```nari
print(JSON.stringify({ a: 1, b: [2, 3] }));       // {"a":1,"b":[2,3]}
print(JSON.stringify({ a: 1 }, 2));                // pretty-printed with 2-space indent
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

Wait for all handles to complete.

```nari
let results = Spawn.all(handles);
```

### Spawn.race

Get the first completed handle.

```nari
let winner = Spawn.race(handles);
```

### Spawn.any

Get the first successful handle.

```nari
let firstSuccess = Spawn.any(handles);
```

### Spawn.all_settled

Get all results with status. Each result is `{ index, duration, status, value? , error? }`
where `status` is `"fulfilled"` or `"rejected"`.

```nari
let settled = Spawn.all_settled(handles);
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
print("sqrt(16) = " @ math.sqrt(16));
print("5^2 = " @ math.pow(5, 2));

// File I/O (fs operations are async - await them)
if (await fs.file_exists("data.txt")) {
    let data = await fs.read_file("data.txt");
    let lines = data.split("\n");
    print("File has " @ lines.length() @ " lines");
}

// HTTP request (async)
spawn {
    let response = await http.fetch("https://api.example.com/status");
    print("API Status: " @ response.status_code);
};

// Keep event loop running
set_interval(func() {
    print("Tick...");
}, 5000);
```

## Next Steps

- [Built-in Functions](11-builtins.md) - Core global functions
- [Asynchronous Programming](08-async.md) - Spawn and async patterns
- [Modules](09-modules.md) - Creating your own modules
