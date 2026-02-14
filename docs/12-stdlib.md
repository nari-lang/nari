# Standard Library

The Nari standard library provides high-level modules for common tasks. It is automatically loaded and available without imports.

## system Module

### system.version

Get the standard library version.

```nari
print(system.version);  // "0.0.1"
```

### system.print

Alias for the global `print` function.

```nari
system.print("Hello!");
```

### system.math

Reference to the math module.

```nari
let result = system.math.sqrt(16);
```

### system.fs

Reference to the file system module.

```nari
let content = system.fs.readFile("data.txt");
```

## math Module

Mathematical functions and utilities.

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

### fs.readFile

Read entire file contents.

```nari
let content = fs.readFile("data.txt");
print(content);
```

**Parameters:** `path` - File path  
**Returns:** String (file contents) or null on error

### fs.writeFile

Write content to a file (overwrites existing).

```nari
fs.writeFile("output.txt", "Hello, World!");
```

**Parameters:**
- `path` - File path
- `content` - Content to write

**Returns:** None

### fs.appendFile

Append content to a file.

```nari
fs.appendFile("log.txt", "New log entry\n");
```

**Parameters:**
- `path` - File path
- `content` - Content to append

**Returns:** None

### fs.fileExists

Check if a file exists.

```nari
if (fs.fileExists("config.json")) {
    let config = fs.readFile("config.json");
    print("Config loaded");
}
```

**Parameters:** `path`  
**Returns:** Boolean

### fs.isDirectory

Check if a path is a directory.

```nari
if (fs.isDirectory("/home/user/")) {
    print("It's a directory");
}
```

**Parameters:** `path`  
**Returns:** Boolean

### fs.deleteFile

Delete a file.

```nari
fs.deleteFile("temp.txt");
```

**Parameters:** `path`  
**Returns:** None

### fs.listDir

List directory contents.

```nari
let files = fs.listDir("/home/user/docs");
for (file in files) {
    print(file);
}
```

**Parameters:** `path` - Directory path  
**Returns:** Array of filenames

## IO Module
### IO operations for reading from standard input (and soon other streams).

### io.stdin.read

Read all from standard input.

```nari
let input = io.stdin.read();
print("You entered: " @ input);
```

**Returns:** String

### io.stdin.readLine

Read a line from standard input.

```nari
print("Enter your name: ");
let name = io.stdin.readLine();
print("Hello, " @ name @ "!");
```

**Returns:** String

### Example: File Processing

```nari
func processLogFile(filename) {
    if (!fs.fileExists(filename)) {
        print("File not found");
        return;
    }
    
    let content = fs.readFile(filename);
    let lines = content.split("\n");
    
    print("Processing " @ lines.length() @ " lines");
    
    for (line in lines) {
        if (line.indexOf("ERROR") != -1) {
            fs.appendFile("errors.log", line @ "\n");
        }
    }
}

processLogFile("app.log");
```

## http Module

HTTP client functionality.

### http.get

Perform an HTTP GET request.

```nari
let response = http.get("https://api.example.com/data");

print("Status: " @ response.statusCode);
print("Body: " @ response.body);
```

**Parameters:** `url` - URL to request  
**Returns:** Response object with:
- `statusCode` - HTTP status code
- `body` - Response body as string

### Example: API Client

```nari
func fetchUser(userId) {
    let url = "https://api.example.com/users/" @ toString(userId);
    
    let response = Spawn.await(http.get(url));
    
    if (response.statusCode == 200) {
        return response.body;
    } else {
        throw "Failed to fetch user: " @ response.statusCode;
    }
}

try {
    let userData = fetchUser(123);
    print("User data: " @ userData);
} catch (e) {
    print("Error: " @ e);
}
```

## net Module

Network server functionality.

### net.createServer

Create a TCP server.

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

net.createServer(8080, handleConnection);
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
func onConnection(conn) {
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
net.createServer(3000, onConnection);
```

## Spawn Module

Asynchronous operation utilities. See [Asynchronous Programming](08-async.md) for details.

### Spawn.map

Create spawn handles from an array.

```nari
let urls = ["https://api1.com", "https://api2.com"];
let handles = Spawn.map(urls, func(url) {
    return http.get(url);
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

### Spawn.allSettled

Get all results with status.

```nari
let settled = Spawn.allSettled(handles);
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

// File I/O
if (io.fileExists("data.txt")) {
    let data = io.readFile("data.txt");
    let lines = data.split("\n");
    print("File has " @ lines.length() @ " lines");
}

// HTTP request (async)
spawn {
    let response = http.get("https://api.example.com/status");
    print("API Status: " @ response.statusCode);
};

// Keep event loop running
setInterval(func() {
    print("Tick...");
}, 5000);
```

## Next Steps

- [Built-in Functions](11-builtins.md) - Core global functions
- [Asynchronous Programming](08-async.md) - Spawn and async patterns
- [Modules](09-modules.md) - Creating your own modules
