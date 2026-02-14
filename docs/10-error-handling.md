# Error Handling

Nari provides try/catch/finally blocks for error handling and a `throw` statement for raising errors.

## try/catch/finally

### Basic try/catch

```nari
try {
    // Code that might throw
    let result = riskyOperation();
    print(result);
} catch (error) {
    print("Error occurred: " @ error);
}
```

### With finally

The `finally` block always executes, whether an error occurred or not:

```nari
try {
    let file = openFile("data.txt");
    processFile(file);
} catch (e) {
    print("Error: " @ e);
} finally {
    print("Cleanup code runs here");
    // Close file, release resources, etc.
}
```

### Nested try/catch

```nari
try {
    let data = fetchData();
    
    try {
        let parsed = parseData(data);
        saveData(parsed);
    } catch (parseError) {
        print("Parse error: " @ parseError);
    }
} catch (fetchError) {
    print("Fetch error: " @ fetchError);
}
```

## throw Statement

### Throwing Errors

```nari
func divide(a, b) {
    if (b == 0) {
        throw "Division by zero";
    }
    return a / b;
}

try {
    let result = divide(10, 0);
} catch (e) {
    print("Error: " @ e);  // Error: Division by zero
}
```

### Throwing Custom Objects

```nari
func validateAge(age) {
    if (age < 0) {
        throw {
            type: "ValidationError",
            message: "Age cannot be negative",
            value: age
        };
    }
    if (age > 150) {
        throw {
            type: "ValidationError", 
            message: "Age seems unrealistic",
            value: age
        };
    }
    return true;
}

try {
    validateAge(-5);
} catch (error) {
    if (isObject(error)) {
        print(error.type @ ": " @ error.message);
        print("Value was: " @ error.value);
    } else {
        print("Error: " @ error);
    }
}
```

## Error Patterns

### Error Type Classification

```nari
func processRequest(request) {
    if (!request) {
        throw { type: "ValueError", message: "Request is null" };
    }
    
    if (!request.url) {
        throw { type: "ValueError", message: "Missing URL" };
    }
    
    if (request.method != "GET" && request.method != "POST") {
        throw { type: "ValueError", message: "Invalid method" };
    }
    
    // Process request...
}

try {
    processRequest({ method: "DELETE" });
} catch (e) {
    if (e.type == "ValueError") {
        print("Validation error: " @ e.message);
    } else {
        print("Unknown error: " @ e);
    }
}
```

### Result/Error Pattern

```nari
func safeDivide(a, b) {
    if (b == 0) {
        return { success: false, error: "Division by zero" };
    }
    return { success: true, value: a / b };
}

let result = safeDivide(10, 2);
if (result.success) {
    print("Result: " @ result.value);
} else {
    print("Error: " @ result.error);
}
```

### Re-throwing Errors

```nari
func outerFunction() {
    try {
        innerFunction();
    } catch (e) {
        print("Logging error: " @ e);
        throw e;  // Re-throw to caller
    }
}

func innerFunction() {
    throw "Something went wrong";
}

try {
    outerFunction();
} catch (e) {
    print("Caught in main: " @ e);
}
```

## Validation Patterns

### Input Validation

```nari
func validateUser(user) {
    let errors = [];
    
    if (!user.username || user.username.length() < 3) {
        errors.push("Username must be at least 3 characters");
    }
    
    if (!user.email || user.email.indexOf("@") == -1) {
        errors.push("Invalid email address");
    }
    
    if (errors.length() > 0) {
        throw { type: "ValidationError", errors: errors };
    }
    
    return true;
}

try {
    validateUser({ username: "ab", email: "invalid" });
} catch (e) {
    if (e.type == "ValidationError") {
        print("Validation failed:");
        for (error in e.errors) {
            print("  - " @ error);
        }
    }
}
```

### Data Parsing

```nari
func parseJSON(text) {
    // Simplified JSON parser example
    if (text.length() == 0) {
        throw "Empty JSON string";
    }
    
    if (text.charAt(0) != "{" && text.charAt(0) != "[") {
        throw "Invalid JSON format";
    }
    
    // Parse logic...
    return {};
}

try {
    let data = parseJSON("invalid");
} catch (e) {
    print("Parse error: " @ e);
}
```

## Error Handling with Async

### In spawn Blocks

```nari
let handle = spawn {
    try {
        let response = http.get("https://invalid-url.com");
        return response;
    } catch (e) {
        return { error: true, message: e };
    }
};

let result = handle.value;
if (result.error) {
    print("Request failed: " @ result.message);
}
```

### Multiple Async Operations

```nari
func fetchWithFallback(urls) {
    for (url in urls) {
        try {
            let response = http.get(url);
            return response;
        } catch (e) {
            print("Failed to fetch from " @ url @ ": " @ e);
            // Try next URL
        }
    }
    throw "All URLs failed";
}

try {
    let data = fetchWithFallback([
        "https://primary.com",
        "https://backup1.com",
        "https://backup2.com"
    ]);
    print("Success: " @ data.statusCode);
} catch (e) {
    print("Complete failure: " @ e);
}
```

## Resource Management

### Cleanup with finally

```nari
func processFile(filename) {
    let content = null;
    
    try {
        content = fs.readFile(filename);
        if (!content) {
            throw "Could not read file";
        }
        
        // Process file
        let lines = content.split("\n");
        print("Lines: " @ lines.length());
        
    } catch (e) {
        print("Error processing file: " @ e);
    } finally {
        // Cleanup always runs
        print("File processing complete");
        content = null;
    }
}
```

### Transaction Pattern

```nari
func performTransaction() {
    let committed = false;
    
    try {
        startTransaction();
        
        // Do work
        updateRecord(1);
        updateRecord(2);
        updateRecord(3);
        
        commitTransaction();
        committed = true;
        
    } catch (e) {
        print("Transaction error: " @ e);
    } finally {
        if (!committed) {
            rollbackTransaction();
        }
    }
}
```

## Defensive Programming

### Guard Clauses

```nari
func processUser(user) {
    if (!user) {
        throw "User is null";
    }
    
    if (!user.id) {
        throw "User missing ID";
    }
    
    if (!user.name) {
        throw "User missing name";
    }
    
    // Safe to process user
    return user.name @ " (" @ user.id @ ")";
}
```

### Type Checking

```nari
func calculateArea(shape) {
    if (!isObject(shape)) {
        throw "Shape must be an object";
    }
    
    if (shape.type == "circle") {
        if (!isNumber(shape.radius)) {
            throw "Circle radius must be a number";
        }
        return 3.14159 * shape.radius ** 2;
    }
    
    if (shape.type == "rectangle") {
        if (!isNumber(shape.width) || !isNumber(shape.height)) {
            throw "Rectangle dimensions must be numbers";
        }
        return shape.width * shape.height;
    }
    
    throw "Unknown shape type: " @ shape.type;
}
```

## Error Recovery Strategies

### Retry Logic

```nari
func retry(operation, maxAttempts) {
    let attempts = 0;
    let lastError = null;
    
    while (attempts < maxAttempts) {
        try {
            return operation();
        } catch (e) {
            attempts = attempts + 1;
            lastError = e;
            print("Attempt " @ attempts @ " failed: " @ e);
            
            if (attempts < maxAttempts) {
                // Wait before retrying
                setTimeout(func() {}, 1000);
            }
        }
    }
    
    throw "Max retries exceeded. Last error: " @ lastError;
}

// Usage
try {
    let result = retry(func() {
        return http.get("https://unreliable-api.com");
    }, 3);
    print("Success!");
} catch (e) {
    print("Failed after retries: " @ e);
}
```

### Fallback Values

```nari
func getConfigValue(key, fallback) {
    try {
        let config = loadConfig();
        if (config.hasKey(key)) {
            return config[key];
        }
        return fallback;
    } catch (e) {
        print("Config error: " @ e);
        return fallback;
    }
}

let timeout = getConfigValue("timeout", 5000);
```

## Best Practices

### 1. Catch Specific Errors

```nari
// Good: Handle different error types
try {
    performOperation();
} catch (e) {
    if (isObject(e) && e.type == "NetworkError") {
        handleNetworkError(e);
    } else if (isObject(e) && e.type == "ValidationError") {
        handleValidationError(e);
    } else {
        handleUnknownError(e);
    }
}
```

### 2. Don't Swallow Errors Silently

```nari
// Bad
try {
    riskyOperation();
} catch (e) {
    // Silent failure - don't do this
}

// Good
try {
    riskyOperation();
} catch (e) {
    print("Operation failed: " @ e);
    // Or log, notify user, etc.
}
```

### 3. Clean Up in finally

```nari
let resource = null;
try {
    resource = acquireResource();
    useResource(resource);
} catch (e) {
    print("Error: " @ e);
} finally {
    if (resource) {
        releaseResource(resource);
    }
}
```

### 4. Provide Context in Errors

```nari
// Good
throw {
    type: "FileError",
    message: "Could not read file",
    filename: filename,
    reason: "File not found"
};

// Less helpful
throw "Error reading file";
```

## Next Steps

- [Builtins](11-builtins.md) - Built-in error handling functions
- [Custom Types](07-custom-types.md) - Using custom types for structured errors
