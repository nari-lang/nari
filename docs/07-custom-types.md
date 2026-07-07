# Custom Types

Custom types allow you to define structured data types with named fields and optional type annotations.

## Type Declarations

### Basic Type Declaration

```nari
type Person {
    name: string;
    age: number
}
```

### Type with Multiple Fields

```nari
type User {
    id: number;
    username: string;
    email: string;
    isActive: bool;
    roles: string[]
}
```

### Type with Array Fields

```nari
type ShoppingCart {
    items: string[];
    quantities: number[];
    totalPrice: number
}
```

## Using Custom Types

### Creating Instances

Custom types are implemented as objects at runtime:

```nari
type Person {
    name: string;
    age: number;
    email: string
}

let alice: Person = {
    name: "Alice",
    age: 30,
    email: "alice@example.com"
};

let bob: Person = {
    name: "Bob",
    age: 25,
    email: "bob@example.com"
};
```

### Function Parameters with Custom Types

```nari
type Point {
    x: number;
    y: number
}

func distance(p1: Point, p2: Point) -> number {
    let dx = p2.x - p1.x;
    let dy = p2.y - p1.y;
    return (dx ** 2 + dy ** 2) ** 0.5;
}

let pointA = { x: 0, y: 0 };
let pointB = { x: 3, y: 4 };
print(distance(pointA, pointB));  // 5.0
```

### Return Types

```nari
type Score {
    playerName: string;
    points: number;
    timestamp: number
}

func createScore(name: string, points: number) -> Score {
    return {
        playerName: name,
        points: points,
        timestamp: 0  // Could use a timestamp here
    };
}

let score = createScore("Alice", 1000);
print(score.playerName @ ": " @ score.points);
```

### Arrays of Custom Types

```nari
type Product {
    id: number;
    name: string;
    price: number
}

func calculateTotal(products: Product[]) -> number {
    let total = 0;
    for (product in products) {
        total = total + product.price;
    }
    return total;
}

let cart = [
    { id: 1, name: "Apple", price: 1.50 },
    { id: 2, name: "Banana", price: 0.75 },
    { id: 3, name: "Orange", price: 2.00 }
];

print(calculateTotal(cart));  // 4.25
```

## Complex Type Examples

### Nested Structures

```nari
type Address {
    street: string;
    city: string;
    zipCode: string
}

type Employee {
    id: number;
    name: string;
    department: string;
    salary: number
}

// Note: Currently custom types can't reference other custom types
// But you can use objects for nesting:

let employee = {
    id: 1,
    name: "John Doe",
    department: "Engineering",
    salary: 80000,
    address: {
        street: "123 Main St",
        city: "Boston",
        zipCode: "02101"
    }
};
```

### Business Logic with Types

```nari
type BankAccount {
    accountNumber: string;
    balance: number;
    owner: string
}

func deposit(account: BankAccount, amount: number) -> BankAccount {
    account.balance = account.balance + amount;
    return account;
}

func withdraw(account: BankAccount, amount: number) -> bool {
    if (amount <= account.balance) {
        account.balance = account.balance - amount;
        return true;
    }
    return false;
}

func getAccountInfo(account: BankAccount) -> string {
    return account.owner @ "'s account: $" @ to_string(account.balance);
}

// Usage
let myAccount: BankAccount = {
    accountNumber: "123456",
    balance: 1000,
    owner: "Alice"
};

deposit(myAccount, 500);
print(getAccountInfo(myAccount));  // Alice's account: $1500

withdraw(myAccount, 200);
print(getAccountInfo(myAccount));  // Alice's account: $1300
```

### Data Validation

```nari
type ValidationResult {
    isValid: bool;
    errors: string[]
}

type RegisterForm {
    username: string;
    email: string;
    password: string
}

func validateForm(form: RegisterForm) -> ValidationResult {
    let result = {
        isValid: true,
        errors: []
    };
    
    if (form.username.length() < 3) {
        result.errors.push("Username must be at least 3 characters");
        result.isValid = false;
    }
    
    if (form.email.index_of("@") == -1) {
        result.errors.push("Invalid email address");
        result.isValid = false;
    }
    
    if (form.password.length() < 8) {
        result.errors.push("Password must be at least 8 characters");
        result.isValid = false;
    }
    
    return result;
}

let form = {
    username: "al",
    email: "alice.example.com",
    password: "1234"
};

let validation = validateForm(form);
if (!validation.isValid) {
    for (error in validation.errors) {
        print("Error: " @ error);
    }
}
```

## Type Annotations vs Runtime Behavior

### Important Notes

1. **Documentation Only**: Type annotations are primarily for documentation and code clarity
2. **No Runtime Enforcement**: Currently, types are not enforced at runtime
3. **Object Implementation**: Custom types are implemented as regular objects

```nari
type Number {
    value: number
}

// This works even though it doesn't match the type structure
let wrong: NumberType = "not a number";  // No error at runtime
```

### Best Practices

```nari
// Use types for clear documentation
type Config {
    host: string;
    port: number;
    debug: bool
}

// Factory functions help ensure correct structure
func createConfig(host: string, port: number) -> Config {
    return {
        host: host,
        port: port,
        debug: false
    };
}

// This ensures the object has the right shape
let config = createConfig("localhost", 8080);
```

## Advanced Patterns

### Builder Pattern

```nari
type HttpRequest {
    method: string;
    url: string;
    headers: object;
    body: string
}

func createRequestBuilder() {
    let request = {
        method: "GET",
        url: "",
        headers: {},
        body: ""
    };
    
    return {
        setMethod: func(method) {
            request.method = method;
            return this;
        },
        setUrl: func(url) {
            request.url = url;
            return this;
        },
        addHeader: func(key, value) {
            request.headers[key] = value;
            return this;
        },
        setBody: func(body) {
            request.body = body;
            return this;
        },
        build: func() {
            return request;
        }
    };
}
```

### Type Guards (Manual)

```nari
type Admin {
    userId: number;
    permissions: string[]
}

type Guest {
    sessionId: string
}

func isAdmin(user) -> bool {
    return user.has_key("permissions");
}

func processUser(user) {
    if (isAdmin(user)) {
        print("Admin user with " @ user.permissions.length() @ " permissions");
    } else {
        print("Guest user");
    }
}
```

## Limitations

Current limitations of the type system:

1. **No type checking**: Types are not enforced at runtime
2. **No inheritance**: Types cannot extend other types
3. **No union types**: Cannot express "Type A or Type B"
4. **No interfaces**: No formal interface declarations

Note: Generics and enums are supported. see [Generics and Enums](16-generics-enums.md).

## Future Enhancements

Possible future improvements:

- Runtime type validation
- Type inheritance
- Union and intersection types
- Structural type checking

## Next Steps

- [Functions](06-functions.md) - Using types with functions
- [Asynchronous Programming](08-async.md) - Types in async code
- [Built-in Functions](11-builtins.md) - Type checking functions
- [Objects](15-object-methods.md) - Working with objects
