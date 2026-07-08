# Nari Standard Library Reference

> Auto-generated from `src/stdlib/std/*.nari` by `tools/gen_stdlib_reference.py`.
> Do not edit by hand - re-run the generator to update.

The Nari standard library is split into two surfaces:

- **Prelude** - globals every program can use without an import (`Result`,
  `Option`, `Ok`/`Err`/`Some`/`None`, `String`, `Array`, `Object`, `fs`, `io`,
  `platform`, `process`, `math`, `net`, `http`, `JSON`, `system`, `yield`,
  `Spawn`).
- **Importable modules** - opt-in domain modules pulled in via
  `import { Name } from "std/<name>";`. They live under
  `src/stdlib/std/<name>.nari` and are embedded into the runtime by
  `tools/embed_std_modules.py`.


## Contents

- [Prelude](#prelude) _(always available)_
  - [`String`](#string)
  - [`Array`](#array)
  - [`Object`](#object)
  - [`fs`](#fs)
  - [`io`](#io)
  - [`platform`](#platform)
  - [`process`](#process)
  - [`math`](#math)
  - [`net`](#net)
  - [`http`](#http)
  - [`JSON`](#json)
  - [`system`](#system)
  - [`Spawn`](#spawn)
  - [Globals](#globals)

### Importable modules

- [Archive](#archive) - `import { ... } from "std/archive";`
- [Map / Set](#map-set) - `import { ... } from "std/collections";`
- [Date](#date) - `import { ... } from "std/date";`
- [Hex / Base64](#hex-base64) - `import { ... } from "std/encoding";`
- [Hash](#hash) - `import { ... } from "std/hash";`
- [Logger](#logger) - `import { ... } from "std/logger";`
- [Path](#path) - `import { ... } from "std/path";`
- [Random](#random) - `import { ... } from "std/random";`
- [Regex](#regex) - `import { ... } from "std/regex";`
- [URL](#url) - `import { ... } from "std/url";`
- [uuid](#uuid) - `import { ... } from "std/uuid";`

---

## Prelude

_Source: `src/stdlib/std/prelude.nari`_

Nari stdlib prelude. Always loaded; populates the implicit globals every
program can rely on without an explicit import.

Importable modules (Date, Regex, URL, Path, Logger, Random, uuid, Hash,
Archive, Hex, Base64, Map, Set) live under src/stdlib/std/<name>.nari and
must be brought in via:

    import { Foo } from "std/<name>";

### Globals

#### `Ok(value)`

Result helpers

#### `Err(error)`

_No description._

#### `Some(value)`

Option helpers

#### `None()`

_No description._

#### `yield()`

_No description._

### String

#### `capitalize(str)`

_No description._

#### `count_words(text)`

_No description._

#### `truncate(str, max_length)`

_No description._

### Array

#### `unique(arr)`

_No description._

#### `flatten(arr)`

_No description._

#### `chunk(arr, size)`

_No description._

#### `find_max(arr)`

_No description._

#### `sum(arr : number[])`

_No description._

#### `partition(arr, predicate)`

_No description._

#### `intersection(arr1, arr2)`

_No description._

### Object

#### `deep_clone(obj)`

_No description._

#### `equals(obj1, obj2)`

_No description._

#### `pick(obj, selected_keys)`

_No description._

#### `omit(obj, excluded_keys)`

_No description._

#### `group_by(arr, key_fn)`

_No description._

### fs

#### `read_file(path)`

_No description._

#### `write_file(path, content)`

_No description._

#### `append_file(path, content)`

_No description._

#### `exists(path)`

_No description._

#### `file_exists(path)`

_No description._

#### `is_directory(path)`

_No description._

#### `mkdir_all(path)`

_No description._

#### `delete_file(path)`

_No description._

#### `list_dir(path)`

_No description._

### io

#### `stdin =  { ... }`

_No description._

### platform

#### `arch = __platform_arch()`

_No description._

#### `os = __platform_os()`

_No description._

#### `endianness = __platform_endianness()`

_No description._

#### `hostname = __platform_hostname()`

_No description._

#### `getenv(var)`

_No description._

### process

#### `exit(code)`

_No description._

#### `argc = __process_argc()`

_No description._

#### `argv = __process_argv()`

_No description._

### math

#### `PI = 3.141592653589793`

_No description._

#### `E = 2.718281828459045`

_No description._

#### `pow(base, exponent)`

_No description._

#### `square(n)`

_No description._

#### `sqrt = __math_sqrt`

_No description._

#### `abs(n)`

_No description._

#### `min(a, b)`

_No description._

#### `max(a, b)`

_No description._

#### `clamp(n, lo, hi)`

_No description._

#### `round(n)`

_No description._

#### `rand = __math_rand`

_No description._

#### `sin = __math_sin`

_No description._

#### `cos = __math_cos`

_No description._

#### `tan = __math_tan`

_No description._

#### `atan = __math_atan`

_No description._

#### `atan2 = __math_atan2`

_No description._

#### `exp = __math_exp`

_No description._

#### `log = __math_log`

_No description._

#### `log10(x)`

_No description._

#### `log2(x)`

_No description._

#### `floor = __math_floor`

_No description._

#### `ceil = __math_ceil`

_No description._

### net

#### `create_server(port, on_connection)`

Legacy blocking server. Spins an internal accept loop and a yield
loop on the main task, exiting only when shutdown is requested.

#### `connect(host, port)`

TCP client. Returns an IO handle that resolves to a conn:
  { fd, ip, port }
Read/write/close via net.read(conn, cb), net.write(conn, data, cb),
net.close(conn). The conn object is also accepted directly.

#### `listen(port)`

TCP listener (non-blocking). Returns an IO handle resolving to:
  { fd, port }
Pass port=null or 0 for an ephemeral kernel-assigned port; the
resolved server.port reflects the bound port. Use net.accept(server)
to wait for a single inbound connection (loop to keep accepting),
and net.close_server(server) when done.

#### `accept(server)`

Wait for one inbound TCP connection on a listening server. Returns
an IO handle resolving to a conn object { fd, ip, port }. To issue
accept concurrently with other work (e.g. a same-process connect),
call accept() WITHOUT awaiting first, then await later.

#### `read(conn, cb)`

Read up to one chunk from a TCP conn. cb(err, data).

#### `write(conn, data, cb)`

Write data to a TCP conn. cb(err).

#### `close(conn)`

Close a TCP conn.

#### `close_server(server)`

Close a TCP listener.

#### `udp_socket(port)`

UDP socket. Returns an IO handle resolving to { fd, port }.
port null/0 -> ephemeral. Use net.udp_send(sock, host, port, data),
net.udp_recv(sock, timeout_ms?), net.udp_close(sock).

#### `udp_send(sock, host, port, data)`

Send a UDP datagram. Returns an IO handle resolving to bytes sent.

#### `udp_recv(sock, timeout_ms)`

Receive one UDP datagram. timeout_ms is optional; null/0 blocks
forever (until shutdown). Resolves to { data, ip, port }.

#### `udp_close(sock)`

Close a UDP socket.

### http

#### `get(url)`

_No description._

#### `request(options, callback)`

_No description._

### JSON

#### `parse(text)`

parse a JSON string into a Nari value (object, array, string, number, bool, or null)
throws SyntaxError on malformed input

#### `stringify(value, indent)`

serialize a Nari value to a JSON string
optional second argument `indent` (integer) enables pretty-printing

### system

`system` is the small ambient utility namespace. Domain modules
(Regex, Path, Date, Random, uuid, Hash, Archive, Hex, Base64, Map, Set,
Logger) are NOT bundled here - import them from "std/<name>".

#### `version = stdlib_version`

_No description._

#### `print = print`

_No description._

#### `math = math`

_No description._

#### `fs = fs`

_No description._

#### `io = io`

_No description._

#### `net = net`

_No description._

#### `http = http`

_No description._

#### `platform = platform`

_No description._

#### `json = JSON`

_No description._

#### `exec(command)`

_No description._

### Spawn

#### `race(handles)`

race: Returns the fastest completing handle

#### `all(handles)`

all: Wait for all handles to complete, return array of results

#### `all_settled(handles)`

all_settled: Wait for all handles, return results including failures
Returns array of { status, value?, error?, duration }

#### `any(handles)`

any: Return first successful handle (by completion time)
Note: Returns fastest successful, not necessarily first to succeed

#### `map(items, fn)`

map: Apply async operation to array of items
Usage: Spawn.map(urls, func(url) { return http.get(url); })

---

## Archive

_Source: `src/stdlib/std/archive.nari`_

Archive read/write backed by libarchive (tar, tar.gz, zip).
Extraction performs safety checks against zip-slip / absolute paths /
symlinks / size limits - see src/builtins/archive.cpp for details.

    import { Archive } from "std/archive";

### Archive

#### `list(path)`

Archive.list(path) -> [{ name, size, type, mtime }, ...]
type: "file" | "dir" | "symlink" | "other"

#### `extract(archive_path, dest_dir, opts)`

Archive.extract(archive_path, dest_dir, opts?) -> { files, bytes }
opts: { max_files?: int=10000, max_bytes?: int=1<<30, allow_links?: bool=false }
Rejects entries with absolute or escaping paths, and symlinks
(unless allow_links=true). dest_dir is created if needed.

#### `create(archive_path, files, opts)`

Archive.create(archive_path, files, opts?) -> { files, bytes }
Format inferred from extension: .tar / .tar.gz / .tgz / .zip.
`files`: array of either string (source path) or { src, dest }.
opts: { base_dir?: string } - when set, string sources are stored
  relative to base_dir; otherwise the basename is used.

---

## Map / Set

_Source: `src/stdlib/std/collections.nari`_

Ordered string-keyed collections.

    import { Map, Set } from "std/collections";

Map: O(1) lookup via a backing object plus a parallel keys array for
stable insertion order. Keys are stringified at insert time.
Set: ordered string-keyed set built on top of Map. Stringifies values at
insert time, so Set.add(1) and Set.add("1") collide.

### Map

#### `create()`

_No description._

### Set

#### `create(initial)`

_No description._

---

## Date

_Source: `src/stdlib/std/date.nari`_

Wall-clock time. All timestamps are integer milliseconds since the Unix
epoch (1970-01-01T00:00:00Z). Designed for determinism: parse_iso of a
value without an explicit timezone is treated as UTC.

    import { Date } from "std/date";
    let d = Date.from_ms(Date.now());
    d.year; d.month; d.day; d.to_iso_string();

Format strings use C strftime with one extension: "%L" -> 3-digit
milliseconds.

### Date

#### `now()`

Returns ms-since-epoch (int).

#### `utc(year, month, day, hour, minute, second, ms)`

Construct a UTC timestamp from components. month is 1..12.

#### `parse_iso(s)`

Parse an ISO-8601 / RFC-3339 string. Throws DateError on failure.

#### `format(ms, fmt, utc)`

strftime-style format. utc defaults to true.

#### `from_ms(ms, opts)`

Build a Date instance from ms. opts.utc defaults to true.

---

## Hex / Base64

_Source: `src/stdlib/std/encoding.nari`_

Binary-safe Hex / Base64 codecs backed by native builtins. They operate
on raw byte strings (bytes 0-255), unlike from_char_code (capped at 127).

    import { Hex, Base64 } from "std/encoding";

### Hex

#### `encode(s)`

Hex.encode(str) -> lowercase hex string (2 chars per byte)

#### `decode(s)`

Hex.decode(hex) -> byte string. Accepts upper/lowercase; throws on
odd length or non-hex characters.

### Base64

#### `encode(s)`

Base64.encode(str) -> standard base64 with '=' padding

#### `decode(s)`

Base64.decode(b64) -> byte string. Skips ASCII whitespace; throws on
invalid characters.

---

## Hash

_Source: `src/stdlib/std/hash.nari`_

Cryptographic hash helpers backed by mbedtls. Intended for integrity
checks (lockfile, registry archives) - not authentication.

    import { Hash } from "std/hash";

### Hash

#### `sha256(s)`

Hash.sha256(str) -> 64-char lowercase hex string of SHA-256(bytes(str))

#### `sha256_file(path)`

Hash.sha256_file(path) -> 64-char lowercase hex string of SHA-256
of the file's bytes. Streams the file; safe for large archives.

---

## Logger

_Source: `src/stdlib/std/logger.nari`_

Leveled logging with timestamps. Routes to system.print by default;
pass a `sink` function to capture (e.g. for tests or file output).
Levels: trace=10, debug=20, info=30, warn=40, error=50, fatal=60.
Set min level via Logger.create({ level: "info" }) - anything lower is
silently dropped.

    import { Logger } from "std/logger";

### Logger

#### `levels =  { ... }`

_No description._

#### `create(opts)`

Logger.create(opts?) -> logger instance.
opts: { name?: string, level?: string|int=info, sink?: func(line),
        timestamps?: bool=true }

---

## Path

_Source: `src/stdlib/std/path.nari`_

POSIX-style path utilities (forward-slash separator). Pure Nari - no
filesystem access. Mirrors the subset of node:path / std/path most
scripts need: join, basename, dirname, extname, normalize, is_absolute.

    import { Path } from "std/path";

### Path

#### `sep = "/"`

_No description._

#### `is_absolute(p)`

_No description._

#### `basename(p, ext)`

basename(path, ext?) -> last component, optionally stripping ext

#### `dirname(p)`

dirname(path) -> everything before the last separator

#### `extname(p)`

extname(path) -> ".ext" or "" (mirrors node:path semantics for
leading-dot files: ".bashrc" has no extname)

#### `join(parts)`

join(...parts) -> single normalized path. nulls/empty parts skipped.

#### `normalize(p)`

normalize(path) -> collapse ".", ".." and duplicate slashes

---

## Random

_Source: `src/stdlib/std/random.nari`_

Seedable PRNG (xorshift32). Deterministic given the same seed; entirely
pure-Nari so it does NOT share state with math.rand (which uses a global
mt19937). NOT cryptographically secure.

    import { Random } from "std/random";

### Random

#### `create(seed)`

Random.create(seed?) -> { next, next_int(lo, hi), next_float(), pick(arr) }
seed=0 is reseeded to 1 (xorshift32 cannot recover from 0).

---

## Regex

_Source: `src/stdlib/std/regex.nari`_

Stdlib surface over the runtime's regex engine (SRELL, ECMAScript syntax).
Lets you build regexes from dynamic strings (literals like /foo/i remain
the primary form). Returns/uses native regex values, so .test() / .exec()
remain available as methods.

    import { Regex } from "std/regex";

### Regex

#### `new(pattern, flags)`

Regex.new(pattern, flags?) -> regex value. Throws RegexError on
malformed pattern. flags: any of "imsguvy" (g/u/v/y silently accepted).

#### `escape(s)`

Regex.escape(str) -> str with all regex metacharacters backslash-escaped

#### `match(re, str)`

Regex.match(re, str) -> first match object {value, index, groups} or null.
Accepts either a regex value or a pattern string.

#### `test(re, str)`

Regex.test(re, str) -> bool. Accepts regex or pattern string.

#### `split(re, str)`

Regex.split(re, str) -> array of segments split by every match.
No-match returns [str].

#### `replace(re, str, replacement)`

Regex.replace(re, str, replacement) -> str with FIRST match replaced.
`replacement` is a literal string ($1 etc. not interpolated - use
Regex.replace_with for callback-based substitution).

#### `replace_all(re, str, replacement)`

Regex.replace_all(re, str, replacement) -> str with EVERY non-overlapping
match replaced. Literal replacement (see Regex.replace).

---

## URL

_Source: `src/stdlib/std/url.nari`_

Permissive parser/builder for URLs of the shape
  scheme://[user[:pass]@]host[:port][/path][?query][#fragment]
Parsing is intentionally lenient (does not enforce host syntax) so that
it works for HTTP, ws://, file://, custom schemes, etc. For strict
WHATWG-conformant parsing you would want a dedicated native parser; this
is the 95% case for stdlib consumers.

  import { URL } from "std/url";
  let u = URL.parse("https://alice:s3cr3t@example.com:8443/a/b?x=1&y=2#top");
  u.protocol; // "https"
  u.host;     // "example.com"
  u.params;   // { x: "1", y: "2" }
  u.to_string();

Percent-encoding helpers:
  URL.encode(s) / URL.decode(s)                - strict component encoding
                                                 (slashes encoded too).
  URL.encode_path(s)                            - path-safe (keeps "/" ":").
  URL.parse_query(s) / URL.format_query(obj)     - `?k=v&k2=v2` <-> object.

### URL

#### `encode(s)`

Component-encode a string (everything but unreserved is %XX).
Use for query keys/values, username, password, fragment.

#### `encode_path(s)`

Path-safe encode (leaves "/" ":" "@" untouched). Use for path
segments where slashes are meaningful.

#### `decode(s)`

Decode a percent-encoded string. Throws URLError on malformed input.

#### `decode_form(s)`

Decode as application/x-www-form-urlencoded ("+" -> space).

#### `parse_query(s)`

Parse a query string ("a=1&b=2" or "?a=1&b=2") into an object.
Repeated keys collapse to the LAST value. Use parse_query_all for arrays.

#### `parse_query_all(s)`

Like parse_query but preserves all values for repeated keys.
Single-value keys still map to a string; repeated keys map to an array.

#### `format_query(obj)`

Format an object as a query string. Values may be strings, numbers,
bools, or arrays (each element emitted as a separate k=v pair).
Null values are skipped. Output does NOT include a leading "?".

#### `parse(s)`

Parse a full URL string. Returns an instance object with fields and
helper methods. Throws URLError if the scheme is missing.

#### `create(parts)`

Build a URL string from an object of parts. Parts are written
verbatim - no re-encoding - so callers should pass already-encoded
path/search if they contain special characters (or use URL.parse
then .with()).

---

## uuid

_Source: `src/stdlib/std/uuid.nari`_

uuid.v4() -> RFC-4122 version-4 UUID string. Backed by the
non-cryptographic PRNG (math.rand / mt19937) - do NOT use for
security-sensitive tokens.

    import { uuid } from "std/uuid";

### uuid

#### `v4()`

_No description._
