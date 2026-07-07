#!/usr/bin/env python3
"""
Mutate a compiled .naric file to produce semantically-invalid bytecode that
should be caught by the bytecode verifier.

Called by tests/naric_robustness.sh to exercise each verifier path.

Usage:
    verifier_mutate.py <in.naric> <out.naric> <mutation>

Mutations:
    bad_opcode      write 0xFE (unknown opcode) at start of first function
    huge_const_idx  write OP_LOAD_CONST (0) + operand 0xFFFF
    huge_str_idx    write OP_LOAD_GLOBAL (3) + operand 0xFFFF
    bad_local_idx   write OP_LOAD_VAR (1) + operand 0xFFFF
    stack_underflow write OP_POP at function entry
    bad_jump_target write OP_JUMP to the middle of its own operand

Opcode numeric values come from the enum order in src/bytecode.h and must
stay in sync with it.
"""
import struct
import sys


OP_LOAD_CONST = 0
OP_LOAD_VAR = 1
OP_LOAD_GLOBAL = 3
OP_POP = 5
OP_LOAD_NONE = 7
OP_RETURN = 42
OP_JUMP = 36
# OP_JUMP is the first jump; we find it by pattern, not value.
# Its byte value depends on the enum order see, check src/bytecode.h.
# Instead of hardcoding, find the start of the code section and mutate
# the first byte matching the requested semantic.


def read_u32(buf, pos):
    return struct.unpack_from("<I", buf, pos)[0], pos + 4


def read_u16(buf, pos):
    return struct.unpack_from("<H", buf, pos)[0], pos + 2


def read_u8(buf, pos):
    return buf[pos], pos + 1


def skip_string(buf, pos):
    n, pos = read_u32(buf, pos)
    return pos + n


def first_code_offset(buf):
    """
    Walk the header and function metadata to find the offset of the first function's code bytes.

    Returns (offset, code_len, main_fn_idx).
    """
    assert buf[:4] == b"NARI"
    pos = 8  # magic(4) + version(2) + flags(2)
    # string table
    n_strs, pos = read_u32(buf, pos)
    for _ in range(n_strs):
        pos = skip_string(buf, pos)
    # function table
    n_funcs, pos = read_u32(buf, pos)
    main_idx, pos = read_u32(buf, pos)
    # first function metadata
    pos = skip_string(buf, pos)  # name
    pos += 5  # param_count, capture_count, rest_param_idx, is_lambda, strict_mode
    n_vars, pos = read_u32(buf, pos)
    for _ in range(n_vars):
        pos = skip_string(buf, pos)
    n_consts, pos = read_u32(buf, pos)
    for _ in range(n_consts):
        pos += 1 + 8  # type byte + 8 bytes payload
    code_len, pos = read_u32(buf, pos)
    return pos, code_len, main_idx


def find_opcode(buf, start, length, target_op):
    """
    Walk the code section looking for the first occurrence of `target_op`.
    Returns its offset, or -1 if not found.
    Only looks at opcode positions by following known operand widths.
    """
    # Minimal operand-width table matching bytecode_verify.h.
    # Only the opcodes we might search for need to be correct; others we
    # treat conservatively as 2-byte operand (good enough for small files).
    widths = {
        OP_LOAD_CONST: 2,
        OP_LOAD_VAR: 2,
        # OP_STORE_VAR=2, OP_LOAD_GLOBAL=3, ...
    }
    pc = start
    end = start + length
    while pc < end:
        op = buf[pc]
        if op == target_op:
            return pc
        # Unknown operand width: bail out.
        w = widths.get(op, 2)
        pc += 1 + w
    return -1


def main():
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        sys.exit(2)

    in_path, out_path, mutation = sys.argv[1], sys.argv[2], sys.argv[3]
    buf = bytearray(open(in_path, "rb").read())
    code_off, code_len, _ = first_code_offset(buf)

    if mutation == "bad_opcode":
        buf[code_off] = 0xFE

    elif mutation == "huge_const_idx":
        # The first opcode in any function body is usually a LOAD_CONST/LOAD_*.
        # Write OP_LOAD_CONST (value 0) + operand 0xFFFF.
        buf[code_off] = 0  # OP_LOAD_CONST
        buf[code_off + 1] = 0xFF  # msb
        buf[code_off + 2] = 0xFF  # lsb

    elif mutation == "huge_str_idx":
        # OP_LOAD_GLOBAL is enum value 3. Its operand is a 2-byte string idx.
        buf[code_off] = 3  # OP_LOAD_GLOBAL
        buf[code_off + 1] = 0xFF
        buf[code_off + 2] = 0xFF

    elif mutation == "bad_local_idx":
        buf[code_off] = 1  # OP_LOAD_VAR
        buf[code_off + 1] = 0xFF
        buf[code_off + 2] = 0xFF

    elif mutation == "stack_underflow":
        # Preserve the original first instruction's 3-byte footprint while
        # replacing it with three no-operand instructions. LOAD_NONE supplies one
        # value, the first POP consumes it, and the second POP underflows on the
        # reachable entry path.
        buf[code_off] = OP_LOAD_NONE
        buf[code_off + 1] = OP_POP
        buf[code_off + 2] = OP_POP

    elif mutation == "bad_jump_target":
        # OP_JUMP has a signed 16-bit operand. Offset -1 targets the middle of
        # the jump instruction operand, which is within the function body but
        # not an instruction boundary.
        buf[code_off] = OP_JUMP
        buf[code_off + 1] = 0xFF
        buf[code_off + 2] = 0xFF

    else:
        print(f"unknown mutation: {mutation}", file=sys.stderr)
        sys.exit(2)

    open(out_path, "wb").write(bytes(buf))


if __name__ == "__main__":
    main()
