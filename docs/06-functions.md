# Functions

## Function Declarations

### Basic Function Declaration

```nari
func greet(name) {
    print("Hello, " @ name @ "!");
}

greet("Alice");  // Hello, Alice!
```

### Function with Return Value

```nari
func add(a, b) {
    return a + b;
}

let result = add(5, 3);  // 8
```

### Function with Type Annotations

```nari
func multiply(x: number, y: number) -> number {
    return x * y;
}

func formatName(first: string, last: string) -> string {
    return first @ " " @ last;
}
```

## Function Parameters

### Default Parameters

```nari
func greet(name, greeting = "Hello") {
    print(greeting @ ", " @ name @ "!");
}

greet("Alice");              // Hello, Alice!
greet("Bob", "Hi");          // Hi, Bob!
```

### Rest Parameters

Collect remaining arguments into an array:

```nari
func sum(...numbers) {
    let total = 0;
    for (num in numbers) {
        total = total + num;
    }
    return total;
}

print(sum(1, 2, 3));        // 6
print(sum(5, 10, 15, 20));  // 50
```

**With other parameters:**
```nari
func format(prefix, ...items) {
    let result = prefix;
    for (item in items) {
        result = result @ " " @ item;
    }
    return result;
}

print(format("Items:", "apple", "banana", "cherry"));
// "Items: apple banana cherry"
```

### Type Annotated Parameters

```nari
func process(data: string[], count: number = 10) -> number {
    return data.length() + count;
}

func buildUser(id: number, name: string, roles: string[]) -> User {
    return { id: id, name: name, roles: roles };
}
```

## Function Expressions

### Anonymous Functions

```nari
let multiply = func(a, b) {
    return a * b;
};

print(multiply(4, 5));  // 20
```

### Functions in Objects

```nari
let calculator = {
    add: func(a, b) { return a + b; },
    subtract: func(a, b) { return a - b; },
    multiply: func(a, b) { return a * b; }
};

print(calculator.add(10, 5));       // 15
print(calculator.multiply(3, 4));   // 12
```

### Functions in Arrays

```nari
let operations = [
    func(x) { return x * 2; },
    func(x) { return x + 10; },
    func(x) { return x ** 2; }
];

print(operations[0](5));   // 10
print(operations[1](5));   // 15
print(operations[2](5));   // 25
```

## Higher-Order Functions

Functions that take other functions as parameters or return functions:

### Functions as Arguments

```nari
func applyOperation(f, value) {
    return f(value);
}

let double = func(x) { return x * 2; };
let square = func(x) { return x * x; };

print(applyOperation(double, 5));  // 10
print(applyOperation(square, 5));  // 25
```

### Returning Functions

```nari
func makeMultiplier(factor) {
    return func(x) {
        return x * factor;
    };
}

let double = makeMultiplier(2);
let triple = makeMultiplier(3);

print(double(5));   // 10
print(triple(5));   // 15
```

### Function Composition

```nari
func compose(f, g) {
    return func(x) {
        return f(g(x));
    };
}

let addOne = func(x) { return x + 1; };
let double = func(x) { return x * 2; };

let addThenDouble = compose(double, addOne);
print(addThenDouble(5));  // (5 + 1) * 2 = 12
```

## Closures

Functions that capture variables from their enclosing scope:

### Basic Closure

```nari
func makeCounter() {
    let count = 0;
    
    return func() {
        count = count + 1;
        return count;
    };
}

let counter = makeCounter();
print(counter());  // 1
print(counter());  // 2
print(counter());  // 3
```

### Closure with Multiple Functions

```nari
func createAccount(initialBalance) {
    let balance = initialBalance;
    
    return {
        deposit: func(amount) {
            balance = balance + amount;
            return balance;
        },
        withdraw: func(amount) {
            if (amount <= balance) {
                balance = balance - amount;
                return balance;
            }
            return null;
        },
        getBalance: func() {
            return balance;
        }
    };
}

let account = createAccount(100);
print(account.getBalance());     // 100
account.deposit(50);
print(account.getBalance());     // 150
account.withdraw(30);
print(account.getBalance());     // 120
```

### Closure in Loops

```nari
func createFunctions() {
    let funcs = [];
    
    for (let i = 0; i < 3; i++) {
        let captured = i;  // Capture current value
        let f = func() { return captured; };
        funcs.push(f);
    }
    
    return funcs;
}

let functions = createFunctions();
print(functions[0]());  // 0
print(functions[1]());  // 1
print(functions[2]());  // 2
```

## Recursion

Functions calling themselves:

### Factorial

```nari
func factorial(n) {
    if (n <= 1) {
        return 1;
    }
    return n * factorial(n - 1);
}

print(factorial(5));  // 120
```

### Fibonacci

```nari
func fibonacci(n) {
    if (n <= 1) {
        return n;
    }
    return fibonacci(n - 1) + fibonacci(n - 2);
}

print(fibonacci(7));  // 13
```

### Array Processing

```nari
func sumArray(arr, index) {
    if (index >= arr.length()) {
        return 0;
    }
    return arr[index] + sumArray(arr, index + 1);
}

print(sumArray([1, 2, 3, 4, 5], 0));  // 15
```

## Immediately Invoked Function Expressions (IIFE)

```nari
let result = (func() {
    let temp = 10;
    return temp * 2;
})();

print(result);  // 20
```

## Function Scope and Variables

### Local Variables

```nari
func example() {
    let local = 10;   // Only accessible in function
    print(local);
}

example();
// print(local);  // Error: not accessible
```

### Global Variables

```nari
global shared = 100;

func modifyGlobal() {
    shared = 200;
}

print(shared);      // 100
modifyGlobal();
print(shared);      // 200
```

### Lexical Scoping

```nari
let outer = "outer";

func makeFunc() {
    let inner = "inner";
    
    return func() {
        print(outer);  // Accesses outer scope
        print(inner);  // Accesses enclosing function
    };
}

let f = makeFunc();
f();  // Prints: outer, inner
```

## Best Practices

### 1. Single Responsibility

Each function should do one thing well:

```nari
// Good
func calculateTotal(items) {
    let total = 0;
    for (item in items) {
        total = total + item.price;
    }
    return total;
}

func formatCurrency(amount) {
    return "$" @ toString(amount);
}

// Usage
let total = calculateTotal(items);
print(formatCurrency(total));
```

### 2. Descriptive Names

```nari
// Good
func calculateMonthlyPayment(principal, rate, years) {
    // ...
}

// Avoid
func calc(p, r, y) {
    // ...
}
```

### 3. Limit Parameters

If you need many parameters, consider using an object:

```nari
// Instead of many parameters
func createUser(id, name, email, age, city, country) {
    // ...
}

// Use an options object
func createUser(options) {
    return {
        id: options.id,
        name: options.name,
        email: options.email,
        age: options.age ?? 0,
        city: options.city ?? "",
        country: options.country ?? ""
    };
}

let user = createUser({
    id: 1,
    name: "Alice",
    email: "alice@example.com",
    age: 30
});
```

> [!NOTE]
> An options object is best paired with a [custom type](07-custom-types.md) for better type safety.

### 4. Pure Functions

Functions without side effects are easier to test and reason about:

```nari
// Pure function
func add(a, b) {
    return a + b;
}

// Impure function (modifies external state)
global counter = 0;
func incrementCounter() {
    counter = counter + 1;
}
```

## Next Steps

- [Custom Types](07-custom-types.md) - Using types with functions
- [Asynchronous Programming](08-async.md) - Async functions
