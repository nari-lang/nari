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
    short_compact    replace the first function body with a compact alias missing its operand

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


def read_u16(buf, pos):
    return struct.unpack_from("<H", buf, pos)[0], pos + 2


def read_u8(buf, pos):
    return buf[pos], pos + 1


def read_varuint(buf, pos):
    value = 0
    shift = 0
    while True:
        if shift >= 64:
            raise ValueError("varuint overflow")
        byte, pos = read_u8(buf, pos)
        value |= (byte & 0x7F) << shift
        if byte & 0x80 == 0:
            return value, pos
        shift += 7


def skip_string(buf, pos):
    n, pos = read_varuint(buf, pos)
    return pos + n


def first_code_offset(buf):
    """
    Walk the header and function metadata to find the offset of the first function's code bytes.

    Returns (length offset, code offset, code length, main function index).
    """
    assert buf[:4] == b"NARI"
    pos = 8  # magic(4) + version(2) + flags(2)
    # string table
    n_strs, pos = read_varuint(buf, pos)
    for _ in range(n_strs):
        pos = skip_string(buf, pos)
    # function table
    n_funcs, pos = read_varuint(buf, pos)
    main_idx, pos = read_varuint(buf, pos)
    # first function metadata
    pos = skip_string(buf, pos)  # name
    # param_count(u8), capture_count(u16), rest_param_idx(i8), is_lambda(u8),
    # js_undefined_params(u8), strict_mode(u8) -- keep in sync with
    # BytecodeSerializer::serialize in src/bytecode/bytecode_serializer.h
    pos += 7
    n_vars, pos = read_varuint(buf, pos)
    for _ in range(n_vars):
        pos = skip_string(buf, pos)
    n_consts, pos = read_varuint(buf, pos)
    for _ in range(n_consts):
        const_type, pos = read_u8(buf, pos)
        if const_type == 1:  # zigzag integer
            _, pos = read_varuint(buf, pos)
        elif const_type == 2:  # float
            pos += 8
        elif const_type in (3, 4):  # string, function
            _, pos = read_varuint(buf, pos)
        elif const_type != 0:  # none has no payload
            raise ValueError(f"unknown constant type {const_type}")
    length_pos = pos
    code_len, pos = read_varuint(buf, pos)
    return length_pos, pos, code_len, main_idx


def write_varuint(value):
    result = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        result.append(byte | (0x80 if value else 0))
        if not value:
            return result


def replace_first_instruction(buf, length_pos, code_off, code_len, replacement):
    encoded_size = 2 if buf[code_off] & 0x80 else 3
    new_code_len = code_len - encoded_size + len(replacement)
    buf[code_off:code_off + encoded_size] = replacement
    buf[length_pos:code_off] = write_varuint(new_code_len)


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
    length_pos, code_off, code_len, _ = first_code_offset(buf)

    if mutation == "bad_opcode":
        buf[code_off] = 0xFE

    elif mutation == "huge_const_idx":
        # The first opcode in any function body is usually a LOAD_CONST/LOAD_*.
        # Write OP_LOAD_CONST (value 0) + operand 0xFFFF.
        replace_first_instruction(buf, length_pos, code_off, code_len, bytes((OP_LOAD_CONST, 0xFF, 0xFF)))

    elif mutation == "huge_str_idx":
        # OP_LOAD_GLOBAL is enum value 3. Its operand is a 2-byte string idx.
        replace_first_instruction(buf, length_pos, code_off, code_len, bytes((OP_LOAD_GLOBAL, 0xFF, 0xFF)))

    elif mutation == "bad_local_idx":
        replace_first_instruction(buf, length_pos, code_off, code_len, bytes((OP_LOAD_VAR, 0xFF, 0xFF)))

    elif mutation == "stack_underflow":
        # Preserve the original first instruction's 3-byte footprint while
        # replacing it with three no-operand instructions. LOAD_NONE supplies one
        # value, the first POP consumes it, and the second POP underflows on the
        # reachable entry path.
        replace_first_instruction(buf, length_pos, code_off, code_len, bytes((OP_LOAD_NONE, OP_POP, OP_POP)))

    elif mutation == "bad_jump_target":
        # OP_JUMP has a signed 16-bit operand. Offset -1 targets the middle of
        # the jump instruction operand, which is within the function body but
        # not an instruction boundary.
        replace_first_instruction(buf, length_pos, code_off, code_len, bytes((OP_JUMP, 0xFF, 0xFF)))

    elif mutation == "short_compact":
        # A compact LOAD_CONST alias must be followed by its low operand byte.
        # Keep the remainder of the file intact so rejection specifically comes
        # from expanding this one-byte code section.
        replacement = bytes((OP_LOAD_CONST | 0x80,))
        buf = buf[:length_pos] + write_varuint(len(replacement)) + replacement + buf[code_off + code_len:]

    else:
        print(f"unknown mutation: {mutation}", file=sys.stderr)
        sys.exit(2)

    open(out_path, "wb").write(bytes(buf))


if __name__ == "__main__":
    main()
