# Classes

Nari supports object-oriented programming through a full-featured class system with public and private access control, constructors, methods, and the `this` keyword for referring to the current instance.

## Class Declaration

Classes are declared using the `class` keyword followed by the class name and a body containing fields and methods:

```nari
class Person {
    public name: string;
    private age: number;
    
    public init(n: string, a: number) {
        this.name = n;
        this.age = a;
    }
    
    public greet() -> string {
        return "Hello, I'm " @ this.name;
    }
}
```

## Fields

Fields are class member variables that hold data for each instance. Each field must have a visibility modifier (`public` or `private`) and a type annotation.

### Public Fields

Public fields can be accessed from outside the class:

```nari
class Point {
    public x: number;
    public y: number;
}

let p = new Point();
p.x = 10;
p.y = 20;
print(p.x);  // 10
```

### Private Fields

Private fields can only be accessed from within the class methods:

```nari
class BankAccount {
    private balance: number = 0;
    
    public deposit(amount: number) {
        this.balance = this.balance + amount;
    }
    
    public getBalance() -> number {
        return this.balance;
    }
}

let account = new BankAccount();
account.deposit(100);
print(account.getBalance());  // 100
// account.balance would cause a runtime error
```

### Field Default Values

Fields can have default values that will be assigned when an instance is created:

```nari
class Counter {
    public count: number = 0;
    private increment: number = 1;
}
```

## Constructors

The constructor is a special method named `init` that is called when a new instance is created using the `new` keyword.

```nari
class Person {
    public name: string;
    private age: number;
    
    public init(n: string, a: number) {
        this.name = n;
        this.age = a;
    }
}

let alice = new Person("Alice", 25);
```

> [!WARNING]
> Currently you are allowed to call the constructor after it's been initialized by calling the init() method again, but this is not recommended and will potentially be disallowed in the future.

Big TLDR:
- Constructors are named `init`
- Constructors are called automatically when using `new ClassName(args)` (obviously)
- Visibility modifiers have no effect on the constructor, anything can call it.
- If no constructor is defined, you can still create instances with `new ClassName()`, fields will default to `null` or a default value if specified.
- The number of arguments **must** match the constructor's parameter count

## Methods

Methods are functions defined within a class that operate on instance data.

### Public Methods

Public methods can be called from outside the class:

```nari
class Calculator {
    private result: number = 0;
    
    public add(n: number) -> number {
        this.result = this.result + n;
        return this.result;
    }
}

let calc = new Calculator();
print(calc.add(5));  // 5
```

### Private Methods

Private methods can only be called from within other methods of the same class:

```nari
class Person {
    public name: string;
    private age: number;
    
    private incrementAge() {
        this.age = this.age + 1;
    }
    
    public birthday() {
        this.incrementAge();
        print(this.name @ " is now " @ toString(this.age));
    }
}
```

## The `this` Keyword

Inside class methods and constructors, `this` refers to the current instance of the class:

```nari
class Rectangle {
    public width: number;
    public height: number;
    
    public init(w: number, h: number) {
        this.width = w;    // this.width refers to the field
        this.height = h;   // this.height refers to the field
    }
    
    public area() -> number {
        return this.width * this.height;
    }
}
```

## Creating Instances

Instances are created using the `new` keyword followed by the class name and constructor arguments:

```nari
let person = new Person("Alice", 30);
let point = new Point();  // No constructor arguments
```

## Type Information

The `typeof()` function returns the class name for class instances:

```nari
class Person {
    public name: string;
}

let p = new Person();
print(typeof(p));  // "Person"
```

## Access Control

Nari enforces access control at runtime:

- **Public members**: Can be accessed from anywhere
- **Private members**: Can only be accessed from within methods of the same class

Attempting to access private members from outside the class will result in a runtime error:

```nari
class Example {
    private secret: number = 42;
}

let ex = new Example();
// print(ex.secret);  // Runtime error: Cannot access private field
```

## Complete Example

```nari
class BankAccount {
    public accountNumber: string;
    private balance: number;
    private owner: string;
    
    public init(number: string, ownerName: string) {
        this.accountNumber = number;
        this.balance = 0;
        this.owner = ownerName;
    }
    
    public deposit(amount: number) {
        if (amount > 0) {
            this.balance = this.balance + amount;
            return true;
        }
        return false;
    }
    
    public withdraw(amount: number) -> bool {
        if (amount > 0 && amount <= this.balance) {
            this.balance = this.balance - amount;
            return true;
        }
        return false;
    }
    
    public getBalance() -> number {
        return this.balance;
    }
    
    public getOwner() -> string {
        return this.owner;
    }
    
    private log(message: string) {
        print("[" @ this.accountNumber @ "] " @ message);
    }
}

let account = new BankAccount("12345", "Alice");
account.deposit(1000);
account.withdraw(250);
print(account.getBalance());  // 750
print(account.getOwner());    // Alice
```