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

### charAt(index)

Get character at index.

```nari
"Hello".charAt(0);   // "H"
"Hello".charAt(4);   // "o"
"Hello".charAt(10);  // "" (out of bounds)
```

**Parameters:**
- `string` - Source string
- `index` - Character position (0-based)

**Returns:** Single character string or empty string

### slice(start, [end])

Extract a section of a string (similar to substr but uses start/end indices).

Note: Use `substr` for length-based extraction.

## Searching

### indexOf(searchString)

Find first occurrence of substring.

```nari
"Hello, World!".indexOf("o");     // 4
"Hello, World!".indexOf("World"); // 7
"Hello, World!".indexOf("xyz");   // -1 (not found)

// Case sensitive
"Hello".indexOf("hello");         // -1
```

**Parameters:**
- `searchString` - Substring to find

**Returns:** Index of first match, or -1 if not found

### lastIndexOf(searchString)

Find last occurrence of substring.

```nari
"Hello, World!".lastIndexOf("o");  // 8
"abcabc".lastIndexOf("abc");       // 3
"abcabc".lastIndexOf("xyz");       // -1
```

**Parameters:**
- `searchString` - Substring to find

**Returns:** Index of last match, or -1 if not found

### startsWith(prefix)

Check if string starts with prefix.

```nari
"Hello, World!".startsWith("Hello");  // true
"Hello, World!".startsWith("World");  // false
"".startsWith("");                    // true
```

**Parameters:**
- `prefix` - Prefix to match

**Returns:** Boolean

### endsWith(string, suffix)

Check if string ends with suffix.

```nari
"Hello, World!".endsWith("World!");   // true
"Hello, World!".endsWith("Hello");    // false
"test.txt".endsWith(".txt");          // true
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

### replaceAll(searchString, replaceString)

Replace all occurrences.

```nari
"abc abc abc".replaceAll("abc", "xyz");    // "xyz xyz xyz"
"Hello Hello".replaceAll("Hello", "Hi");   // "Hi Hi"
"test".replaceAll("t", "T");               // "TesT"
```

**Parameters:**
- `searchString` - Text to find (all occurrences)
- `replaceString` - Replacement text

**Returns:** New string

### trim(string)

Remove leading and trailing whitespace.

```nari
"  hello  ".trim();                // "hello"
"\n\ttext\t\n".trim();             // "text"
"  spaced  words  ".trim();        // "spaced  words"
```

**Parameters:** None

**Returns:** Trimmed string

## Case Conversion

### toUpper()

Convert to uppercase.

```nari
"hello".toUpper();         // "HELLO"
"Hello, World!".toUpper(); // "HELLO, WORLD!"
"123abc".toUpper();        // "123ABC"
```

**Parameters:** None

**Returns:** Uppercase string

### toLower()

Convert to lowercase.

```nari
"HELLO".toLower();         // "hello"
"Hello, World!".toLower(); // "hello, world!"
"ABC123".toLower();        // "abc123"
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
["a", "b", "c"].join(", ");      // "a, b, c"
["hello", "world"].join(" ");    // "hello world"
[1, 2, 3].join("-");             // "1-2-3"
["single"].join(",");            // "single"
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

## Concatenation

### Using @ Operator

```nari
"Hello" @ " " @ "World";          // "Hello World"
"Age: " @ 25;                     // "Age: 25"
"a" @ "b" @ "c";                  // "abc"
```

### Using += Operator

> [!WARNING]
> This is deprecated, and will eventually be removed. You *should* always use @ or String Interpolation.

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
let x = 5, y = 10;
print(`Sum: {x + y}`);            // "Sum: 15"

// Function calls
print(`Upper: {"hello".toUpper()}`);  // "Upper: HELLO"
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
    cleaned = cleaned.toLower();
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
    
    let first = str.charAt(0).toUpper();
    let rest = str.substr(1);
    return first @ rest;
}

print(capitalize("hello"));  // "Hello"
```

### Word Count

```nari
func countWords(text) {
    let trimmed = text.trim();
    if (trimmed.length() == 0) { return 0 }
    
    let words = trimmed.split(" ");
    return words.length();
}

print(countWords("Hello world"));     // 2
```

### String Truncation

```nari
func truncate(str, maxLength) {
    if (str.length() <= maxLength) {
        return str;
    }
    return str.substr(0, maxLength - 3) @ "...";
}

print(truncate("This is a long string", 10));
// "This is..."
```

## Next Steps

- [Array Methods](14-array-methods.md) - Array manipulation
- [Built-in Functions](11-builtins.md) - All built-in functions
