# Modules

Nari supports modular code organization through the `import` statement.

## Import Statement

### Basic Import

Import functions and variables from another file:

```nari
// math_utils.nari
func add(a, b) {
    return a + b;
}

func multiply(a, b) {
    return a * b;
}

global PI = 3.14159;
```

```nari
// main.nari
import "math_utils.nari";

print(add(5, 3));        // 8
print(multiply(4, 2));   // 8
print(PI);               // 3.14159
```

### Relative Paths

```nari
// Import from same directory
import "helpers.nari";

// Import from subdirectory
import "utils/math.nari";

// Import from parent directory  
import "../shared.nari";
```

### Absolute Paths

```nari
import "/usr/local/lib/nari/stdlib.nari";
```

## Module Organization

### Recommended Structure

```
project/
  ├── main.nari
  ├── lib/
  │   ├── utils.nari
  │   ├── helpers.nari
  │   └── constants.nari
  └── modules/
      ├── user.nari
      └── database.nari
```

### Module Pattern

Create modules with encapsulated functionality:

```nari
// user_module.nari

global UserModule = {
    create: func(name, email) {
        return {
            name: name,
            email: email,
            createdAt: 0  // timestamp
        };
    },
    
    validate: func(user) {
        if (!user.name || user.name.length() < 3) {
            return false;
        }
        if (user.email.indexOf("@") == -1) {
            return false;
        }
        return true;
    },
    
    toString: func(user) {
        return user.name @ " <" @ user.email @ ">";
    }
};
```

```nari
// main.nari
import "user_module.nari";

let user = UserModule.create("Alice", "alice@example.com");
if (UserModule.validate(user)) {
    print(UserModule.toString(user));
}
```

## Global Variables Across Modules

### Defining Globals

```nari
// config.nari
global APP_NAME = "MyApp";
global APP_VERSION = "1.0.0";
global DEBUG = true;
```

```nari
// main.nari
import "config.nari";

print(APP_NAME @ " v" @ APP_VERSION);
if (DEBUG) {
    print("Debug mode enabled");
}
```

### Module-Level Initialization

```nari
// database.nari

global connection = null;

func __init__() {
    // This runs when module is imported
    connection = { 
        host: "localhost",
        port: 5432,
        connected: false
    };
    print("Database module initialized");
}

// Call initialization
__init__();

global DB = {
    connect: func() {
        connection.connected = true;
        print("Connected to database");
    },
    
    disconnect: func() {
        connection.connected = false;
        print("Disconnected");
    }
};
```

## Import Behavior

### Import Execution

- Imported files are executed once when first imported
- Subsequent imports of the same file are ignored (import caching)
- Circular imports are prevented

```nari
// lib.nari
print("Lib module loaded");  // Printed once

func helper() {
    return 42;
}
```

```nari
// main.nari
import "lib.nari";  // Prints: "Lib module loaded"
import "lib.nari";  // No output (already loaded)

print(helper());
```

### Import Order

Imports are processed in order:

```nari
import "first.nari";   // Executes completely
import "second.nari";  // Then this executes
import "third.nari";   // Finally this

// All imports are complete here
```

## Standard Library

The standard library is automatically imported:

```nari
// No import needed for:
// - math module
// - io module
// - http module
// - net module
// - system module

print(math.sqrt(16));        // 4
print(system.version);       // stdlib version
```

See [Standard Library](12-stdlib.md) for details.

## Code Organization Examples

### Utilities Module

```nari
// utils/string_utils.nari

global StringUtils = {
    capitalize: func(str) {
        if (str.length() == 0) return str;
        let first = str.charAt(0);
        let rest = str.substr(1, str.length());
        return first.toUpper() @ rest;
    },
    
    reverse: func(str) {
        let result = "";
        for (let i = str.length() - 1; i >= 0; i--) {
            result = result @ str.charAt(i);
        }
        return result;
    },
    
    truncate: func(str, maxLen) {
        if (str.length() <= maxLen) {
            return str;
        }
        return str.substr(0, maxLen - 3) @ "...";
    }
};
```

### Constants Module

```nari
// constants.nari

global HTTP_STATUS = {
    OK: 200,
    CREATED: 201,
    BAD_REQUEST: 400,
    UNAUTHORIZED: 401,
    NOT_FOUND: 404,
    SERVER_ERROR: 500
};

global COLORS = {
    RED: "#FF0000",
    GREEN: "#00FF00",
    BLUE: "#0000FF"
};

global CONFIG = {
    MAX_RETRIES: 3,
    TIMEOUT_MS: 5000,
    PAGE_SIZE: 20
};
```

### Type Definitions Module

```nari
// types.nari

type User {
    id: number;
    username: string;
    email: string;
    role: string
}

type Post {
    id: number;
    title: string;
    content: string;
    authorId: number;
    createdAt: number
}

type Comment {
    id: number;
    postId: number;
    userId: number;
    text: string
}
```

## Module Patterns

### Factory Pattern

```nari
// factories.nari

global UserFactory = {
    create: func(username, email) {
        return {
            id: 0,  // Would be set by database
            username: username,
            email: email,
            role: "user",
            createdAt: 0
        };
    },
    
    createAdmin: func(username, email) {
        let user = UserFactory.create(username, email);
        user.role = "admin";
        return user;
    }
};
```

### Singleton Pattern

```nari
// logger.nari

global Logger = (func() {
    let instance = null;
    
    return {
        getInstance: func() {
            if (instance == null) {
                instance = {
                    logs: [],
                    log: func(message) {
                        instance.logs.push(message);
                        print("[LOG] " @ message);
                    },
                    getLogs: func() {
                        return instance.logs;
                    }
                };
            }
            return instance;
        }
    };
})();
```

## Best Practices

### 1. One Module Per File

Keep related functionality together:

```nari
// Good: user.nari contains all user-related functions
// Avoid: mixing user, post, and comment logic in one file
```

### 2. Explicit Exports via Globals

Make it clear what's exported:

```nari
// user.nari

// Private helper
func validateEmail(email) {
    return email.indexOf("@") != -1;
}

// Public API
global User = {
    create: func(name, email) {
        if (!validateEmail(email)) {
            throw "Invalid email";
        }
        return { name: name, email: email };
    }
};
```

### 3. Avoid Global Pollution

Use namespaces to group related functions:

```nari
// Good
global MathUtils = {
    add: func(a, b) { return a + b; },
    multiply: func(a, b) { return a * b; }
};

// Avoid
global add = func(a, b) { return a + b; };
global multiply = func(a, b) { return a * b; };
```

### 4. Document Dependencies

```nari
// user_controller.nari
// Dependencies: user_model.nari, validation.nari

import "user_model.nari";
import "validation.nari";

// ...
```

## Limitations

Current module system limitations:

1. **No selective imports**: Can't import specific symbols
2. **No aliases**: Can't rename imports
3. **No dynamic imports**: Must be static at top level
4. **Global namespace**: All exports go to global scope

## Next Steps

- [Error Handling](10-error-handling.md) - Using try/catch in modules
- [Standard Library](12-stdlib.md) - Built-in modules
