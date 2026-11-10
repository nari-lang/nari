#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <system_error>

namespace nari {

// Stream helpers leave the stream open. Reads replace the output and clear it
// on failure; writes flush buffered data before reporting success.
bool read_stream(FILE *stream, std::string &output, std::error_code &error, const std::atomic<bool> *cancel = nullptr);
bool write_stream(FILE *stream, const void *data, size_t size, std::error_code &error, const std::atomic<bool> *cancel = nullptr);
bool seek_file(FILE *stream, int64_t offset, std::error_code &error);
bool resize_file(FILE *stream, int64_t size, std::error_code &error);
// Clears the owner's pointer even when close fails; a stream cannot be reused
// or closed a second time after fclose has returned an error.
bool close_file(FILE *&stream, std::error_code &error);

// Files are opened in binary mode. Success includes closing the file without
// error; an empty file is a successful read of an empty string.
bool read_file(const std::string &path, std::string &output, std::error_code &error, const std::atomic<bool> *cancel = nullptr);
bool write_file(
    const std::string &path, const void *data, size_t size, std::error_code &error, bool append = false,
    const std::atomic<bool> *cancel = nullptr
);

} // namespace nari
