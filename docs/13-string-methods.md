# String Methods Reference

Complete reference for string manipulation functions.

## Extraction and Slicing

### substr(start, [length])

Extract a substring.

```nari
"Hello, World!".substr(0, 5);    // "Hello"
"Hello, World!".substr(7);       // "World!" (to end)
"Hello, World!".substr(7, 5);    // "World"
```

**Parameters:**
- `start` - Starting index (0-based)
- `length` - Number of characters (optional, defaults to end)

**Returns:** String

### char_at(index)

Get character at index.

```nari
"Hello".char_at(0);   // "H"
"Hello".char_at(4);   // "o"
"Hello".char_at(10);  // "" (out of bounds)
```

**Parameters:**
- `index` - Character position (0-based)

**Returns:** Single character string or empty string

### at(index)

Get character at index, with support for negative indices.

```nari
"hello".at(0);       // "h"
"hello".at(-1);      // "o" (last character)
"hello".at(-2);      // "l"
"hello".at(10);      // "" (out of bounds)
```

**Parameters:**
- `index` - Character position (negative counts from end)

**Returns:** Single character string or empty string

### to_char_array()

Split a string into an array of individual characters.

```nari
"abc".to_char_array();            // ["a", "b", "c"]
"".to_char_array();               // []
"hello".to_char_array().reverse().join("");  // "olleh"
```

**Parameters:** None

**Returns:** Array of single-character strings

> Note: strings do not have a `slice` method. Use `substr(start, [length])`
> for substring extraction (`slice` is an array-only method).

## Searching

### index_of(searchString)

Find first occurrence of substring.

```nari
"Hello, World!".index_of("o");     // 4
"Hello, World!".index_of("World"); // 7
"Hello, World!".index_of("xyz");   // -1 (not found)

// Case sensitive
"Hello".index_of("hello");         // -1
```

**Parameters:**
- `searchString` - Substring to find

**Returns:** Index of first match, or -1 if not found

### last_index_of(searchString, [fromIndex])

Find last occurrence of substring, optionally searching backward from
`fromIndex`.

```nari
"Hello, World!".last_index_of("o");  // 8
"abcabc".last_index_of("abc");       // 3
"abcabc".last_index_of("abc", 2);    // 0 (searches backward from index 2)
"abcabc".last_index_of("xyz");       // -1
```

**Parameters:**
- `searchString` - Substring to find
- `fromIndex` - Optional index to search backward from

**Returns:** Index of last match, or -1 if not found

### includes(searchString)

Check whether the string contains a substring.

```nari
"hello world".includes("world");  // true
"hello".includes("xyz");          // false
```

**Parameters:**
- `searchString` - Substring to look for

**Returns:** Boolean

### starts_with(prefix)

Check if string starts with prefix.

```nari
"Hello, World!".starts_with("Hello");  // true
"Hello, World!".starts_with("World");  // false
"".starts_with("");                    // true
```

**Parameters:**
- `prefix` - Prefix to match

**Returns:** Boolean

### ends_with(suffix)

Check if string ends with suffix.

```nari
"Hello, World!".ends_with("World!");   // true
"Hello, World!".ends_with("Hello");    // false
"test.txt".ends_with(".txt");          // true
```

**Parameters:**
- `suffix` - Suffix to match

**Returns:** Boolean

## Modification

### replace(searchString, replaceString)

Replace first occurrence.

```nari
"Hello, World!".replace("World", "Nari");  // "Hello, Nari!"
"abc abc abc".replace("abc", "xyz");       // "xyz abc abc"
"test".replace("xyz", "123");              // "test" (no match)
```

**Parameters:**
- `searchString` - Text to find
- `replaceString` - Replacement text

**Returns:** New string

### replace_all(searchString, replaceString)

Replace all occurrences.

```nari
"abc abc abc".replace_all("abc", "xyz");    // "xyz xyz xyz"
"Hello Hello".replace_all("Hello", "Hi");   // "Hi Hi"
"test".replace_all("t", "T");               // "TesT"
```

**Parameters:**
- `searchString` - Text to find (all occurrences)
- `replaceString` - Replacement text

**Returns:** New string

### trim()

Remove leading and trailing whitespace.

```nari
"  hello  ".trim();                // "hello"
"\n\ttext\t\n".trim();             // "text"
"  spaced  words  ".trim();        // "spaced  words"
```

**Parameters:** None

**Returns:** Trimmed string

### trim_start()

Remove leading whitespace only.

```nari
"  hello  ".trim_start();           // "hello  "
"\t\nhello".trim_start();           // "hello"
```

**Parameters:** None

**Returns:** String with leading whitespace removed

### trim_end()

Remove trailing whitespace only.

```nari
"  hello  ".trim_end();             // "  hello"
"hello\t\n".trim_end();             // "hello"
```

**Parameters:** None

**Returns:** String with trailing whitespace removed

### pad_start(targetLength, [padString])

Pad the beginning of a string to reach a target length.

```nari
"5".pad_start(3, "0");        // "005"
"hi".pad_start(5);            // "   hi" (default pad is space)
"hello".pad_start(3);         // "hello" (no change, already long enough)
"x".pad_start(7, "ab");       // "abababx"
```

**Parameters:**
- `targetLength` - Desired minimum length of the result
- `padString` - String to pad with (optional, defaults to `" "`)

**Returns:** Padded string

### pad_end(targetLength, [padString])

Pad the end of a string to reach a target length.

```nari
"hi".pad_end(5);              // "hi   " (default pad is space)
"hi".pad_end(8, "!-");        // "hi!-!-!-"
"hello".pad_end(3);           // "hello" (no change, already long enough)
"x".pad_end(5, ".");          // "x...."
```

**Parameters:**
- `targetLength` - Desired minimum length of the result
- `padString` - String to pad with (optional, defaults to `" "`)

**Returns:** Padded string

### repeat(count)

Repeat a string a given number of times.

```nari
"abc".repeat(3);             // "abcabcabc"
"-".repeat(10);              // "----------"
"hello".repeat(0);           // ""
"xy".repeat(1);              // "xy"
```

**Parameters:**
- `count` - Number of times to repeat (must be non-negative)

**Returns:** Repeated string

## Case Conversion

### to_upper()

Convert to uppercase.

```nari
"hello".to_upper();         // "HELLO"
"Hello, World!".to_upper(); // "HELLO, WORLD!"
"123abc".to_upper();        // "123ABC"
```

**Parameters:** None

**Returns:** Uppercase string

### to_lower()

Convert to lowercase.

```nari
"HELLO".to_lower();         // "hello"
"Hello, World!".to_lower(); // "hello, world!"
"ABC123".to_lower();        // "abc123"
```

**Parameters:** None

**Returns:** Lowercase string

## Splitting and Joining

### split(delimiter)

Split string into array.

```nari
"a,b,c".split(",");              // ["a", "b", "c"]
"one two three".split(" ");      // ["one", "two", "three"]
"hello".split("");               // ["h", "e", "l", "l", "o"]
"no-delimiter".split(",");       // ["no-delimiter"]
```

**Parameters:**
- `delimiter` - Separator string

**Returns:** Array of strings

### join(separator)

Join array elements into string.

```nari
print(["a", "b", "c"].join(", "));      // "a, b, c"
print(["hello", "world"].join(" "));    // "hello world"
print([1, 2, 3].join("-"));             // "1-2-3"
print(["single"].join(","));            // "single"
```

**Parameters:**
- `separator` - String to insert between elements

**Returns:** String

## String Information

### length()

Get string length as a `number`.

```nari
"hello".length();          // 5
"".length();               // 0
"Hello, World!".length();  // 13
```

**Parameters:** None

**Returns:** Number

### char_code_at(index)

Get the character code (ASCII value) at the given index.

```nari
"A".char_code_at(0);         // 65
"hello".char_code_at(1);     // 101 (e)
"0".char_code_at(0);         // 48
```

**Parameters:**
- `index` - Character position (0-based)

**Returns:** Number (character code, or -1 if out of bounds)

### from_char_code(code)

Create a single-character string from an ASCII code. Called as a standalone function, not a method.

```nari
from_char_code(65);          // "A"
from_char_code(97);          // "a"
from_char_code(48);          // "0"
from_char_code(32);          // " "
```

**Parameters:**
- `code` - ASCII character code (0 - 127)

**Returns:** Single character string

## Concatenation

### Using @ Operator

```nari
"Hello" @ " " @ "World";          // "Hello World"
"Age: " @ 25;                     // "Age: 25"
"a" @ "b" @ "c";                  // "abc"
```

### Using += Operator

```nari
let str = "Hello";
str += " World";                  // "Hello World"
```

## String Interpolation

### Template Strings

Use backticks for interpolation:

```nari
let name = "Alice";
let age = 30;
let msg = `Name: {name}, Age: {age}`;
print(msg);  // "Name: Alice, Age: 30"

// Expressions
let x = 5;
let y = 10;
print(`Sum: {x + y}`);            // "Sum: 15"

// Function calls
print(`Upper: {"hello".to_upper()}`);  // "Upper: HELLO"
```

## Practical Examples

### Parse CSV Line

```nari
func parseCSVLine(line) {
    return line.split(",");
}

let data = "Alice,30,Boston";
let fields = parseCSVLine(data);
print(fields[0]);  // "Alice"
print(fields[1]);  // "30"
```

### Clean User Input

```nari
func cleanInput(input) {
    let cleaned = input.trim();
    cleaned = cleaned.to_lower();
    return cleaned;
}

let userInput = "  HELLO  ";
print(cleanInput(userInput));  // "hello"
```

### URL Builder

```nari
func buildURL(base, params) {
    let url = base;
    let keys = params.keys(); 
    if (keys.length() > 0) {
        url = url @ "?";
        for (let i = 0; i < keys.length(); i++) {
            if (i > 0) {
                url = url @ "&";
            }
            url = url @ keys[i] @ "=" @ params[keys[i]];
        }
    }
    
    return url;
}

let url = buildURL("https://api.example.com/search", {
    q: "nari",
    limit: "10"
});
print(url);  // "https://api.example.com/search?q=nari&limit=10"
```

### String Capitalization

```nari
func capitalize(str) {
    if (str.length() == 0) { return str }
    
    let first = str.char_at(0).to_upper();
    let rest = str.substr(1);
    return first @ rest;
}

print(capitalize("hello"));  // "Hello"
```

### Word Count

```nari
func count_words(text) {
    let trimmed = text.trim();
    if (trimmed.length() == 0) { return 0 }
    
    let words = trimmed.split(" ");
    return words.length();
}

print(count_words("Hello world"));     // 2
```

### String Truncation

```nari
func truncate(str, max_length) {
    if (str.length() <= max_length) {
        return str;
    }
    return str.substr(0, max_length - 3) @ "...";
}

print(truncate("This is a long string", 10));
// "This is..."
```

## Next Steps

- [Array Methods](14-array-methods.md) - Array manipulation
- [Built-in Functions](11-builtins.md) - All built-in functions
