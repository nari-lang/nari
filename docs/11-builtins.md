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

### is_number

Check if a value is a number.

```nari
is_number(42);        // true
is_number("42");      // false
is_number(3.14);      // true
```

**Parameters:** `value`  
**Returns:** Boolean

### is_string

Check if a value is a string.

```nari
is_string("hello");   // true
is_string(42);        // false
is_string("");        // true
```

**Parameters:** `value`  
**Returns:** Boolean

### is_bool

Check if a value is a boolean.

```nari
is_bool(true);        // true
is_bool(false);       // true
is_bool(1);           // false
```

**Parameters:** `value`  
**Returns:** Boolean

### is_array

Check if a value is an array.

```nari
is_array([1, 2, 3]);  // true
is_array({});         // false
is_array("array");    // false
```

**Parameters:** `value`  
**Returns:** Boolean

### is_object

Check if a value is an object.

```nari
is_object({a: 1});    // true
is_object([1, 2]);    // false
is_object(null);      // false
```

**Parameters:** `value`  
**Returns:** Boolean

### is_function

Check if a value is a function.

```nari
is_function(func(){});           // true
is_function(print);              // true
is_function("not a function");   // false
```

**Parameters:** `value`  
**Returns:** Boolean

## Type Conversion

### to_number

Convert a value to a number.

```nari
to_number("42");      // 42
to_number("3.14");    // 3.14
to_number(true);      // 1
to_number(false);     // 0
to_number("abc");     // 0 (invalid conversion)
```

**Parameters:** `value`  
**Returns:** Number

### to_string

Convert a value to a string.

```nari
to_string(42);        // "42"
to_string(3.14);      // "3.14"
to_string(true);      // "true"
to_string([1, 2]);    // "[1, 2]"
to_string({a: 1});    // "{a: 1}"
```

**Parameters:** `value`  
**Returns:** String

### to_bool

Convert a value to a boolean.

```nari
to_bool(1);           // true
to_bool(0);           // false
to_bool("text");      // true
to_bool("");          // false
to_bool([]);          // false
to_bool([1]);         // true
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

### char_at

Get character at a specific index.

```nari
"Hello".char_at(0);   // "H"
"Hello".char_at(4);   // "o"
```

**Parameters:** `index`

**Returns:** String (single character)

### index_of

Find the first occurrence of a substring.

```nari
"Hello, World!".index_of("o");     // 4
"Hello, World!".index_of("World"); // 7
"Hello".index_of("x");             // -1 (not found)
```

**Parameters:** `searchString`

**Returns:** Number (index, or -1 if not found)

### last_index_of

Find the last occurrence of a substring.

```nari
"Hello, World!".last_index_of("o");  // 8
"abcabc".last_index_of("a");         // 3
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

### replace_all

Replace all occurrences of a substring.

```nari
"abc abc abc".replace_all("abc", "xyz");  // "xyz xyz xyz"
"Hello".replace_all("l", "L");            // "HeLLo"
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

### to_upper

Convert to uppercase.

```nari
"hello".to_upper();     // "HELLO"
"Hello!".to_upper();    // "HELLO!"
```

**Parameters:** None  
**Returns:** Uppercase string

### to_lower

Convert to lowercase.

```nari
"HELLO".to_lower();     // "hello"
"Hello!".to_lower();    // "hello!"
```

**Parameters:** None  
**Returns:** Lowercase string

### starts_with

Check if string starts with a substring.

```nari
"Hello, World!".starts_with("Hello");  // true
"Hello".starts_with("Hi");             // false
```

**Parameters:** `prefix`

**Returns:** Boolean

### ends_with

Check if string ends with a substring.

```nari
"Hello, World!".ends_with("World!");   // true
"Hello".ends_with("o");                // true
"Hello".ends_with("x");                // false
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

### has_key

Check if an object has a specific key.

```nari
let obj = {name: "Alice"};
obj.has_key("name");   // true
obj.has_key("age");    // false
```

**Parameters:** `key` - Property name (string)

**Returns:** Boolean

## Timing Functions

### set_timeout

Execute a function after a delay.

```nari
set_timeout(func() {
    print("This runs after 2 seconds");
}, 2000);
```

**Parameters:**
- `callback` - Function to execute
- `delay` - Delay in milliseconds

**Returns:** Timer ID (for potential cancellation)

### set_interval

Execute a function repeatedly at intervals.

```nari
let intervalId = set_interval(func() {
    print("This runs every second");
}, 1000);
```

**Parameters:**
- `callback` - Function to execute
- `interval` - Interval in milliseconds

**Returns:** Interval ID

### clear_interval

Stop a repeating interval.

```nari
let id = set_interval(func() { print("tick"); }, 1000);
// Later...
clear_interval(id);
```

**Parameters:** `intervalId` - ID from set_interval  
**Returns:** None

## Console Input

### read_line

Read a line from standard input.

```nari
let name = read_line();
print("Hello, " @ name @ "!");
```

**Parameters:** None  
**Returns:** String (input line)

### read_all

Read all remaining input from stdin.

```nari
let content = read_all();
print("You entered: " @ content);
```

**Parameters:** None  
**Returns:** String (all input)

## Numeric and Utility Functions

### parse_int

Parse an integer from a string.

```nari
parse_int("42");       // 42
parse_int("3.9");      // 3
```

**Parameters:** `value`  
**Returns:** Number (integer)

### parse_float

Parse a floating-point number from a string.

```nari
parse_float("3.14");   // 3.14
parse_float("10");     // 10
```

**Parameters:** `value`  
**Returns:** Number

### random

Return a random float in the range `[0, 1)`.

```nari
let r = random();     // e.g. 0.4271...
```

**Parameters:** None  
**Returns:** Number

### range

Generate an array of integers. Supports `range(stop)`, `range(start, stop)`, and
`range(start, stop, step)`.

```nari
range(5);          // [0, 1, 2, 3, 4]
range(2, 6);       // [2, 3, 4, 5]
range(0, 10, 2);   // [0, 2, 4, 6, 8]
```

**Returns:** Array of numbers

### contains

Check whether an array contains a value.

```nari
contains([1, 2, 3], 2);   // true
contains([1, 2, 3], 9);   // false
```

**Parameters:** `array`, `value`  
**Returns:** Boolean

### from_char_code

Create a single-character string from a character code.

```nari
from_char_code(65);   // "A"
from_char_code(97);   // "a"
```

**Parameters:** `code`  
**Returns:** String (single character)

### time

Return the current time (seconds since the Unix epoch).

```nari
let now = time();
```

**Parameters:** None  
**Returns:** Number

## Delegates

Delegates wrap a target object together with a handler, allowing method
interception/proxying.

### Delegate

Create a delegate from a target and a handler.

```nari
let d = Delegate(target, handler);
```

**Parameters:** `target`, `handler`  
**Returns:** Delegate

### is_delegate

Check whether a value is a delegate.

```nari
is_delegate(d);   // true
```

**Parameters:** `value`  
**Returns:** Boolean

### delegate_target / delegate_handler

Retrieve the target object or handler from a delegate.

```nari
let t = delegate_target(d);
let h = delegate_handler(d);
```

**Parameters:** `delegate`  
**Returns:** The target object / handler

## Dynamic Evaluation

### eval

Parse and execute Nari source code at runtime. A bare expression evaluates to its value,
whereas statement sequences return the value of an explicit `return` (or `null` without one).

```nari
eval("1 + 2");                        // 3
eval("let x = 40; return x + 2");     // 42
eval("func helper() { return 7; }");  // defines helper for later evals
```

Evaluated code can call global functions and read globals, and functions it
defines remain visible to later `eval` calls. It cannot see the caller's local
variables (indirect-eval semantics). However functions defined by
`eval` cannot be called in compiled programs directly.
This will be supported later, it was just too difficult to implement for now.

**Parameters:** `source` (string)  
**Returns:** The value of the expression or `return`, else `null`

## Next Steps

- [String Methods](13-string-methods.md) - Detailed string operations
- [Array Methods](14-array-methods.md) - Detailed array operations
- [Standard Library](12-stdlib.md) - Higher-level modules
