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

## Inheritance

Classes can inherit from other classes using the `extends` keyword, allowing you to create hierarchies and reuse code.

### Basic Inheritance

```nari
class Animal {
    public name: string
    public age: number
    
    init(n: string, a: number) {
        this.name = n;
        this.age = a;
    }
    
    speak() {
        print(this.name @ " makes a sound");
    }
    
    getInfo() {
        return this.name @ " is " @ this.age @ " years old";
    }
}

class Dog extends Animal {
    public breed: string
    
    init(n: string, a: number, b: string) {
        // Initialize parent fields
        this.name = n;
        this.age = a;
        // Initialize own fields
        this.breed = b;
    }
    
    // Override parent method
    speak() {
        print(this.name @ " barks: Woof!");
    }
    
    // New method specific to Dog
    getBreed() {
        return this.breed;
    }
}

let dog = new Dog("Buddy", 3, "Golden Retriever");
dog.speak();            // Buddy barks: Woof!
print(dog.getInfo());   // Buddy is 3 years old (inherited method)
print(dog.getBreed());  // Golden Retriever
```

### Method Overriding

Child classes can override parent methods:

```nari
class Shape {
    public name: string
    
    init(n: string) {
        this.name = n;
    }
    
    area() {
        return 0;
    }
    
    describe() {
        print("This is a " @ this.name);
    }
}

class Rectangle extends Shape {
    public width: number
    public height: number
    
    init(w: number, h: number) {
        this.name = "rectangle";
        this.width = w;
        this.height = h;
    }
    
    // Override the area method
    area() {
        return this.width * this.height;
    }
}

let rect = new Rectangle(5, 10);
rect.describe();        // This is a rectangle (inherited)
print(rect.area());     // 50 (overridden)
```

### Multi-level Inheritance

Classes can form inheritance chains:

```nari
class Animal {
    public name: string
    
    init(n: string) {
        this.name = n;
    }
    
    move() {
        print(this.name @ " moves");
    }
}

class Dog extends Animal {
    public breed: string
    
    init(n: string, b: string) {
        this.name = n;
        this.breed = b;
    }
    
    bark() {
        print("Woof!");
    }
}

class Puppy extends Dog {
    public age: number
    
    init(n: string, b: string, a: number) {
        this.name = n;
        this.breed = b;
        this.age = a;
    }
    
    // Override move from Animal
    move() {
        print(this.name @ " bounces playfully");
    }
}

let puppy = new Puppy("Max", "Labrador", 1);
puppy.move();    // Max bounces playfully
puppy.bark();    // Woof! (from Dog)
```

### Inheriting Private Fields

Private fields are inherited but maintain their visibility rules:

```nari
class Base {
    private secret: string
    
    init(s: string) {
        this.secret = s;
    }
    
    public revealSecret() {
        return this.secret;  // OK: accessed from within Base
    }
}

class Derived extends Base {
    init(s: string) {
        this.secret = s;  // OK: field exists in Derived
    }
    
    public tryAccess() {
        // Note: this.secret is accessible in methods,
        // but only within the class that originally defined it
        return "Cannot directly access parent's private field";
    }
}

let obj = new Derived("hidden");
print(obj.revealSecret());  // hidden (inherited method works)
```

### Current Limitations

> [!NOTE]
> - No `super` keyword to call parent constructors
> - No `super.method()` syntax to call parent method implementations
> - Child constructors must manually initialize parent fields
> - No multiple inheritance (a class can only extend one parent)

### Best Practices

**1. Initialize all fields in constructors:**
```nari
class Child extends Parent {
    init(parentField, childField) {
        // Initialize parent fields first
        this.parentField = parentField;
        // Then child fields
        this.childField = childField;
    }
}
```

**2. Use inheritance for "is-a" relationships:**
```nari
// Good: Dog is an Animal
class Dog extends Animal { }

// Good: Circle is a Shape
class Circle extends Shape { }
```

**3. Keep inheritance hierarchies shallow:**
```nari
// Prefer shallow hierarchies (1-2 levels)
class Animal { }
class Dog extends Animal { }

// Avoid deep hierarchies when possible
class A { }
class B extends A { }
class C extends B { }
class D extends C { }  // Getting too deep
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