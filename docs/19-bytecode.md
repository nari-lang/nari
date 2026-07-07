# Nari Bytecode (`.naric`)

Nari source files can be compiled to bytecode with `naric` and later executed by the interpreter.

```bash
./build/release/naric script.nari -o script.naric
./build/release/interpreter script.naric
```

The `.naric` format is currently an internal/runtime format rather than a long-term stable ABI. It is useful for faster startup, bytecode-only builds, embedded targets, and compiled module imports.

## Compatibility Status

The bytecode format is versioned, but it should still be treated as tied to the interpreter version that produced it unless a future release explicitly declares bytecode compatibility.

Current policy:

- `.naric` files are expected to be produced by the matching `naric` binary.
- Older or newer bytecode may be rejected by the deserializer or verifier.
- The verifier checks structural safety before execution, but does not make untrusted bytecode safe to run with unrestricted runtime capabilities.

## High-Level Structure

A bytecode file contains:

1. A magic header and format version.
2. A string table.
3. A function table.
4. Per-function metadata:
   - name,
   - source file,
   - bytecode instruction stream,
   - constants,
   - local variable names,
   - source line map,
   - parameter/capture counts,
   - strict-mode/type metadata.
5. Serialized type declarations used by runtime introspection and FFI struct metadata.
6. The main function index.

All multi-byte bytecode operands in instruction streams are encoded big-endian: high byte first, low byte second. The VM reads them as `(a << 8) | b`.

## Instruction Streams

Each function has a linear bytecode stream. The first byte of every instruction is an opcode from `OpCode` in `src/bytecode/bytecode.h`. Operands follow immediately after the opcode.

Common operand shapes are:

- no operand,
- `u8` argument count,
- `u16` local/string/constant/function index,
- signed `i16` relative jump offset,
- `u16 + u8` for class/method/type operations,
- `u16 + u16` for regex and try-handler operands,
- variable-length closure operands.

Closure operands are encoded as:

```text
OP_MAKE_CLOSURE
  func_idx: u16
  capture_count: u8
  repeated capture_count times:
    source: u8    # 0 = parent local, 1 = parent capture, 2 = global name
    index:  u16   # local/capture slot or string-table index for globals
```

## Verification

`BytecodeSerializer::deserialize()` runs `BytecodeVerifier::verify()` before returning a loaded chunk. If verification fails, deserialization returns null and execution is refused.

The verifier currently checks:

- `main_func_idx` is in range.
- Constant table references point to valid strings/functions.
- Every opcode is known.
- Operand bytes are not truncated.
- String, local, capture, constant, function, class, method, type, and regex indices are in range.
- Jump and try-handler targets stay within the function body.
- Jump and try-handler targets land on instruction boundaries.
- Closure capture descriptors are complete and refer to valid parent locals/captures or global strings.
- A conservative stack-height dataflow pass rejects obvious stack underflow and inconsistent stack heights at control-flow joins.

The verifier does not currently prove:

- runtime type correctness,
- that every code path reaches `RETURN` or `THROW`,
- semantic validity of all exception-control-flow combinations,
- or safety of runtime capabilities such as FFI, filesystem access, process execution, network access, or package code.

## Compiled Module Imports

Source files may import precompiled `.naric` modules. When this happens, the interpreter merges the module chunk into the current program chunk.

During merge:

- module strings are remapped into the destination string table,
- module function indices are remapped into the destination function table,
- the module main/top-level function is renamed to the generated module-init function name,
- string/function operands inside bytecode are rewritten,
- and the merge fails atomically if an unknown/truncated/unmappable opcode is encountered.

This keeps partially remapped functions out of the destination chunk.

## Future Work

Planned improvements include:

- centralizing opcode metadata so the VM, verifier, disassembler, JIT scanner, and compiled-module merger share one source of truth,
- stronger exception-flow verification,
- stable bytecode compatibility policy once the language/runtime settles,
- richer disassembly output,
- and conformance tests for all bytecode opcodes and verifier failure modes.
