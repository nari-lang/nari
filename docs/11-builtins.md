# Built-in Functions

Core functions available globally without any imports.

## Console Output

### print

Print values to standard output.

```nari
print("Hello, World!");
print("Multiple", "arguments", "supported");
print(42);
print([1, 2, 3]);
print({name: "Alice", age: 30});
```

**Parameters:** Any number of values  
**Returns:** None

## Type Checking

### typeof

Get the type of a value as a string.

```nari
typeof(42);          // "int"
typeof(3.14);        // "float"
typeof("text");      // "string"
typeof(true);        // "bool"
typeof([1, 2]);      // "array"
typeof({a: 1});      // "object"
typeof(func(){});    // "function"
typeof(null);        // "null"
```

**Parameters:** `value` - Any value  
**Returns:** String type name

### isNumber

Check if a value is a number.

```nari
isNumber(42);        // true
isNumber("42");      // false
isNumber(3.14);      // true
```

**Parameters:** `value`  
**Returns:** Boolean

### isString

Check if a value is a string.

```nari
isString("hello");   // true
isString(42);        // false
isString("");        // true
```

**Parameters:** `value`  
**Returns:** Boolean

### isBool

Check if a value is a boolean.

```nari
isBool(true);        // true
isBool(false);       // true
isBool(1);           // false
```

**Parameters:** `value`  
**Returns:** Boolean

### isArray

Check if a value is an array.

```nari
isArray([1, 2, 3]);  // true
isArray({});         // false
isArray("array");    // false
```

**Parameters:** `value`  
**Returns:** Boolean

### isObject

Check if a value is an object.

```nari
isObject({a: 1});    // true
isObject([1, 2]);    // false
isObject(null);      // false
```

**Parameters:** `value`  
**Returns:** Boolean

### isFunction

Check if a value is a function.

```nari
isFunction(func(){});           // true
isFunction(print);              // true
isFunction("not a function");   // false
```

**Parameters:** `value`  
**Returns:** Boolean

## Type Conversion

### toNumber

Convert a value to a number.

```nari
toNumber("42");      // 42
toNumber("3.14");    // 3.14
toNumber(true);      // 1
toNumber(false);     // 0
toNumber("abc");     // 0 (invalid conversion)
```

**Parameters:** `value`  
**Returns:** Number

### toString

Convert a value to a string.

```nari
toString(42);        // "42"
toString(3.14);      // "3.14"
toString(true);      // "true"
toString([1, 2]);    // "[1, 2]"
toString({a: 1});    // "{a: 1}"
```

**Parameters:** `value`  
**Returns:** String

### toBool

Convert a value to a boolean.

```nari
toBool(1);           // true
toBool(0);           // false
toBool("text");      // true
toBool("");          // false
toBool([]);          // false
toBool([1]);         // true
```

**Parameters:** `value`  
**Returns:** Boolean

## Array Methods

Array methods are called on the array itself using dot notation. See [Array Methods](14-array-methods.md) for full reference.

### push

Add an element to the end of an array.

```nari
let arr = [1, 2, 3];
arr.push(4);
// arr is now [1, 2, 3, 4]
```

**Parameters:** `value` - The value to add

**Returns:** None (modifies array in place)

### pop

Remove and return the last element from an array.

```nari
let arr = [1, 2, 3];
let last = arr.pop();  // 3
// arr is now [1, 2]
```

**Parameters:** None  
**Returns:** The removed element

### length

Get the length of an array, string, or object.

```nari
[1, 2, 3].length();      // 3
"hello".length();        // 5
[].length();             // 0
{a: 1, b: 2}.length();  // 2
```

**Parameters:** None  
**Returns:** Number (length)

### slice

Extract a portion of an array.

```nari
let arr = [1, 2, 3, 4, 5];
arr.slice(1, 3);     // [2, 3]
arr.slice(2);        // [3, 4, 5] (from index 2 to end)
arr.slice(0, 2);     // [1, 2]
```

**Parameters:**
- `start` - Starting index (inclusive)
- `end` - Ending index (exclusive, optional)

**Returns:** New array

### concat

Concatenate arrays.

```nari
let arr1 = [1, 2];
let arr2 = [3, 4];
arr1.concat(arr2);   // [1, 2, 3, 4]
```

**Parameters:** Array to concatenate  
**Returns:** New concatenated array

### join

Join array elements into a string.

```nari
let arr = ["a", "b", "c"];
arr.join(", ");      // "a, b, c"
arr.join("-");       // "a-b-c"
[1, 2, 3].join("");  // "123"
```

**Parameters:** `separator` - String to insert between elements

**Returns:** String

## String Methods

String methods are called on the string itself using dot notation. See [String Methods](13-string-methods.md) for full reference.

### substr

Extract a substring.

```nari
let str = "Hello, World!";
str.substr(0, 5);    // "Hello"
str.substr(7);       // "World!"
```

**Parameters:**
- `start` - Starting index
- `length` - Length to extract (optional)

**Returns:** String

### charAt

Get character at a specific index.

```nari
"Hello".charAt(0);   // "H"
"Hello".charAt(4);   // "o"
```

**Parameters:** `index`

**Returns:** String (single character)

### indexOf

Find the first occurrence of a substring.

```nari
"Hello, World!".indexOf("o");     // 4
"Hello, World!".indexOf("World"); // 7
"Hello".indexOf("x");             // -1 (not found)
```

**Parameters:** `searchString`

**Returns:** Number (index, or -1 if not found)

### lastIndexOf

Find the last occurrence of a substring.

```nari
"Hello, World!".lastIndexOf("o");  // 8
"abcabc".lastIndexOf("a");         // 3
```

**Parameters:** `searchString`

**Returns:** Number (index, or -1 if not found)

### split

Split a string into an array.

```nari
"a,b,c".split(",");           // ["a", "b", "c"]
"one two three".split(" ");   // ["one", "two", "three"]
"hello".split("");            // ["h", "e", "l", "l", "o"]
```

**Parameters:** `delimiter`

**Returns:** Array of strings

### replace

Replace the first occurrence of a substring.

```nari
"Hello, World!".replace("World", "Nari");  // "Hello, Nari!"
"abc abc".replace("abc", "xyz");           // "xyz abc"
```

**Parameters:**
- `searchString`
- `replaceString`

**Returns:** New string

### replaceAll

Replace all occurrences of a substring.

```nari
"abc abc abc".replaceAll("abc", "xyz");  // "xyz xyz xyz"
"Hello".replaceAll("l", "L");            // "HeLLo"
```

**Parameters:**
- `searchString`
- `replaceString`

**Returns:** New string

### trim

Remove whitespace from both ends.

```nari
"  hello  ".trim();    // "hello"
"\n\ttext\t\n".trim(); // "text"
```

**Parameters:** None  
**Returns:** Trimmed string

### toUpper

Convert to uppercase.

```nari
"hello".toUpper();     // "HELLO"
"Hello!".toUpper();    // "HELLO!"
```

**Parameters:** None  
**Returns:** Uppercase string

### toLower

Convert to lowercase.

```nari
"HELLO".toLower();     // "hello"
"Hello!".toLower();    // "hello!"
```

**Parameters:** None  
**Returns:** Lowercase string

### startsWith

Check if string starts with a substring.

```nari
"Hello, World!".startsWith("Hello");  // true
"Hello".startsWith("Hi");             // false
```

**Parameters:** `prefix`

**Returns:** Boolean

### endsWith

Check if string ends with a substring.

```nari
"Hello, World!".endsWith("World!");   // true
"Hello".endsWith("o");                // true
"Hello".endsWith("x");                // false
```

**Parameters:** `suffix`

**Returns:** Boolean

## Object Methods

Object methods are called on the object itself using dot notation. See [Object Methods](15-object-methods.md) for full reference.

### keys

Get an array of object keys.

```nari
let obj = {name: "Alice", age: 30, city: "Boston"};
obj.keys();  // ["name", "age", "city"]
```

**Parameters:** None  
**Returns:** Array of strings (keys)

### values

Get an array of object values.

```nari
let obj = {name: "Alice", age: 30};
obj.values();  // ["Alice", 30]
```

**Parameters:** None  
**Returns:** Array of values

### hasKey

Check if an object has a specific key.

```nari
let obj = {name: "Alice"};
obj.hasKey("name");   // true
obj.hasKey("age");    // false
```

**Parameters:** `key` - Property name (string)

**Returns:** Boolean

## Timing Functions

### setTimeout

Execute a function after a delay.

```nari
setTimeout(func() {
    print("This runs after 2 seconds");
}, 2000);
```

**Parameters:**
- `callback` - Function to execute
- `delay` - Delay in milliseconds

**Returns:** Timer ID (for potential cancellation)

### setInterval

Execute a function repeatedly at intervals.

```nari
let intervalId = setInterval(func() {
    print("This runs every second");
}, 1000);
```

**Parameters:**
- `callback` - Function to execute
- `interval` - Interval in milliseconds

**Returns:** Interval ID

### clearInterval

Stop a repeating interval.

```nari
let id = setInterval(func() { print("tick"); }, 1000);
// Later...
clearInterval(id);
```

**Parameters:** `intervalId` - ID from setInterval  
**Returns:** None

## Console Input

### readLine

Read a line from standard input.

```nari
let name = readLine();
print("Hello, " @ name @ "!");
```

**Parameters:** None  
**Returns:** String (input line)

### readAll

Read all remaining input from stdin.

```nari
let content = readAll();
print("You entered: " @ content);
```

**Parameters:** None  
**Returns:** String (all input)

## Next Steps

- [String Methods](13-string-methods.md) - Detailed string operations
- [Array Methods](14-array-methods.md) - Detailed array operations
- [Standard Library](12-stdlib.md) - Higher-level modules
