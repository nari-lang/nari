#pragma once

#include "bytecode.h"
#include "bytecode_verify.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace nari {
namespace bytecode {

/*
Nari Bytecode file format (.naric)

Header:
    magic:   4 bytes   "NARI"
    version: 2 bytes   u16 (currently 13)
    flags:   2 bytes   u16 (reserved)

String table:
    count:  ULEB128
    for each string:
        length: ULEB128
        data:   N bytes string

Function table:
    count:  ULEB128
    main_func_idx: ULEB128
    for each function:
        name_length:  ULEB128
        name:  N bytes string
        param_count:  1 byte  u8
        capture_count:  2 bytes u16
        rest_param_idx: 1 byte  i8
        is_lambda:  1 byte  u8
        var_count:  ULEB128
        for each var_name:
            length: ULEB128
            data:  N bytes string
        const_count:  ULEB128
        for each constant:
            type:  1 byte  u8 (0=none, 1=int, 2=float, 3=string, 4=function)
            data:  type-specific (none=0, int=zigzag ULEB128, float=8, string/function=ULEB128)
        code_length:  ULEB128
        code:  N bytes (common u16 operands below 256 use a compact opcode alias)
*/

static constexpr uint8_t MAGIC[4] = { 'N', 'A', 'R', 'I' };
static constexpr uint16_t FORMAT_VERSION = 12;

class BytecodeSerializer {
  public:
    // serialize a compiled Chunk to a byte buffer
    static std::vector<uint8_t> serialize(const Chunk &chunk) {
        std::vector<uint8_t> buf;

        // header
        write_bytes(buf, MAGIC, 4);
        write_u16(buf, FORMAT_VERSION);
        write_u16(buf, 0); // flags (reserved)

        // string table
        write_varuint(buf, chunk.strings.size());
        for (const auto &s : chunk.strings) {
            write_string(buf, s);
        }

        // function table
        write_varuint(buf, chunk.functions.size());
        write_varuint(buf, chunk.main_func_idx);

        for (const auto &func : chunk.functions) {
            // name
            write_string(buf, func.name);

            // metadata
            write_u8(buf, func.param_count);
            write_u16(buf, func.capture_count);
            write_i8(buf, func.rest_param_index);
            write_u8(buf, func.is_lambda ? 1 : 0);
            write_u8(buf, func.js_undefined_params ? 1 : 0);
            write_u8(buf, func.strict_mode ? 1 : 0);

            // var_names
            write_varuint(buf, func.var_names.size());
            for (const auto &vn : func.var_names) {
                write_string(buf, vn);
            }

            // constants
            write_varuint(buf, func.constants.size());
            for (const auto &c : func.constants) {
                write_u8(buf, static_cast<uint8_t>(c.type));
                switch (c.type) {
                    case Constant::Type::INT:
                        write_varint(buf, c.as_int);
                        break;
                    case Constant::Type::FLOAT:
                        write_f64(buf, c.as_float);
                        break;
                    case Constant::Type::STRING:
                        write_varuint(buf, c.string_idx);
                        break;
                    case Constant::Type::FUNCTION:
                        write_varuint(buf, c.func_idx);
                        break;
                    case Constant::Type::NONE:
                        break;
                }
            }

            // code
            std::vector<uint8_t> serialized_code = compact_code(func.code);
            write_varuint(buf, serialized_code.size());
            write_bytes(buf, serialized_code.data(), serialized_code.size());
        }

        // type declarations (for FFI struct support)
        write_varuint(buf, chunk.types.size());
        for (const auto &type : chunk.types) {
            write_string(buf, type.name);
            write_u8(buf, type.is_union ? 1 : 0);
            write_string(buf, type.alias_target); // empty string if not an alias
            write_varuint(buf, type.fields.size());
            for (const auto &field : type.fields) {
                write_string(buf, field.name);
                write_string(buf, field.type_name);
                write_u8(buf, field.is_array ? 1 : 0);
                write_u64(buf, field.fixed_array_count);
            }
        }

        return buf;
    }

    // deserialize a byte buffer into a Chunk
    static Chunk *deserialize(const uint8_t *data, size_t length) {
        size_t pos = 0;

        // header
        if (length < 8) {
            fprintf(stderr, "naric: file too small for header\n");
            return nullptr;
        }

        if (memcmp(data, MAGIC, 4) != 0) {
            fprintf(stderr, "naric: invalid magic bytes (not a .naric file)\n");
            return nullptr;
        }
        pos += 4;

        // own the Chunk via unique_ptr until the end so any exception frees it automatically
        std::unique_ptr<Chunk> chunk;
        try {
            uint16_t version = read_u16(data, length, pos);
            if (version != FORMAT_VERSION) {
                fprintf(stderr, "naric: unsupported format version %u (expected %u)\n", version, FORMAT_VERSION);
                return nullptr;
            }

            /* uint16_t flags = */ read_u16(data, length, pos); // reserved

            chunk.reset(new Chunk());

            // sanity bound for container counts to prevent huge pre-allocation from a crafted file.
            constexpr uint32_t MAX_COUNT = 1u << 20;

            // string table
            uint32_t string_count = read_varuint32(data, length, pos);
            if (string_count > MAX_COUNT) {
                throw DeserializeError{};
            }
            chunk->strings.resize(string_count);
            for (uint32_t i = 0; i < string_count; i++) {
                chunk->strings[i] = read_string(data, length, pos);
            }

            // function table
            uint32_t func_count = read_varuint32(data, length, pos);
            if (func_count > MAX_COUNT) {
                throw DeserializeError{};
            }
            chunk->main_func_idx = read_varuint32(data, length, pos);
            chunk->functions.resize(func_count);

            for (uint32_t fi = 0; fi < func_count; fi++) {
                FunctionMeta &func = chunk->functions[fi];

                // name
                func.name = read_string(data, length, pos);

                // metadata
                func.param_count = read_u8(data, length, pos);
                func.capture_count = read_u16(data, length, pos);
                func.rest_param_index = read_i8(data, length, pos);
                func.is_lambda = read_u8(data, length, pos) != 0;
                func.js_undefined_params = read_u8(data, length, pos) != 0;
                func.strict_mode = read_u8(data, length, pos) != 0;

                // var_names
                uint32_t var_count = read_varuint32(data, length, pos);
                if (var_count > MAX_COUNT) {
                    throw DeserializeError{};
                }
                func.var_names.resize(var_count);
                for (uint32_t vi = 0; vi < var_count; vi++) {
                    func.var_names[vi] = read_string(data, length, pos);
                }

                // constants
                uint32_t const_count = read_varuint32(data, length, pos);
                if (const_count > MAX_COUNT) {
                    throw DeserializeError{};
                }
                func.constants.resize(const_count);
                for (uint32_t ci = 0; ci < const_count; ci++) {
                    Constant &c = func.constants[ci];
                    c.type = static_cast<Constant::Type>(read_u8(data, length, pos));
                    switch (c.type) {
                        case Constant::Type::INT:
                            // go through make_int() rather than assigning as_int directly
                            c = Constant::make_int(read_varint(data, length, pos));
                            break;
                        case Constant::Type::FLOAT:
                            c.as_float = read_f64(data, length, pos);
                            break;
                        case Constant::Type::STRING:
                            c.string_idx = read_varuint32(data, length, pos);
                            if (c.string_idx >= chunk->strings.size()) {
                                throw DeserializeError{};
                            }
                            break;
                        case Constant::Type::FUNCTION:
                            c.func_idx = read_varuint32(data, length, pos);
                            break;
                        case Constant::Type::NONE:
                            break;
                        default:
                            throw DeserializeError{};
                    }
                }

                // code
                uint32_t code_len = read_varuint32(data, length, pos);
                // Use `code_len > length - pos` form to avoid potential
                // size_t wrap on 32-bit hosts. The (1<<26) ceiling caps the
                // allocation regardless.
                if (code_len > (1u << 26) || pos > length || code_len > length - pos) {
                    throw DeserializeError{};
                }
                expand_code(data + pos, code_len, func.code);
                pos += code_len;
            }

            // type declarations (for FFI struct support)
            if (pos < length) {
                uint32_t type_count = read_varuint32(data, length, pos);
                if (type_count > MAX_COUNT) {
                    throw DeserializeError{};
                }
                chunk->types.resize(type_count);
                for (uint32_t ti = 0; ti < type_count; ti++) {
                    TypeInfo &type = chunk->types[ti];
                    type.name = read_string(data, length, pos);
                    type.is_union = read_u8(data, length, pos) != 0;
                    type.alias_target = read_string(data, length, pos);
                    uint32_t field_count = read_varuint32(data, length, pos);
                    if (field_count > MAX_COUNT) {
                        throw DeserializeError{};
                    }
                    type.fields.resize(field_count);
                    for (uint32_t fi = 0; fi < field_count; fi++) {
                        type.fields[fi].name = read_string(data, length, pos);
                        type.fields[fi].type_name = read_string(data, length, pos);
                        type.fields[fi].is_array = read_u8(data, length, pos) != 0;
                        type.fields[fi].fixed_array_count = read_u64(data, length, pos);
                        if (type.fields[fi].fixed_array_count > nari::MAX_FIXED_ARRAY_COUNT) {
                            throw DeserializeError{};
                        }
                    }
                }
            }
        } catch (const DeserializeError &) {
            fprintf(stderr, "naric: malformed or truncated file\n");
            // unique_ptr destructor will free chunk if any.
            return nullptr;
        } catch (const std::bad_alloc &) {
            fprintf(stderr, "naric: out of memory while deserializing\n");
            return nullptr;
        }

        // verify every opcode's operands reference valid constants, strings, locals, functions, and jump targets.
        if (!BytecodeVerifier::verify(*chunk)) {
            return nullptr;
        }

        // Transfer ownership to the caller (matches the pre-existing `Chunk*` return contract).
        return chunk.release();
    }

  private:
    static bool has_compact_u16_alias(OpCode op) {
        if (opcode_operand_size(op) != 2) {
            return false;
        }
        switch (op) {
            case OpCode::OP_JUMP:
            case OpCode::OP_JUMP_IF_FALSE:
            case OpCode::OP_JUMP_IF_TRUE:
            case OpCode::OP_JUMP_IF_NONE:
                return false;
            default:
                return static_cast<uint8_t>(op) < 0x80;
        }
    }

    static std::vector<uint8_t> compact_code(const ByteArray &code) {
        std::vector<uint8_t> result;
        result.reserve(code.size());
        for (size_t pc = 0; pc < code.size();) {
            OpCode op = (OpCode)code[pc];
            size_t size = decoded_instruction_size(code, pc);
            if (size == 0) {
                result.insert(result.end(), code.begin() + pc, code.end());
                break;
            }
            if (has_compact_u16_alias(op) && code[pc + 1] == 0) {
                result.push_back(static_cast<uint8_t>(op) | 0x80);
                result.push_back(code[pc + 2]);
            } else {
                result.insert(result.end(), code.begin() + pc, code.begin() + pc + size);
            }
            pc += size;
        }
        return result;
    }

    static void expand_code(const uint8_t *data, size_t length, ByteArray &code) {
        code.clear();
        code.reserve(length);
        for (size_t pc = 0; pc < length;) {
            uint8_t encoded_op = data[pc];
            OpCode alias_op = (OpCode)(encoded_op & 0x7f);
            if ((encoded_op & 0x80) != 0 && has_compact_u16_alias(alias_op)) {
                if (pc + 2 > length || code.size() > (1u << 26) - 3) {
                    throw DeserializeError{};
                }
                code.push_back(static_cast<uint8_t>(alias_op));
                code.push_back(0);
                code.push_back(data[pc + 1]);
                pc += 2;
                continue;
            }

            OpCode op = static_cast<OpCode>(encoded_op);
            const OpcodeInfo *info = opcode_info(op);
            if (!info) {
                code.push_back(encoded_op);
                pc++;
                continue;
            }
            size_t size = 1 + info->operand_size;
            if (size > length - pc) {
                throw DeserializeError{};
            }
            if (info->variable_size) {
                uint16_t capture_count = (static_cast<uint16_t>(data[pc + 3]) << 8) | data[pc + 4];
                size += static_cast<size_t>(capture_count) * 3;
                if (size > length - pc) {
                    throw DeserializeError{};
                }
            }
            if (code.size() > (1u << 26) - size) {
                throw DeserializeError{};
            }
            code.insert(code.end(), data + pc, data + pc + size);
            pc += size;
        }
    }

    // write helpers (little-endian)
    static void write_bytes(std::vector<uint8_t> &buf, const uint8_t *data, size_t len) {
        buf.insert(buf.end(), data, data + len);
    }

    static void write_u8(std::vector<uint8_t> &buf, uint8_t v) {
        buf.push_back(v);
    }

    static void write_i8(std::vector<uint8_t> &buf, int8_t v) {
        buf.push_back(static_cast<uint8_t>(v));
    }

    static void write_u16(std::vector<uint8_t> &buf, uint16_t v) {
        buf.push_back(v & 0xFF);
        buf.push_back((v >> 8) & 0xFF);
    }

    static void write_varuint(std::vector<uint8_t> &buf, uint64_t v) {
        do {
            uint8_t byte = static_cast<uint8_t>(v & 0x7f);
            v >>= 7;
            buf.push_back(v ? static_cast<uint8_t>(byte | 0x80) : byte);
        } while (v);
    }

    static void write_varint(std::vector<uint8_t> &buf, int64_t v) {
        uint64_t zigzag = v >= 0 ? static_cast<uint64_t>(v) << 1 : (static_cast<uint64_t>(~v) << 1) | 1;
        write_varuint(buf, zigzag);
    }

    static void write_u64(std::vector<uint8_t> &buf, uint64_t v) {
        for (int i = 0; i < 8; i++) {
            buf.push_back(v & 0xFF);
            v >>= 8;
        }
    }

    static void write_f64(std::vector<uint8_t> &buf, double v) {
        uint64_t u;
        memcpy(&u, &v, sizeof(u));
        for (int i = 0; i < 8; i++) {
            buf.push_back(u & 0xFF);
            u >>= 8;
        }
    }

    static void write_string(std::vector<uint8_t> &buf, const std::string &s) {
        write_varuint(buf, s.size());
        write_bytes(buf, reinterpret_cast<const uint8_t *>(s.data()), s.size());
    }

    // exception thrown when a .naric file is malformed or truncated.
    struct DeserializeError {};

    // read helpers (little-endian). All bounds-checked against `length`.
    static uint8_t read_u8(const uint8_t *data, size_t length, size_t &pos) {
        if (pos + 1 > length) {
            throw DeserializeError{};
        }
        return data[pos++];
    }

    static int8_t read_i8(const uint8_t *data, size_t length, size_t &pos) {
        if (pos + 1 > length) {
            throw DeserializeError{};
        }
        return static_cast<int8_t>(data[pos++]);
    }

    static uint16_t read_u16(const uint8_t *data, size_t length, size_t &pos) {
        if (pos + 2 > length) {
            throw DeserializeError{};
        }
        uint16_t v = data[pos] | (static_cast<uint16_t>(data[pos + 1]) << 8);
        pos += 2;
        return v;
    }

    static uint64_t read_varuint(const uint8_t *data, size_t length, size_t &pos) {
        uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            uint8_t byte = read_u8(data, length, pos);
            if (shift == 63 && (byte & 0xfe) != 0) {
                throw DeserializeError{};
            }
            value |= static_cast<uint64_t>(byte & 0x7f) << shift;
            if ((byte & 0x80) == 0) {
                return value;
            }
        }
        throw DeserializeError{};
    }

    static uint32_t read_varuint32(const uint8_t *data, size_t length, size_t &pos) {
        uint64_t value = read_varuint(data, length, pos);
        if (value > UINT32_MAX) {
            throw DeserializeError{};
        }
        return static_cast<uint32_t>(value);
    }

    static int64_t read_varint(const uint8_t *data, size_t length, size_t &pos) {
        uint64_t zigzag = read_varuint(data, length, pos);
        return (zigzag & 1) ? ~static_cast<int64_t>(zigzag >> 1) : static_cast<int64_t>(zigzag >> 1);
    }

    static uint64_t read_u64(const uint8_t *data, size_t length, size_t &pos) {
        if (pos + 8 > length) {
            throw DeserializeError{};
        }
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) {
            v |= static_cast<uint64_t>(data[pos++]) << (i * 8);
        }
        return v;
    }

    static double read_f64(const uint8_t *data, size_t length, size_t &pos) {
        if (pos + 8 > length) {
            throw DeserializeError{};
        }
        uint64_t u = 0;
        for (int i = 0; i < 8; i++) {
            u |= static_cast<uint64_t>(data[pos++]) << (i * 8);
        }
        double v;
        memcpy(&v, &u, sizeof(v));
        return v;
    }

    static std::string read_string(const uint8_t *data, size_t length, size_t &pos) {
        uint32_t len = read_varuint32(data, length, pos);
        // same overflow-safe formulation as the bytecode-length check.
        if (len > (1u << 24) || pos > length || len > length - pos) {
            throw DeserializeError{};
        }
        std::string s(reinterpret_cast<const char *>(data + pos), len);
        pos += len;
        return s;
    }
};

} // namespace bytecode
} // namespace nari
