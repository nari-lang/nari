# JIT internals

The bytecode VM owns a `JitSession`. It owns the method compiler, trace compiler,
and executable code for that VM. VM call paths use the session API for
compilation and entry lookup; `on_backedge` owns trace heat and profitability.
DAP execution constructs its VM with JIT
disabled because native code has no per-instruction debugger hook.

The method compiler binds a `Chunk` before the VM uses compiled entries. Binding
a different chunk drains asynchronous compilation and replaces the fixed entry
table. Generated calls embed addresses in that table, so it must not resize
while its code is live. Functions appended to a chunk after binding run in the
interpreter until a later VM run rebinds the chunk. The trace cache is cleared
at the start of each VM run because traces embed bytecode instruction pointers.

Trace code is organized in `src/jit/trace/`: `record.cpp` records an iteration,
`verify.cpp` checks its exit metadata, `opt.cpp` performs trace rewrites, and
`lower.cpp` emits machine code. Recording receives the pre-dispatch function
identity and instruction PC. A step that can side-exit carries an optional
resume PC; zero is a valid PC. Lowering checks the recording before generating
code and reconstructs the interpreter operand stack for resumable exits.

Integer add, subtract, and multiply in the method IR return `Number`, since
overflow promotes an `Int48` to a float. The register tier keeps such results
boxed. The register tier uses guarded integer fast paths for arithmetic and
comparisons on boxed numeric values, with helper fallbacks for other types and
overflow. A conservative range pass marks arithmetic as `proven_int48` when
loop guards and bounded operands prove that the result cannot overflow. It
also recognizes bounded multiplication followed by modulo. The IR verifier
checks typed integer operations before code generation.

`src/jit/abi.h` owns the VM, frame, value, and heap offsets used by generated
code. It uses the compiler's layout extension for non-standard-layout C++
objects, avoiding construction of VM objects to discover offsets. The JIT is
enabled only after the standard-library layout check succeeds.
