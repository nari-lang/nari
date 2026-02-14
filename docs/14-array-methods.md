# Array Methods Reference

Complete reference for array manipulation functions.

## Creating Arrays

### Literal Syntax

```nari
let empty = [];
let numbers = [1, 2, 3, 4, 5];
let mixed = [1, "two", true, null];
let nested = [[1, 2], [3, 4]];
```

### From String Split

```nari
let items = "a,b,c".split(",");  // ["a", "b", "c"]
```

## Adding Elements

### push(value)

Add element to end.

```nari
let arr = [1, 2, 3];
arr.push(4);
print(arr);  // [1, 2, 3, 4]

// Chaining pushes
arr.push(5);
arr.push(6);
print(arr);  // [1, 2, 3, 4, 5, 6]
```

**Parameters:** `value` - Element to add

**Returns:** undefined (modifies array in-place)

## Removing Elements

### pop()

Remove and return last element.

```nari
let arr = [1, 2, 3, 4];
let last = arr.pop();
print(last);  // 4
print(arr);   // [1, 2, 3]
```

**Parameters:** None  
**Returns:** Removed element

## Array Information

### length()

Get array length.

```nari
[1, 2, 3].length();     // 3
[].length();            // 0
[[1], [2]].length();    // 2

let arr = [1, 2, 3];
for (let i = 0; i < arr.length(); i++) {
    print(arr[i]);
}
```

**Parameters:** None  
**Returns:** Number (element count)

## Extraction and Slicing

### slice(start, [end])

Extract subarray without modifying original.

```nari
let arr = [1, 2, 3, 4, 5];

arr.slice(0, 2);      // [1, 2]
arr.slice(2);         // [3, 4, 5] (to end)
arr.slice(1, 4);      // [2, 3, 4]
arr.slice(0, 0);      // []

print(arr);            // [1, 2, 3, 4, 5] (unchanged)
```

**Parameters:**
- `start` - Starting index (inclusive)
- `end` - Ending index (exclusive, optional)

**Returns:** New array

## Combining Arrays

### concat(array2)

Combine two arrays.

```nari
let a = [1, 2];
let b = [3, 4];
let c = a.concat(b);
print(c);  // [1, 2, 3, 4]

// Originals unchanged
print(a);  // [1, 2]
print(b);  // [3, 4]

// Chaining
let d = c.concat([5, 6]);
print(d);  // [1, 2, 3, 4, 5, 6]
```

**Parameters:** `array2` - Array to concatenate

**Returns:** New combined array

### join(separator)

Convert array to string.

```nari
[1, 2, 3].join(", ");          // "1, 2, 3"
["a", "b", "c"].join("-");     // "a-b-c"
[true, false].join(" | ");     // "true | false"
["single"].join(",");          // "single"
```

**Parameters:** `separator` - String between elements

**Returns:** String

## Iteration

### for-in Loop

```nari
let fruits = ["apple", "banana", "cherry"];

for (fruit in fruits) {
    print(fruit);
}
// Prints: apple, banana, cherry
```

### Traditional for Loop

```nari
let numbers = [1, 2, 3, 4, 5];

for (let i = 0; i < numbers.length(); i++) {
    print(numbers[i]);
}
```

### forEach (via stdlib)

See [Standard Library](12-stdlib.md) for `Spawn.map` and other functional methods.

## Higher-Order Functions

### map, filter

Use `spawn` blocks with `Spawn.map` for parallel operations:

```nari
let numbers = [1, 2, 3, 4, 5];

// Map
let doubled = Spawn.map(numbers, func(n) {
    return n * 2;
});
print(doubled);  // [2, 4, 6, 8, 10]

// Filter (manual)
func filter(arr, predicate) {
    let result = [];
    for (let i = 0; i < arr.length(); i++) {
        if (predicate(arr[i])) {
            result.push(arr[i]);
        }
    }
    return result;
}

let evens = filter(numbers, func(n) {
    return n % 2 == 0;
});
print(evens);  // [2, 4]
```

## Practical Examples

### Stack Operations

```nari
// Stack (LIFO)
let stack = [];

func stackPush(s, value) {
    s.push(value);
}

func stackPop(s) {
    return s.pop();
}

stackPush(stack, 1);
stackPush(stack, 2);
stackPush(stack, 3);

print(stackPop(stack));  // 3
print(stackPop(stack));  // 2
```

### Queue Operations

```nari
// Queue (FIFO) using slice
let queue = [];

func enqueue(q, value) {
    q.push(value);
}

func dequeue(q) {
    let first = q[0];
    let rest = q.slice(1);
    // Copy remaining items back
    while (q.length() > 0) {
        q.pop();
    }
    for (item in rest) {
        q.push(item);
    }
    return first;
}

enqueue(queue, "first");
enqueue(queue, "second");
enqueue(queue, "third");

print(dequeue(queue));  // "first"
print(dequeue(queue));  // "second"
```

### Remove Duplicates

```nari
func unique(arr) {
    let result = [];
    for (item in arr) {
        let found = false;
        for (existing in result) {
            if (existing == item) {
                found = true;
            }
        }
        if (!found) {
            result.push(item);
        }
    }
    return result;
}

let numbers = [1, 2, 2, 3, 1, 4, 3];
print(unique(numbers));  // [1, 2, 3, 4]
```

### Flatten Nested Array

```nari
func flatten(arr) {
    let result = [];
    for (item in arr) {
        if (isArray(item)) {
            let flat = flatten(item);
            result = result.concat(flat);
        } else {
            result.push(item);
        }
    }
    return result;
}

let nested = [1, [2, [3, 4]], 5];
print(flatten(nested));  // [1, 2, 3, 4, 5]
```

### Chunk Array

```nari
func chunk(arr, size) {
    let result = [];
    for (let i = 0; i < arr.length(); i += size) {
        let end = i + size;
        if (end > arr.length()) {
            end = arr.length();
        }
        result.push(arr.slice(i, end));
    }
    return result;
}

let numbers = [1, 2, 3, 4, 5, 6, 7];
print(chunk(numbers, 3));  // [[1, 2, 3], [4, 5, 6], [7]]
```

### Find Max/Min

```nari
func findMax(arr) {
    if (arr.length() == 0) return null;
    
    let max = arr[0];
    for (let i = 1; i < arr.length(); i++) {
        if (arr[i] > max) {
            max = arr[i];
        }
    }
    return max;
}

let numbers = [3, 7, 2, 9, 1];
print(findMax(numbers));  // 9
```

### Sum Array

```nari
func sum(arr) {
    let total = 0;
    for (num in arr) {
        total += num;
    }
    return total;
}

print(sum([1, 2, 3, 4, 5]));  // 15
```

### Partition Array

```nari
func partition(arr, predicate) {
    let pass = [];
    let fail = [];
    
    for (item in arr) {
        if (predicate(item)) {
            pass.push(item);
        } else {
            fail.push(item);
        }
    }
    
    return [pass, fail];
}

let numbers = [1, 2, 3, 4, 5, 6];
let result = partition(numbers, func(n) {
    return n % 2 == 0;
});

print(result[0]);  // [2, 4, 6]
print(result[1]);   // [1, 3, 5]
```

### Array Intersection

```nari
func intersection(arr1, arr2) {
    let result = [];
    for (item in arr1) {
        let inArr2 = false;
        for (other in arr2) {
            if (other == item) {
                inArr2 = true;
            }
        }
        let inResult = false;
        for (existing in result) {
            if (existing == item) {
                inResult = true;
            }
        }
        if (inArr2 && !inResult) {
            result.push(item);
        }
    }
    return result;
}

let a = [1, 2, 3, 4];
let b = [3, 4, 5, 6];
print(intersection(a, b));  // [3, 4]
```

## Performance Notes

- `push` and `pop` are O(1)
- `concat` creates new array (O(n))
- `slice` creates new array (O(n))

## Next Steps

- [Object Methods](15-object-methods.md) - Object/map operations
- [Built-in Functions](11-builtins.md) - All built-in functions
- [Standard Library](12-stdlib.md) - Spawn.map for functional array processing
