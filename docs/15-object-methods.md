# Object Methods Reference

Complete reference for object/map operations and manipulation.

## Creating Objects

### Literal Syntax

```nari
let empty = {};

let person = {
    name: "Alice",
    age: 30,
    city: "Boston"
};

let nested = {
    user: {
        name: "Bob",
        email: "bob@example.com"
    },
    settings: {
        theme: "dark",
        notifications: true
    }
};

// Mixed types
let mixed = {
    id: 123,
    name: "Widget",
    active: true,
    tags: ["new", "featured"],
    metadata: null
};
```

## Accessing Properties

### Dot Notation

```nari
let obj = { name: "Alice", age: 30 };

print(obj.name);  // "Alice"
print(obj.age);   // 30
```

### Bracket Notation

```nari
let obj = { name: "Alice", age: 30 };

print(obj["name"]);  // "Alice"

// Dynamic keys
let key = "age";
print(obj[key]);     // 30

// Special characters in keys
let data = { "user-name": "alice123" };
print(data["user-name"]);  // "alice123"
```

## Modifying Objects

### Adding Properties

```nari
let obj = {};

obj.name = "Alice";
obj.age = 30;
obj["city"] = "Boston";

print(obj);  // { name: "Alice", age: 30, city: "Boston" }
```

### Updating Properties

```nari
let obj = { name: "Alice", age: 30 };

obj.age = 31;
obj["name"] = "Alicia";

print(obj);  // { name: "Alicia", age: 31 }
```

### Deleting Properties

Note: Nari doesn't have a built-in delete operator. Set to null or rebuild object:

```nari
// Set to null
let obj = { name: "Alice", age: 30, city: "Boston" };
obj.city = null;

// Rebuild without property
func deleteKey(obj, keyToDelete) {
    let newObj = {};
    let objKeys = obj.keys();
    
    for (key in objKeys) {
        if (key != keyToDelete) {
            newObj[key] = obj[key];
        }
    }
    
    return newObj;
}

let person = { name: "Alice", age: 30, city: "Boston" };
person = deleteKey(person, "city");
print(person);  // { name: "Alice", age: 30 }
```

## Object Inspection

### keys()

Get array of object keys.

```nari
let obj = { name: "Alice", age: 30, city: "Boston" };

let objKeys = obj.keys();
print(objKeys);  // ["name", "age", "city"]

// Iterate over keys
for (key in objKeys) {
    print(key @ ": " @ obj[key]);
}
// name: Alice
// age: 30
// city: Boston
```

**Parameters:** None  
**Returns:** Array of key strings

### values()

Get array of object values.

```nari
let obj = { name: "Alice", age: 30, city: "Boston" };

let objValues = obj.values();
print(objValues);  // ["Alice", 30, "Boston"]

// Sum numeric values
let scores = { math: 90, english: 85, science: 92 };
let total = 0;
for (score in scores.values()) {
    total += score;
}
print(total);  // 267
```

**Parameters:** None  
**Returns:** Array of values

### hasKey(key)

Check if object has property.

```nari
let obj = { name: "Alice", age: 30 };

obj.hasKey("name");     // true
obj.hasKey("age");      // true
obj.hasKey("city");     // false

// Safe property access
if (obj.hasKey("email")) {
    print(obj.email);
} else {
    print("No email");
}
```

**Parameters:** `key` - Property name (string)

**Returns:** Boolean

### length()

Get number of properties.

```nari
let obj = { name: "Alice", age: 30, city: "Boston" };
obj.length();  // 3
```

**Parameters:** None  
**Returns:** Number

## Iteration

### Iterate Over Keys

```nari
let person = { name: "Alice", age: 30, city: "Boston" };

for (key in person.keys()) {
    print(key @ ": " @ person[key]);
}
```

### Iterate Over Values

```nari
let scores = { math: 90, english: 85, science: 92 };

for (score in scores.values()) {
    print(score);
}
```

### Iterate Over Entries

```nari
let person = { name: "Alice", age: 30, city: "Boston" };

let personKeys = person.keys();
for (let i = 0; i < personKeys.length(); i++) {
    let key = personKeys[i];
    let value = person[key];
    print(`{key}: {value}`);
}
```

## Object Transformation

### Map Values

```nari
func mapValues(obj, transform) {
    let result = {};
    let objKeys = obj.keys();
    
    for (key in objKeys) {
        result[key] = transform(obj[key]);
    }
    
    return result;
}

let scores = { math: 90, english: 85, science: 92 };
let scaled = mapValues(scores, func(score) {
    return score / 10;
});
print(scaled);  // { math: 9, english: 8.5, science: 9.2 }
```

### Filter Object

```nari
func filterObject(obj, predicate) {
    let result = {};
    let objKeys = obj.keys();
    
    for (key in objKeys) {
        if (predicate(key, obj[key])) {
            result[key] = obj[key];
        }
    }
    
    return result;
}

let person = { name: "Alice", age: 30, city: "Boston", active: true };
let strings = filterObject(person, func(key, value) {
    return isString(value);
});
print(strings);  // { name: "Alice", city: "Boston" }
```

### Merge Objects

```nari
func merge(obj1, obj2) {
    let result = {};
    
    // Copy first object
    for (key in obj1.keys()) {
        result[key] = obj1[key];
    }
    
    // Copy second object (overwrites duplicates)
    for (key in obj2.keys()) {
        result[key] = obj2[key];
    }
    
    return result;
}

let defaults = { theme: "light", fontSize: 12, notifications: true };
let userPrefs = { theme: "dark", fontSize: 14 };

let settings = merge(defaults, userPrefs);
print(settings);
// { theme: "dark", fontSize: 14, notifications: true }
```

## Practical Examples

### Deep Clone

```nari
func deepClone(obj) {
    if (isArray(obj)) {
        let result = [];
        for (item in obj) {
            result.push(deepClone(item));
        }
        return result;
    }

    if (!isObject(obj)) {
        return obj;  // Primitive value
    }
    
    let result = {};
    for (key in obj.keys()) {
        result[key] = deepClone(obj[key]);
    }
    return result;
}

let original = {
    name: "Alice",
    scores: [90, 85, 92],
    meta: { active: true }
};

let copy = deepClone(original);
copy.scores[0] = 100;

print(original.scores[0]);  // 90 (unchanged)
print(copy.scores[0]);      // 100
```

### Object Equality

```nari
func equals(obj1, obj2) {
    if (!isObject(obj1) || !isObject(obj2)) {
        return obj1 == obj2;
    }
    
    let keys1 = obj1.keys();
    let keys2 = obj2.keys();
    
    if (keys1.length() != keys2.length()) {
        return false;
    }
    
    for (key in keys1) {
        if (!obj2.hasKey(key)) {
            return false;
        }
        if (!equals(obj1[key], obj2[key])) {
            return false;
        }
    }
    
    return true;
}

let a = { name: "Alice", age: 30 };
let b = { name: "Alice", age: 30 };
let c = { name: "Bob", age: 30 };

print(equals(a, b));  // true
print(equals(a, c));  // false
```

### Pick Properties

```nari
func pick(obj, selectedKeys) {
    let result = {};
    for (key in selectedKeys) {
        if (obj.hasKey(key)) {
            result[key] = obj[key];
        }
    }
    return result;
}

let user = {
    id: 123,
    name: "Alice",
    email: "alice@example.com",
    password: "secret",
    role: "admin"
};

let publicInfo = pick(user, ["id", "name", "role"]);
print(publicInfo);  // { id: 123, name: "Alice", role: "admin" }
```

### Omit Properties

```nari
func omit(obj, excludedKeys) {
    let result = {};
    let objKeys = obj.keys();
    
    for (key in objKeys) {
        let excluded = false;
        for (exKey in excludedKeys) {
            if (exKey == key) {
                excluded = true;
            }
        }
        if (!excluded) {
            result[key] = obj[key];
        }
    }
    
    return result;
}

let user = {
    id: 123,
    name: "Alice",
    email: "alice@example.com",
    password: "secret"
};

let safeUser = omit(user, ["password"]);
print(safeUser);  // { id: 123, name: "Alice", email: "alice@example.com" }
```

### Get Nested Property

```nari
func getProperty(obj, path) {
    let pathKeys = path.split(".");
    let current = obj;
    
    for (key in pathKeys) {
        if (!isObject(current) || !current.hasKey(key)) {
            return null;
        }
        current = current[key];
    }
    
    return current;
}

let data = {
    user: {
        profile: {
            name: "Alice",
            settings: {
                theme: "dark"
            }
        }
    }
};

print(getProperty(data, "user.profile.name"));           // "Alice"
print(getProperty(data, "user.profile.settings.theme")); // "dark"
print(getProperty(data, "user.invalid.path"));           // null
```

### Set Nested Property

```nari
func setProperty(obj, path, value) {
    let pathKeys = path.split(".");
    let current = obj;
    
    for (let i = 0; i < pathKeys.length() - 1; i++) {
        let key = pathKeys[i];
        
        if (!current.hasKey(key) || !isObject(current[key])) {
            current[key] = {};
        }
        
        current = current[key];
    }
    
    current[pathKeys[pathKeys.length() - 1]] = value;
}

let config = {};
setProperty(config, "server.port", 8080);
setProperty(config, "server.host", "localhost");
setProperty(config, "database.name", "mydb");

print(config);
// {
//   server: { port: 8080, host: "localhost" },
//   database: { name: "mydb" }
// }
```

### Group By

```nari
func groupBy(arr, keyFn) {
    let result = {};
    
    for (item in arr) {
        let key = keyFn(item);
        
        if (!result.hasKey(key)) {
            result[key] = [];
        }
        
        result[key].push(item);
    }
    
    return result;
}

let people = [
    { name: "Alice", age: 30, city: "Boston" },
    { name: "Bob", age: 25, city: "NYC" },
    { name: "Charlie", age: 30, city: "Boston" }
];

let byAge = groupBy(people, func(person) {
    return toString(person.age);
});

print(byAge);
// {
//   "25": [{ name: "Bob", age: 25, city: "NYC" }],
//   "30": [
//     { name: "Alice", age: 30, city: "Boston" },
//     { name: "Charlie", age: 30, city: "Boston" }
//   ]
// }
```

### Count Occurrences

```nari
func countBy(arr, keyFn) {
    let result = {};
    
    for (item in arr) {
        let key = keyFn(item);
        
        if (!result.hasKey(key)) {
            result[key] = 0;
        }
        
        result[key] = result[key] + 1;
    }
    
    return result;
}

let fruits = ["apple", "banana", "apple", "orange", "banana", "apple"];

let counts = countBy(fruits, func(fruit) { return fruit; });
print(counts);  // { apple: 3, banana: 2, orange: 1 }
```

### Invert Object

```nari
func invert(obj) {
    let result = {};
    
    for (key in obj.keys()) {
        let value = toString(obj[key]);
        result[value] = key;
    }
    
    return result;
}

let idToName = { "1": "Alice", "2": "Bob", "3": "Charlie" };
let nameToId = invert(idToName);

print(nameToId);  // { Alice: "1", Bob: "2", Charlie: "3" }
```

## Object as Data Store

### Simple Key-Value Store

```nari
let store = {};

func set(key, value) {
    store[key] = value;
}

func get(key) {
    if (store.hasKey(key)) {
        return store[key];
    }
    return null;
}

func remove(key) {
    store = deleteKey(store, key);
}

set("username", "alice123");
set("theme", "dark");

print(get("username"));  // "alice123"
print(get("missing"));   // null
```

### Configuration Manager

```nari
let config = {
    defaults: {
        port: 3000,
        host: "localhost",
        debug: false
    },
    user: {}
};

func getConfig(key) {
    if (config.user.hasKey(key)) {
        return config.user[key];
    }
    if (config.defaults.hasKey(key)) {
        return config.defaults[key];
    }
    return null;
}

func setConfig(key, value) {
    config.user[key] = value;
}

setConfig("port", 8080);
print(getConfig("port"));   // 8080 (user override)
print(getConfig("host"));   // "localhost" (default)
print(getConfig("debug"));  // false (default)
```

## Performance Notes

- Property access is O(1) (hash table)
- `keys()` and `values()` are O(n)
- Object iteration is O(n)

## Next Steps

- [Built-in Functions](11-builtins.md) - All built-in functions
- [Data Types](03-data-types.md) - Type system overview
- [Custom Types](07-custom-types.md) - Structured type definitions
