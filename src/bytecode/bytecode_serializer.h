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
    version: 2 bytes   u16 (currently 6)
    flags:   2 bytes   u16 (reserved)

String table:
    count:  4 bytes  u32
    for each string:
        length: 4 bytes u32
        data:   N bytes string

Function table:
    count:  4 bytes  u32
    main_func_idx: 4 bytes u32
    for each function:
        name_length:  4 bytes u32
        name:  N bytes string
        param_count:  1 byte  u8
        capture_count:  1 byte  u8
        rest_param_idx: 1 byte  i8
        is_lambda:  1 byte  u8
        var_count:  4 bytes u32
        for each var_name:
            length: 4 bytes u32
            data:  N bytes string
        const_count:  4 bytes u32
        for each constant:
            type:  1 byte  u8 (0=none, 1=int, 2=float, 3=string, 4=function)
            data:  8 bytes (int64/double/u32 + pad)
            code_length:  4 bytes u32
            code:  N bytes
*/

static constexpr uint8_t MAGIC[4] = { 'N', 'A', 'R', 'I' };
static constexpr uint16_t FORMAT_VERSION = 6;

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
        write_u32(buf, static_cast<uint32_t>(chunk.strings.size()));
        for (const auto &s : chunk.strings) {
            write_string(buf, s);
        }

        // function table
        write_u32(buf, static_cast<uint32_t>(chunk.functions.size()));
        write_u32(buf, chunk.main_func_idx);

        for (const auto &func : chunk.functions) {
            // name
            write_string(buf, func.name);

            // metadata
            write_u8(buf, func.param_count);
            write_u8(buf, func.capture_count);
            write_i8(buf, func.rest_param_index);
            write_u8(buf, func.is_lambda ? 1 : 0);
            write_u8(buf, func.strict_mode ? 1 : 0);

            // var_names
            write_u32(buf, static_cast<uint32_t>(func.var_names.size()));
            for (const auto &vn : func.var_names) {
                write_string(buf, vn);
            }

            // constants
            write_u32(buf, static_cast<uint32_t>(func.constants.size()));
            for (const auto &c : func.constants) {
                write_u8(buf, static_cast<uint8_t>(c.type));
                switch (c.type) {
                    case Constant::Type::INT:
                        write_i64(buf, c.as_int);
                        break;
                    case Constant::Type::FLOAT:
                        write_f64(buf, c.as_float);
                        break;
                    case Constant::Type::STRING:
                        write_u32(buf, c.string_idx);
                        // pad to 8 bytes
                        write_u32(buf, 0);
                        break;
                    case Constant::Type::FUNCTION:
                        write_u32(buf, c.func_idx);
                        write_u32(buf, 0);
                        break;
                    case Constant::Type::NONE:
                    default:
                        // 8 bytes padding
                        write_i64(buf, 0);
                        break;
                }
            }

            // code
            write_u32(buf, static_cast<uint32_t>(func.code.size()));
            write_bytes(buf, func.code.data(), func.code.size());
        }

        // type declarations (for FFI struct support)
        write_u32(buf, static_cast<uint32_t>(chunk.types.size()));
        for (const auto &type : chunk.types) {
            write_string(buf, type.name);
            write_u8(buf, type.is_union ? 1 : 0);
            write_string(buf, type.alias_target); // empty string if not an alias
            write_u32(buf, static_cast<uint32_t>(type.fields.size()));
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
            uint32_t string_count = read_u32(data, length, pos);
            if (string_count > MAX_COUNT) {
                throw DeserializeError{};
            }
            chunk->strings.resize(string_count);
            for (uint32_t i = 0; i < string_count; i++) {
                chunk->strings[i] = read_string(data, length, pos);
            }

            // function table
            uint32_t func_count = read_u32(data, length, pos);
            if (func_count > MAX_COUNT) {
                throw DeserializeError{};
            }
            chunk->main_func_idx = read_u32(data, length, pos);
            chunk->functions.resize(func_count);

            for (uint32_t fi = 0; fi < func_count; fi++) {
                FunctionMeta &func = chunk->functions[fi];

                // name
                func.name = read_string(data, length, pos);

                // metadata
                func.param_count = read_u8(data, length, pos);
                func.capture_count = read_u8(data, length, pos);
                func.rest_param_index = read_i8(data, length, pos);
                func.is_lambda = read_u8(data, length, pos) != 0;
                func.strict_mode = read_u8(data, length, pos) != 0;

                // var_names
                uint32_t var_count = read_u32(data, length, pos);
                if (var_count > MAX_COUNT) {
                    throw DeserializeError{};
                }
                func.var_names.resize(var_count);
                for (uint32_t vi = 0; vi < var_count; vi++) {
                    func.var_names[vi] = read_string(data, length, pos);
                }

                // constants
                uint32_t const_count = read_u32(data, length, pos);
                if (const_count > MAX_COUNT) {
                    throw DeserializeError{};
                }
                func.constants.resize(const_count);
                for (uint32_t ci = 0; ci < const_count; ci++) {
                    Constant &c = func.constants[ci];
                    c.type = static_cast<Constant::Type>(read_u8(data, length, pos));
                    switch (c.type) {
                        case Constant::Type::INT:
                            c.as_int = read_i64(data, length, pos);
                            break;
                        case Constant::Type::FLOAT:
                            c.as_float = read_f64(data, length, pos);
                            break;
                        case Constant::Type::STRING:
                            c.string_idx = read_u32(data, length, pos);
                            read_u32(data, length, pos); // skip padding
                            if (c.string_idx >= chunk->strings.size()) {
                                throw DeserializeError{};
                            }
                            break;
                        case Constant::Type::FUNCTION:
                            c.func_idx = read_u32(data, length, pos);
                            read_u32(data, length, pos); // skip padding
                            break;
                        case Constant::Type::NONE:
                        default:
                            read_i64(data, length, pos); // skip padding
                            break;
                    }
                }

                // code
                uint32_t code_len = read_u32(data, length, pos);
                // Use `code_len > length - pos` form to avoid potential
                // size_t wrap on 32-bit hosts. The (1<<26) ceiling caps the
                // allocation regardless.
                if (code_len > (1u << 26) || pos > length ||
                    code_len > length - pos) {
                    throw DeserializeError{};
                }
                func.code.resize(code_len);
                if (code_len > 0) {
                    memcpy(func.code.data(), data + pos, code_len);
                    pos += code_len;
                }
            }

            // type declarations (for FFI struct support)
            if (pos < length) {
                uint32_t type_count = read_u32(data, length, pos);
                if (type_count > MAX_COUNT) {
                    throw DeserializeError{};
                }
                chunk->types.resize(type_count);
                for (uint32_t ti = 0; ti < type_count; ti++) {
                    TypeInfo &type = chunk->types[ti];
                    type.name = read_string(data, length, pos);
                    type.is_union = read_u8(data, length, pos) != 0;
                    type.alias_target = read_string(data, length, pos);
                    uint32_t field_count = read_u32(data, length, pos);
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

        // Semantic pass: verify every opcode's operands reference valid
        // constants, strings, locals, functions, and jump targets. A well-formed
        // file can still contain crafted opcodes that would OOB at dispatch.
        if (!BytecodeVerifier::verify(*chunk)) {
            return nullptr;
        }

        // Transfer ownership to the caller (matches the pre-existing `Chunk*` return contract).
        return chunk.release();
    }

  private:
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

    static void write_u32(std::vector<uint8_t> &buf, uint32_t v) {
        buf.push_back(v & 0xFF);
        buf.push_back((v >> 8) & 0xFF);
        buf.push_back((v >> 16) & 0xFF);
        buf.push_back((v >> 24) & 0xFF);
    }

    static void write_i64(std::vector<uint8_t> &buf, int64_t v) {
        uint64_t u = static_cast<uint64_t>(v);
        for (int i = 0; i < 8; i++) {
            buf.push_back(u & 0xFF);
            u >>= 8;
        }
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
        write_u32(buf, static_cast<uint32_t>(s.size()));
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

    static uint32_t read_u32(const uint8_t *data, size_t length, size_t &pos) {
        if (pos + 4 > length) {
            throw DeserializeError{};
        }
        uint32_t v = data[pos] |
                     (static_cast<uint32_t>(data[pos + 1]) << 8) |
                     (static_cast<uint32_t>(data[pos + 2]) << 16) |
                     (static_cast<uint32_t>(data[pos + 3]) << 24);
        pos += 4;
        return v;
    }

    static int64_t read_i64(const uint8_t *data, size_t length, size_t &pos) {
        if (pos + 8 > length) {
            throw DeserializeError{};
        }
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) {
            v |= static_cast<uint64_t>(data[pos++]) << (i * 8);
        }
        return static_cast<int64_t>(v);
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
        uint32_t len = read_u32(data, length, pos);
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
