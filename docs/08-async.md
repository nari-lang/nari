# Asynchronous Programming

Nari provides built-in support for asynchronous operations through `spawn` blocks and related utilities.

## spawn Blocks

### Basic spawn

Execute code asynchronously and get a handle:

```nari
let handle = spawn {
    print("This runs asynchronously");
    return 42;
};

let result = handle.value;
print(result);  // 42
```

### HTTP Requests

```nari
let handle = spawn {
    let response = http.get("https://example.com");
    return response.statusCode;
};

print("Request started...");
let status = handle.value;
print("Status code: " @ status);
```

### Multiple Concurrent Operations

```nari
let handle1 = spawn {
    let data = http.get("https://api1.com/data");
    return data;
};

let handle2 = spawn {
    let data = http.get("https://api2.com/data");
    return data;
};

// Both requests run concurrently
let result1 = handle1.value;
let result2 = handle2.value;
```

## Spawn Methods

### Spawn.map

Create multiple spawn handles from an array:

```nari
let urls = [
    "https://example.com",
    "https://api.example.com",
    "https://www.example.com"
];

let handles = Spawn.map(urls, func(url) {
    return http.get(url);
});

print("Created " @ handles.length() @ " handles");
```

### Spawn.all

Wait for all operations to complete:

```nari
let urls = ["https://example.com", "https://api.example.com"];
let handles = Spawn.map(urls, func(url) { 
    return http.get(url); 
});

let results = Spawn.all(handles);

// results is an array of all responses
for (result in results) {
    print("Status: " @ result.statusCode);
}
```

### Spawn.race

Get the first completed operation:

```nari
let urls = [
    "https://fast-server.com",
    "https://slow-server.com",
    "https://medium-server.com"
];

let handles = Spawn.map(urls, func(url) { 
    return http.get(url); 
});

let winner = Spawn.race(handles);
print("Fastest URL index: " @ winner.index);
print("Duration: " @ winner.duration @ "ms");
print("Result: " @ winner.value.statusCode);
```

### Spawn.any

Get the first successful operation:

```nari
let handles = Spawn.map(urls, func(url) { 
    return http.get(url); 
});

let firstSuccess = Spawn.any(handles);
print("First successful: " @ urls[firstSuccess.index]);
```

### Spawn.allSettled

Get all results with their status (success/failure):

```nari
let handles = Spawn.map(urls, func(url) { 
    return http.get(url); 
});

let settled = Spawn.allSettled(handles);

for (result in settled) {
    if (result.status == "completed") {
        print("Success! Duration: " @ result.duration @ "ms");
    } else {
        print("Failed!");
    }
}
```

## Timers and Scheduling

### setTimeout

Execute code after a delay:

```nari
print("Starting...");

setTimeout(func() {
    print("This runs after 2 seconds");
}, 2000);

print("Timer set!");
```

### setInterval

Execute code repeatedly at intervals:

```nari
let count = 0;

let intervalId = setInterval(func() {
    count = count + 1;
    print("Tick " @ count);
    
    if (count >= 5) {
        clearInterval(intervalId);
    }
}, 1000);

print("Interval started!");
```

### clearInterval

Stop a repeating interval:

```nari
global timerId;

timerId = setInterval(func() {
    print("Running...");
}, 1000);

// Stop after 5 seconds
setTimeout(func() {
    clearInterval(timerId);
    print("Stopped!");
}, 5000);
```

## Event Loop

Nari uses an event loop to manage asynchronous operations:

1. Synchronous code runs first
2. Async operations (spawn, timers) are queued
3. Event loop processes pending operations
4. Program exits when no pending work remains

### Event Loop Example

```nari
print("1. Start");

setTimeout(func() {
    print("3. Timer callback");
}, 0);

print("2. After setTimeout");

// Output:
// 1. Start
// 2. After setTimeout
// 3. Timer callback
```

## Cooperative Multitasking

### yield Function

Yield control to allow other tasks to run:

```nari
func longRunningTask() {
    for (let i = 0; i < 1000000; i++) {
        if (i % 10000 == 0) {
            yield();  // Allow other tasks to run
        }
        // Do work
    }
}

spawn {
    longRunningTask();
};

spawn {
    print("Other task running");
};
```

## HTTP Requests

### HTTP GET

```nari
let response = http.get("https://api.example.com/data");
print("Status: " @ response.statusCode);
print("Body: " @ response.body);
```

### HTTP POST/Custom Requests

```nari
let response = http.request({
    method: "POST",
    url: "https://api.example.com/users",
    headers: {
        "Content-Type": "application/json"
    },
    body: "{\"name\":\"Alice\",\"age\":30}"
});

print("Status: " @ response.statusCode);
```

## Network Servers

### TCP Server

```nari
func onConnection(conn) {
    conn.read(conn, func(err, data) {
        if (err) {
            print("Read error: " @ err);
            conn.close(conn);
            return;
        }
        
        print("Received: " @ data);
        
        conn.write(conn, "Hello from server!", func(writeErr) {
            if (writeErr) {
                print("Write error: " @ writeErr);
            }
            conn.close(conn);
        });
    });
}

net.createServer(8080, onConnection);
print("Server listening on port 8080");

// Keep event loop running
setInterval(func() {}, 1000);
```

### HTTP Server Example

```nari
func handleRequest(conn) {
    conn.read(conn, func(err, request) {
        if (err) {
            conn.close(conn);
            return;
        }
        
        let response = "HTTP/1.1 200 OK\r\n";
        response = response @ "Content-Type: text/html\r\n";
        response = response @ "Connection: close\r\n\r\n";
        response = response @ "<h1>Hello from Nari!</h1>";
        
        conn.write(conn, response, func(writeErr) {
            conn.close(conn);
        });
    });
}

net.createServer(8080, handleRequest);
print("HTTP server running on port 8080");
setInterval(func() {}, 1000);
```

## Async Patterns

### Promise-like Pattern

```nari
func fetchData(url) {
    return spawn {
        return http.get(url);
    };
}

let dataHandle = fetchData("https://api.example.com/data");
let result = dataHandle.value;
print("Got data: " @ result.body);
```

### Parallel Processing

```nari
func processItems(items) {
    let handles = Spawn.map(items, func(item) {
        // Process each item concurrently
        return processItem(item);
    });
    
    return Spawn.all(handles);
}

func processItem(item) {
    // Expensive operation
    return item * 2;
}
```

### Error Handling with Async

```nari
let handle = spawn {
    try {
        let response = http.get("https://invalid-url.com");
        return response;
    } catch (e) {
        print("Request failed: " @ e);
        return null;
    }
};

let result = handle.value;
```

### Timeout Pattern

```nari
func withTimeout(handle, timeoutMs) {
    let timeoutHandle = spawn {
        setTimeout(func() {}, timeoutMs);
        return null;
    };
    
    let result = Spawn.race([handle, timeoutHandle]);
    
    if (result.index == 1) {
        print("Operation timed out!");
        return null;
    }
    
    return result.value;
}
```

## Best Practices

### 1. Don't Block the Event Loop

```nari
// Bad - blocks event loop
let total = 0;
for (let i = 0; i < 1000000; i++) {
    total = total + i;
}

// Good - yields periodically
spawn {
    let total = 0;
    for (let i = 0; i < 1000000; i++) {
        if (i % 1000 == 0) yield();
        total = total + i;
    }
};
```

### 2. Handle Errors in Async Operations

```nari
spawn {
    try {
        let result = riskyOperation();
        process(result);
    } catch (error) {
        print("Error: " @ error);
    }
};
```

### 3. Clean Up Resources

```nari
let intervalId = setInterval(func() {
    // Do work
}, 1000);

// Remember to clear when done
setTimeout(func() {
    clearInterval(intervalId);
}, 10000);
```

## Next Steps

- [Modules](09-modules.md) - Organizing async code into modules
- [Standard Library](12-stdlib.md) - http and net modules
- [Error Handling](10-error-handling.md) - try/catch in async code
