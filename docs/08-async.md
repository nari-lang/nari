# Asynchronous Programming

Nari provides built-in support for asynchronous operations through `spawn` blocks and related utilities.

## spawn Blocks

### Basic spawn

Execute code asynchronously and get a handle. Wait for the result with `.await`:

```nari
let handle = spawn {
    print("This runs asynchronously");
    return 42;
};

let result = handle.await;
print(result);  // 42
```

A handle recognizes exactly these members: `await`, `ready`, `failed`, `error`,
`status_code` and `duration`. Anything else, **including `.value`**, silently
gives `null` rather than raising an error. Always use `.await`.

`await X` in prefix position is sugar for `X.await`. The two forms are identical.

### HTTP Requests

`http.fetch` gives a handle. Awaiting it gives a `Result`, and unwrapping that
gives the response object `{ status_code, body, headers }`:

```nari
let handle = spawn {
    let result = http.fetch("https://example.com").await;
    return result.unwrap().status_code;
};

print("Request started...");
let status = handle.await;
print("Status code: " @ to_string(status));
```

### Multiple Concurrent Operations

```nari
let handle1 = spawn {
    return http.fetch("https://api1.com/data").await;
};

let handle2 = spawn {
    return http.fetch("https://api2.com/data").await;
};

// Both requests run concurrently
let result1 = handle1.await;   // a Result
let result2 = handle2.await;
```

## Spawn Methods

### Spawn.map

Create one handle per array element. It does not wait for anything:

```nari
let urls = [
    "https://example.com",
    "https://api.example.com",
    "https://www.example.com"
];

let handles = Spawn.map(urls, func(url) {
    return http.fetch(url);
});

print("Created " @ to_string(handles.length()) @ " handles");
```

### Spawn.all

Wait for every handle and give an array of the **resolved values**, in the same
order as the handles. For `http.fetch` each value is a `Result`:

```nari
let urls = ["https://example.com", "https://api.example.com"];
let handles = Spawn.map(urls, func(url) {
    return http.fetch(url);
});

let results = Spawn.all(handles);

for (result in results) {
    if (result.is_ok()) {
        print("Status: " @ to_string(result.unwrap().status_code));
    } else {
        print("Failed: " @ to_string(result.unwrap_err()));
    }
}
```

### Spawn.race

Wait for the first handle to finish, whether it succeeded or failed. The result
is `{ index, value, duration, handle }`. `index` points into the array you
passed, `duration` is milliseconds, and `value` is the resolved value:

```nari
let urls = [
    "https://fast-server.com",
    "https://slow-server.com",
    "https://medium-server.com"
];

let handles = Spawn.map(urls, func(url) {
    return http.fetch(url);
});

let winner = Spawn.race(handles);
print("Fastest URL index: " @ to_string(winner.index));
print("Duration: " @ to_string(winner.duration) @ "ms");
print("Status: " @ to_string(winner.value.unwrap().status_code));
```

`winner.value` is a plain object field here, not a handle member, so it does
hold the resolved value.

### Spawn.any

Wait for the first handle that succeeds. The result is
`{ index, value, duration }` with no `handle` field:

```nari
let handles = Spawn.map(urls, func(url) {
    return http.fetch(url);
});

let firstSuccess = Spawn.any(handles);
print("First successful: " @ urls[firstSuccess.index]);
```

## Timers and Scheduling

### set_timeout

Execute code after a delay. It returns `null`, so a timeout cannot be cancelled:

```nari
print("Starting...");

set_timeout(func() {
    print("This runs after 2 seconds");
}, 2000);

print("Timer set!");
```

### set_interval

Execute code repeatedly at intervals. It returns an integer id for
`clear_interval`:

```nari
let count = 0;

let intervalId = set_interval(func() {
    count = count + 1;
    print("Tick " @ count);
    
    if (count >= 5) {
        clear_interval(intervalId);
    }
}, 1000);

print("Interval started!");
```

### clear_interval

Stop a repeating interval:

```nari
global timerId;

timerId = set_interval(func() {
    print("Running...");
}, 1000);

// Stop after 5 seconds
set_timeout(func() {
    clear_interval(timerId);
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

set_timeout(func() {
    print("3. Timer callback");
}, 0);

print("2. After set_timeout");

// Output:
// 1. Start
// 2. After set_timeout
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

`http.fetch` returns a handle. Await it for a `Result`, then unwrap for the
response object `{ status_code, body, headers }`:

```nari
let result = http.fetch("https://api.example.com/data").await;
if (result.is_err()) {
    print("Request failed: " @ to_string(result.unwrap_err()));
} else {
    let response = result.unwrap();
    print("Status: " @ to_string(response.status_code));
    print("Body: " @ response.body);
}
```

### HTTP POST/Custom Requests

```nari
let result = http.fetch("https://api.example.com/users", {
    method: "POST",
    headers: {
        "Content-Type": "application/json"
    },
    body: JSON.stringify({
        name: "Alice",
        age: 30
    }).unwrap()
}).await;

print("Status: " @ to_string(result.unwrap().status_code));
```

`JSON.stringify` gives a `Result`, so unwrap it before passing it as the body.

## Network Servers

Read, write and close go through the module functions `net.read`, `net.write`
and `net.close`. A connection is plain data (`{ fd, ip, port }`) and has no
methods. `net.write(conn, data)` takes two arguments and returns a handle.

### TCP Server

```nari
func on_connection(conn) {
    net.read(conn, func(err, data) {
        if (err != null) {
            print("Read error: " @ to_string(err));
            net.close(conn);
            return;
        }

        print("Received: " @ data);

        net.write(conn, "Hello from server!").await;
        net.close(conn);
    });
}

net.create_server(8080, on_connection);
print("Server listening on port 8080");
```

### HTTP Server Example

```nari
func handleRequest(conn) {
    net.read(conn, func(err, request) {
        if (err != null) {
            net.close(conn);
            return;
        }

        let response = "HTTP/1.1 200 OK\r\n";
        response = response @ "Content-Type: text/html\r\n";
        response = response @ "Connection: close\r\n\r\n";
        response = response @ "<h1>Hello from Nari!</h1>";

        net.write(conn, response).await;
        net.close(conn);
    });
}

net.create_server(8080, handleRequest);
print("HTTP server running on port 8080");
```

`net.create_server` runs its own accept loop, so you do not need a keep-alive
timer to hold the event loop open.

## Async Patterns

### Promise-like Pattern

```nari
func fetchData(url) {
    return spawn {
        return http.fetch(url).await;
    };
}

let dataHandle = fetchData("https://api.example.com/data");
let result = dataHandle.await;          // the Result from http.fetch
print("Got data: " @ result.unwrap().body);
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
    let result = http.fetch("https://invalid-url.com").await;   // a Result
    if (result.is_err()) {
        print("Request failed: " @ to_string(result.unwrap_err()));
        return null;
    }
    return result.unwrap();
};

let response = handle.await;
```

### Timeout Pattern

```nari
func withTimeout(handle, timeout_ms) {
    let timeoutHandle = spawn {
        set_timeout(func() {}, timeout_ms);
        return null;
    };
    
    let result = Spawn.race([handle, timeoutHandle]);

    if (result.index == 1) {
        print("Operation timed out!");
        return null;
    }

    return result.value;   // race result field, holds the resolved value
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
func riskyOperation() { return Err("something broke"); }
func handleResult(v) { print("got " @ to_string(v)); }

spawn {
    let result = riskyOperation();   // returns Result
    if (result.is_err()) {
        print("Error: " @ result.unwrap_err());
        return;
    }
    handleResult(result.unwrap());
};
```

### 3. Clean Up Resources

```nari
let intervalId = set_interval(func() {
    // Do work
}, 1000);

// Remember to clear when done
set_timeout(func() {
    clear_interval(intervalId);
}, 10000);
```

## Next Steps

- [Modules](09-modules.md) - Organizing async code into modules
- [Standard Library](12-stdlib.md) - http and net modules
- [Error Handling](10-error-handling.md) - Result/Option in async code
