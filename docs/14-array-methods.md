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
print([1, 2, 3].length());     // 3
print([].length());            // 0
print([[1], [2]].length());    // 2

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
- `start` - Starting index (inclusive). Negative values are clamped to `0`.
- `end` - Ending index (exclusive, optional).

Indices must be integers; negative `end` (counting from the end) is not
supported.

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
print([1, 2, 3].join(", "));          // "1, 2, 3"
print(["a", "b", "c"].join("-"));     // "a-b-c"
print([true, false].join(" | "));     // "true | false"
print(["single"].join(","));          // "single"
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

### for_each(callback)

Call a function for each element. Returns `null`.

```nari
let numbers = [1, 2, 3];
numbers.for_each(func(n, i) {
    print(i @ ": " @ n);
});
```

**Parameters:** `callback(element, index, array)`

## Higher-Order Functions

Arrays have first-class functional methods. Each callback receives
`(element, index, array)`.

> Note: for CPU-bound work run in parallel across spawns, see
> [`Spawn.map`](12-stdlib.md); the methods below run synchronously.

### map(callback)

Return a new array with the callback applied to each element.

```nari
let numbers = [1, 2, 3, 4, 5];
let doubled = numbers.map(func(n) { return n * 2; });
print(doubled);  // [2, 4, 6, 8, 10]
```

**Returns:** New array

### filter(callback)

Return a new array containing only elements for which the callback returns a
truthy value.

```nari
let numbers = [1, 2, 3, 4, 5];
let evens = numbers.filter(func(n) { return n % 2 == 0; });
print(evens);  // [2, 4]
```

**Returns:** New array

### reduce(callback, [initial])

Reduce the array to a single value. If `initial` is omitted, the first element
is used as the starting accumulator. Reducing an empty array with no initial
value returns `null`.

```nari
let numbers = [1, 2, 3, 4, 5];
let total = numbers.reduce(func(acc, n) { return acc + n; }, 0);
print(total);  // 15
```

**Parameters:** `callback(accumulator, element, index, array)`, optional `initial`

**Returns:** Accumulated value

### find(callback)

Return the first element for which the callback returns truthy, or `null` if
none match.

```nari
let numbers = [1, 2, 3, 4, 5];
print(numbers.find(func(n) { return n > 3; }));  // 4
```

### find_index(callback)

Return the index of the first matching element, or `-1` if none match.

```nari
let numbers = [1, 2, 3, 4, 5];
print(numbers.find_index(func(n) { return n > 3; }));  // 3
```

### every(callback)

Return `true` if the callback returns truthy for every element.

```nari
print([1, 2, 3].every(func(n) { return n > 0; }));  // true
```

### some(callback)

Return `true` if the callback returns truthy for at least one element.

```nari
print([1, 2, 3].some(func(n) { return n > 2; }));  // true
```

## Sorting and Reordering

### sort([comparator])

Sort the array **in place** and return it. Without a comparator, elements are
ordered numerically when possible, otherwise lexicographically. With a
comparator `func(a, b)`, return a negative number if `a` should come before
`b`, positive if after, and `0` if equal.

```nari
let nums = [3, 1, 2];
nums.sort();
print(nums);  // [1, 2, 3]

let desc = [1, 2, 3];
desc.sort(func(a, b) { return b - a; });
print(desc);  // [3, 2, 1]
```

**Returns:** The (now sorted) array

### reverse()

Reverse the array **in place** and return it.

```nari
let arr = [1, 2, 3];
arr.reverse();
print(arr);  // [3, 2, 1]
```

**Returns:** The (now reversed) array

## In-Place Modification

### splice(start, [deleteCount], [...items])

Remove `deleteCount` elements starting at `start`, optionally inserting new
items. Modifies the array in place and returns an array of the removed elements.

```nari
let arr = [1, 2, 3, 4, 5];
let removed = arr.splice(1, 2);
print(removed);  // [2, 3]
print(arr);      // [1, 4, 5]
```

**Returns:** Array of removed elements

### fill(value, [count])

Fill the array with `value` in place. If `count` is given, the array is resized
to `count` elements and all are set to `value`.

```nari
let arr = [1, 2, 3];
arr.fill(0);
print(arr);  // [0, 0, 0]
```

**Returns:** The array

## Searching

### includes(value)

Return `true` if the array contains a value equal to `value`. Also works on
strings (substring check).

```nari
print([1, 2, 3].includes(2));  // true
```

### index_of(value)

Return the index of the first element equal to `value`, or `-1` if not found.

```nari
print([10, 20, 30].index_of(20));  // 1
```

### flat([depth])

Flatten nested arrays. Default depth is 1.

```nari
let nested = [1, [2, 3], [4, [5, 6]]];

print(nested.flat());      // [1, 2, 3, 4, [5, 6]]
print(nested.flat(2));     // [1, 2, 3, 4, 5, 6]

// Depth 0 = no flattening
print(nested.flat(0));     // [1, [2, 3], [4, [5, 6]]]

// Removes empty sub-arrays
let sparse = [1, [], 2, [], 3];
print(sparse.flat());      // [1, 2, 3]
```

**Parameters:** `depth` (optional) - How deep to flatten (default: 1)

**Returns:** New flattened array

### flat_map(callback)

Map each element then flatten the result by one level. Equivalent to `.map(fn).flat()` but more efficient.

```nari
let arr = [1, 2, 3];

// Each element produces multiple values
let result = arr.flat_map(func(x) { return [x, x * 2]; });
print(result);  // [1, 2, 2, 4, 3, 6]

// Non-array returns are kept as-is
let doubled = arr.flat_map(func(x) { return x * 10; });
print(doubled);  // [10, 20, 30]

// Filter and map in one step
let result2 = arr.flat_map(func(x) {
  if (x % 2 == 0) { return []; }  // filter out evens
  return [x * 10];
});
print(result2);  // [10, 30]
```

**Parameters:** `callback(element, index, array)` - Function called for each element

**Returns:** New flattened array

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
// Use the built-in flat() method:
let nested = [1, [2, [3, 4]], 5];
print(nested.flat(100));  // [1, 2, 3, 4, 5]
```

### Chunk Array

```nari
func chunk(arr, size) {
    let result = [];
    for (let i = 0; i < arr.length(); i = i + size) {
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
func find_max(arr) {
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
print(find_max(numbers));  // 9
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
        let in_arr2 = false;
        for (other in arr2) {
            if (other == item) {
                in_arr2 = true;
            }
        }
        let in_result = false;
        for (existing in result) {
            if (existing == item) {
                in_result = true;
            }
        }
        if (in_arr2 && !in_result) {
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
