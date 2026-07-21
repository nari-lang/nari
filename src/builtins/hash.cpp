// Cryptographic hash builtins that use mbedtls

#include "common.h"

#include <mbedtls/sha256.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace {

constexpr size_t kSha256DigestSize = 32;

std::string sha256_to_hex(const unsigned char (&digest)[kSha256DigestSize]) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(kSha256DigestSize * 2);
    for (size_t i = 0; i < kSha256DigestSize; ++i) {
        unsigned char b = digest[i];
        out[i * 2] = kHex[(b >> 4) & 0x0F];
        out[i * 2 + 1] = kHex[b & 0x0F];
    }
    return out;
}

} // namespace

Value ScriptRuntime::builtin_hash_sha256(const Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc != 1 || !argvals[0].is_string()) {
        runtime_fatal("Hash.sha256 expects a single string argument", call);
    }

    const std::string &input = argvals[0].get_string();

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);

    // Second arg = 0 selects SHA-256 (1 would select the legacy SHA-224).
    if (mbedtls_sha256_starts(&ctx, 0) != 0) {
        mbedtls_sha256_free(&ctx);
        runtime_fatal("Hash.sha256: mbedtls_sha256_starts failed", call);
    }

    if (!input.empty()) {
        if (mbedtls_sha256_update(&ctx, reinterpret_cast<const unsigned char *>(input.data()), input.size()) != 0) {
            mbedtls_sha256_free(&ctx);
            runtime_fatal("Hash.sha256: mbedtls_sha256_update failed", call);
        }
    }

    unsigned char digest[kSha256DigestSize];
    if (mbedtls_sha256_finish(&ctx, digest) != 0) {
        mbedtls_sha256_free(&ctx);
        runtime_fatal("Hash.sha256: mbedtls_sha256_finish failed", call);
    }

    mbedtls_sha256_free(&ctx);
    return Value::make_string(sha256_to_hex(digest));
}

Value ScriptRuntime::builtin_hash_sha256_file(const Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc != 1 || !argvals[0].is_string()) {
        runtime_fatal("Hash.sha256_file expects a single path string argument", call);
    }

    const std::string &path = argvals[0].get_string();

    // binary mode is used to avoid text-mode translation on Windows
    FILE *fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) {
        runtime_fatal("Hash.sha256_file: cannot open '" + path + "': " + std::strerror(errno), call);
    }

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);

    if (mbedtls_sha256_starts(&ctx, 0) != 0) {
        mbedtls_sha256_free(&ctx);
        std::fclose(fp);
        runtime_fatal("Hash.sha256_file: mbedtls_sha256_starts failed", call);
    }

    std::array<unsigned char, 64 * 1024> buf;
    while (true) {
        size_t n = std::fread(buf.data(), 1, buf.size(), fp);
        if (n > 0) {
            if (mbedtls_sha256_update(&ctx, buf.data(), n) != 0) {
                mbedtls_sha256_free(&ctx);
                std::fclose(fp);
                runtime_fatal("Hash.sha256_file: mbedtls_sha256_update failed", call);
            }
        }
        if (n < buf.size()) {
            if (std::ferror(fp)) {
                int saved = errno;
                mbedtls_sha256_free(&ctx);
                std::fclose(fp);
                runtime_fatal("Hash.sha256_file: read error on '" + path + "': " + std::strerror(saved), call);
            }
            break; // EOF
        }
    }

    std::fclose(fp);

    unsigned char digest[kSha256DigestSize];
    if (mbedtls_sha256_finish(&ctx, digest) != 0) {
        mbedtls_sha256_free(&ctx);
        runtime_fatal("Hash.sha256_file: mbedtls_sha256_finish failed", call);
    }

    mbedtls_sha256_free(&ctx);
    return Value::make_string(sha256_to_hex(digest));
}
