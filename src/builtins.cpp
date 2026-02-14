#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "runtime.h"
#ifndef NO_OPENSSL
#include "io.h"
#endif
#ifndef NO_FFI
#include "ffi.h"
#endif
#include "parser_api.h"

#ifdef _WIN32
#include "win_funcs.h"
#endif

#ifndef NO_FFI
namespace {

bool utf8_to_utf16(const std::string &input, std::u16string &output) {
  output.clear();

#ifdef _WIN32
  if (input.empty()) {
    return true;
  }

  int required =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.c_str(),
                          static_cast<int>(input.size()), nullptr, 0);

  if (required <= 0) {
    return false;
  }

  output.resize(static_cast<size_t>(required));

  int converted =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.c_str(),
                          static_cast<int>(input.size()),
                          reinterpret_cast<wchar_t *>(output.data()), required);

  return converted == required;
#else
  output.reserve(input.size());

  size_t i = 0;
  while (i < input.size()) {
    uint32_t codepoint = 0;
    unsigned char c = static_cast<unsigned char>(input[i]);

    if ((c & 0x80u) == 0) {
      codepoint = c;
      i += 1;
    } else if ((c & 0xE0u) == 0xC0u) {
      if (i + 1 >= input.size())
        return false;
      unsigned char c1 = static_cast<unsigned char>(input[i + 1]);
      if ((c1 & 0xC0u) != 0x80u)
        return false;
      codepoint = ((c & 0x1Fu) << 6) | (c1 & 0x3Fu);
      if (codepoint < 0x80u)
        return false;
      i += 2;
    } else if ((c & 0xF0u) == 0xE0u) {
      if (i + 2 >= input.size())
        return false;
      unsigned char c1 = static_cast<unsigned char>(input[i + 1]);
      unsigned char c2 = static_cast<unsigned char>(input[i + 2]);
      if ((c1 & 0xC0u) != 0x80u || (c2 & 0xC0u) != 0x80u)
        return false;
      codepoint = ((c & 0x0Fu) << 12) | ((c1 & 0x3Fu) << 6) | (c2 & 0x3Fu);
      if (codepoint < 0x800u)
        return false;
      if (codepoint >= 0xD800u && codepoint <= 0xDFFFu)
        return false;
      i += 3;
    } else if ((c & 0xF8u) == 0xF0u) {
      if (i + 3 >= input.size())
        return false;
      unsigned char c1 = static_cast<unsigned char>(input[i + 1]);
      unsigned char c2 = static_cast<unsigned char>(input[i + 2]);
      unsigned char c3 = static_cast<unsigned char>(input[i + 3]);
      if ((c1 & 0xC0u) != 0x80u || (c2 & 0xC0u) != 0x80u ||
          (c3 & 0xC0u) != 0x80u)
        return false;
      codepoint = ((c & 0x07u) << 18) | ((c1 & 0x3Fu) << 12) |
                  ((c2 & 0x3Fu) << 6) | (c3 & 0x3Fu);
      if (codepoint < 0x10000u || codepoint > 0x10FFFFu)
        return false;
      i += 4;
    } else {
      return false;
    }

    if (codepoint <= 0xFFFFu) {
      output.push_back(static_cast<char16_t>(codepoint));
    } else {
      codepoint -= 0x10000u;
      char16_t high = static_cast<char16_t>(0xD800u + (codepoint >> 10));
      char16_t low = static_cast<char16_t>(0xDC00u + (codepoint & 0x3FFu));
      output.push_back(high);
      output.push_back(low);
    }
  }

  return true;
#endif
}

bool utf16_to_utf8(const std::u16string &input, std::string &output) {
  output.clear();

#ifdef _WIN32
  if (input.empty()) {
    return true;
  }

  int required = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS,
      reinterpret_cast<const wchar_t *>(input.data()),
      static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);

  if (required <= 0) {
    return false;
  }

  output.resize(static_cast<size_t>(required));

  int converted =
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                          reinterpret_cast<const wchar_t *>(input.data()),
                          static_cast<int>(input.size()), output.data(),
                          required, nullptr, nullptr);

  return converted == required;
#else
  output.reserve(input.size());

  for (size_t i = 0; i < input.size(); ++i) {
    uint32_t codepoint = static_cast<uint16_t>(input[i]);

    if (codepoint >= 0xD800u && codepoint <= 0xDBFFu) {
      if (i + 1 >= input.size())
        return false;
      uint32_t low = static_cast<uint16_t>(input[i + 1]);
      if (low < 0xDC00u || low > 0xDFFFu)
        return false;
      codepoint = ((codepoint - 0xD800u) << 10) + (low - 0xDC00u) + 0x10000u;
      ++i;
    } else if (codepoint >= 0xDC00u && codepoint <= 0xDFFFu) {
      return false;
    }

    if (codepoint <= 0x7Fu) {
      output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FFu) {
      output.push_back(static_cast<char>(0xC0u | ((codepoint >> 6) & 0x1Fu)));
      output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else if (codepoint <= 0xFFFFu) {
      output.push_back(static_cast<char>(0xE0u | ((codepoint >> 12) & 0x0Fu)));
      output.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
      output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else {
      output.push_back(static_cast<char>(0xF0u | ((codepoint >> 18) & 0x07u)));
      output.push_back(static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu)));
      output.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
      output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    }
  }

  return true;
#endif
}

} // namespace
#endif

// this is where builtin functions live (functions that call back into native
// code)

Value ScriptRuntime::builtin_print(const std::vector<Value> &argvals,
                                   const nari::CallExpr *) {
  for (size_t i = 0; i < argvals.size(); ++i) {
    if (i)
      printf(" ");
    printf("%s", argvals[i].to_string().c_str());
  }
  printf("\n");
  fflush(stdout);
  return Value::none();
}

Value ScriptRuntime::builtin_setTimeout(const std::vector<Value> &argvals,
                                        const nari::CallExpr *) {
  if (argvals.size() >= 2) {
    Value callback_val = argvals[0];
    int64_t delay_ms = 0;

    if (argvals[1].is_int()) {
      delay_ms = argvals[1].get_int();
    } else if (argvals[1].is_float()) {
      delay_ms = static_cast<int64_t>(argvals[1].get_float());
    }

    auto io_op = std::make_shared<IOOperation>(IOOperation::Type::Timer);
    io_op->timer_ms = delay_ms;

    std::string callback_name;
    if (callback_val.is_function()) {
      callback_name = callback_val.get_function().name;
    }

    io_op->callback = [this, callback_name]() {
      if (!callback_name.empty()) {
        auto it = functions.find(callback_name);
        if (it != functions.end()) {
          call_user_function(it->second.get(), {});
        }
      }
    };

    if (io_pool)
      io_pool->submit(io_op);
  }
  return Value::none();
}

Value ScriptRuntime::builtin_setInterval(const std::vector<Value> &argvals,
                                         const nari::CallExpr *) {
  if (argvals.size() >= 2) {
    Value callback_val = argvals[0];
    int64_t delay_ms = 0;

    if (argvals[1].is_int()) {
      delay_ms = argvals[1].get_int();
    } else if (argvals[1].is_float()) {
      delay_ms = static_cast<int64_t>(argvals[1].get_float());
    }

    std::string callback_name;
    if (callback_val.is_function()) {
      callback_name = callback_val.get_function().name;
    }

    IntervalData interval;
    interval.id = next_interval_id++;
    interval.callback_name = callback_name;
    interval.interval_ms = delay_ms;
    interval.next_fire =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);

    active_intervals[interval.id] = interval;

    return Value::make_int(interval.id);
  }
  return Value::none();
}

Value ScriptRuntime::builtin_clearInterval(const std::vector<Value> &argvals,
                                           const nari::CallExpr *) {
  if (!argvals.empty() && argvals[0].is_int()) {
    int64_t id = argvals[0].get_int();
    active_intervals.erase(id);
  }
  return Value::none();
}

// Math builtins
Value ScriptRuntime::builtin_math_sqrt(const std::vector<Value> &argvals,
                                       const nari::CallExpr *) {
  if (!argvals.empty()) {
    double num = 0.0;
    if (argvals[0].is_int()) {
      num = static_cast<double>(argvals[0].get_int());
    } else if (argvals[0].is_float()) {
      num = argvals[0].get_float();
    }
    if (num >= 0.0) {
      return Value::make_float(std::sqrt(num));
    }
  }
  return Value::none();
}

Value ScriptRuntime::builtin_math_rand(const std::vector<Value> &argvals,
                                       const nari::CallExpr *) {
  static std::random_device rd;
  static std::mt19937 gen(rd());
  static std::uniform_real_distribution<> dis(0.0, 1.0);
  return Value::make_float(dis(gen));
};

// File system builtins
Value ScriptRuntime::builtin_fs_readFile(const std::vector<Value> &argvals,
                                         const nari::CallExpr *) {
  if (argvals.size() >= 1) {
    std::string path = argvals[0].to_string();

    auto io_op = std::make_shared<FileOperation>(IOOperation::Type::FileRead);
    io_op->file_path = path;

    auto handle = Value::make_handle_ptr();
    handle->state = HandleData::Running;

    io_op->callback = [handle, io_op]() {
      handle->end_time = std::chrono::steady_clock::now();
      if (io_op->success) {
        if (io_op->result.type == FileOperation::Result::Type::String) {
          handle->result = Value::make_string(io_op->result.str_value);
        } else if (io_op->result.type == FileOperation::Result::Type::Bool) {
          handle->result = Value::make_bool(io_op->result.bool_value);
        }
        handle->state = HandleData::Completed;
      } else {
        handle->error = Value::make_string(io_op->error_msg);
        handle->state = HandleData::Failed;
      }
    };

    if (io_pool)
      io_pool->submit(io_op);
    return Value::make_handle(handle);
  }
  return Value::none();
}

Value ScriptRuntime::builtin_fs_writeFile(const std::vector<Value> &argvals,
                                          const nari::CallExpr *) {
  if (argvals.size() >= 2) {
    std::string path = argvals[0].to_string();
    std::string content = argvals[1].to_string();

    auto io_op = std::make_shared<FileOperation>(IOOperation::Type::FileWrite);
    io_op->file_path = path;
    io_op->file_content = content;

    auto handle = Value::make_handle_ptr();
    handle->state = HandleData::Running;

    io_op->callback = [handle, io_op]() {
      handle->end_time = std::chrono::steady_clock::now();
      if (io_op->success) {
        handle->result = Value::none();
        handle->state = HandleData::Completed;
      } else {
        handle->error = Value::make_string(io_op->error_msg);
        handle->state = HandleData::Failed;
      }
    };

    if (io_pool)
      io_pool->submit(io_op);
    return Value::make_handle(handle);
  }
  return Value::none();
}

Value ScriptRuntime::builtin_fs_appendFile(const std::vector<Value> &argvals,
                                           const nari::CallExpr *) {
  if (argvals.size() >= 2) {
    std::string path = argvals[0].to_string();
    std::string content = argvals[1].to_string();

    auto io_op = std::make_shared<FileOperation>(IOOperation::Type::FileAppend);
    io_op->file_path = path;
    io_op->file_content = content;

    auto handle = Value::make_handle_ptr();
    handle->state = HandleData::Running;

    io_op->callback = [handle, io_op]() {
      handle->end_time = std::chrono::steady_clock::now();
      if (io_op->success) {
        handle->result = Value::none();
        handle->state = HandleData::Completed;
      } else {
        handle->error = Value::make_string(io_op->error_msg);
        handle->state = HandleData::Failed;
      }
    };

    if (io_pool)
      io_pool->submit(io_op);
    return Value::make_handle(handle);
  }
  return Value::none();
}

Value ScriptRuntime::builtin_fs_fileExists(const std::vector<Value> &argvals,
                                           const nari::CallExpr *) {
  if (argvals.size() >= 1) {
    std::string path = argvals[0].to_string();

    auto io_op = std::make_shared<FileOperation>(IOOperation::Type::FileExists);
    io_op->file_path = path;

    auto handle = Value::make_handle_ptr();
    handle->state = HandleData::Running;

    io_op->callback = [handle, io_op]() {
      handle->end_time = std::chrono::steady_clock::now();
      handle->result = Value::make_bool(io_op->result_bool);
      handle->state = HandleData::Completed;
    };

    if (io_pool)
      io_pool->submit(io_op);
    return Value::make_handle(handle);
  }
  return Value::none();
}

Value ScriptRuntime::builtin_fs_isDirectory(const std::vector<Value> &argvals,
                                            const nari::CallExpr *) {
  if (argvals.size() >= 1) {
    std::string path = argvals[0].to_string();
    std::error_code ec;
    bool is_dir = std::filesystem::is_directory(path, ec);
    return Value::make_bool(!ec && is_dir);
  }
  return Value::make_bool(false);
}

Value ScriptRuntime::builtin_fs_deleteFile(const std::vector<Value> &argvals,
                                           const nari::CallExpr *) {
  if (argvals.size() >= 1) {
    std::string path = argvals[0].to_string();

    auto io_op = std::make_shared<FileOperation>(IOOperation::Type::FileDelete);
    io_op->file_path = path;

    auto handle = Value::make_handle_ptr();
    handle->state = HandleData::Running;

    io_op->callback = [handle, io_op]() {
      handle->end_time = std::chrono::steady_clock::now();
      if (io_op->success) {
        handle->result = Value::make_bool(io_op->result_bool);
        handle->state = HandleData::Completed;
      } else {
        handle->error = Value::make_string(io_op->error_msg);
        handle->state = HandleData::Failed;
      }
    };

    if (io_pool)
      io_pool->submit(io_op);
    return Value::make_handle(handle);
  }
  return Value::none();
}

Value ScriptRuntime::builtin_fs_listDir(const std::vector<Value> &argvals,
                                        const nari::CallExpr *) {
  if (argvals.size() >= 1) {
    std::string path = argvals[0].to_string();

    auto io_op = std::make_shared<FileOperation>(IOOperation::Type::ListDir);
    io_op->file_path = path;

    auto handle = Value::make_handle_ptr();
    handle->state = HandleData::Running;

    io_op->callback = [handle, io_op]() {
      handle->end_time = std::chrono::steady_clock::now();
      if (io_op->success) {
        if (io_op->result.type == FileOperation::Result::Type::String) {
          handle->result = Value::make_string(io_op->result.str_value);
        } else if (io_op->result.type == FileOperation::Result::Type::Array) {
          std::vector<Value> arr;
          for (const auto &item : io_op->result.array_value) {
            arr.push_back(Value::make_string(item));
          }
          handle->result = Value::make_array(std::move(arr));
        } else if (io_op->result.type == FileOperation::Result::Type::Bool) {
          handle->result = Value::make_bool(io_op->result.bool_value);
        }
        handle->state = HandleData::Completed;
      } else {
        handle->error = Value::make_string(io_op->error_msg);
        handle->state = HandleData::Failed;
      }
    };

    if (io_pool)
      io_pool->submit(io_op);
    return Value::make_handle(handle);
  }
  return Value::none();
}

// Network builtins
Value ScriptRuntime::builtin_net_createServer(const std::vector<Value> &argvals,
                                              const nari::CallExpr *) {
  if (argvals.size() >= 2) {
    int port = static_cast<int>(argvals[0].as_number());
    Value callback_val = argvals[1];
    std::string callback_name =
        callback_val.is_function() ? callback_val.get_function().name : "";

    auto listen_op =
        std::make_shared<TcpOperation>(IOOperation::Type::TcpListen);
    listen_op->port = port;
    listen_op->callback = [this, callback_name, listen_op, port]() {
      if (!listen_op->success) {
        fprintf(stderr, "Failed to create server: %s\n",
                listen_op->error_msg.c_str());
        return;
      }

      int server_fd = listen_op->socket_fd;

#ifndef NO_THREADS
      // Register server socket for shutdown
      {
        std::lock_guard<std::mutex> lock(server_sockets_mutex);
        server_sockets.push_back(server_fd);
      }

      auto accept_loop = [this, server_fd, callback_name]() {
        while (!Runtime::g_shutdown_requested.load()) {
          auto accept_op =
              std::make_shared<TcpOperation>(IOOperation::Type::TcpAccept);
          accept_op->socket_fd = server_fd;

          accept_op->callback = [this, callback_name, accept_op]() {
            if (!accept_op->success) {
              return;
            }

            auto conn_obj = Value::make_object();
            auto &conn_map = conn_obj.get_object();
            if (conn_map) {
              (*conn_map)["fd"] = Value::make_int(accept_op->client_fd);
              (*conn_map)["ip"] = Value::make_string(accept_op->client_ip);
              (*conn_map)["port"] = Value::make_int(accept_op->client_port);
              (*conn_map)["read"] = Value::make_function("__net_conn_read");
              (*conn_map)["write"] = Value::make_function("__net_conn_write");
              (*conn_map)["close"] = Value::make_function("__net_conn_close");
            }

            if (!callback_name.empty()) {
              auto it = functions.find(callback_name);
              if (it != functions.end()) {
                call_user_function(it->second.get(), {conn_obj});
              }
            }
          };

          if (io_pool) {
            io_pool->submit(accept_op);
          }

          while (!accept_op->completed &&
                 !Runtime::g_shutdown_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
          }

          if (Runtime::g_shutdown_requested.load()) {
            break;
          }
        }

        // dtor handles closing
      };

      std::lock_guard<std::mutex> lock(accept_threads_mutex);
      accept_threads.emplace_back(accept_loop);
#else
      // No threading support - can't run server
      (void)server_fd;
      (void)callback_name;
#endif
    };

    if (io_pool)
      io_pool->submit(listen_op);
  }
  return Value::none();
}

Value ScriptRuntime::builtin_net_conn_read(const std::vector<Value> &argvals,
                                           const nari::CallExpr *) {
  if (argvals.size() >= 2 && argvals[0].is_object()) {
    auto &obj = argvals[0].get_object();
    if (!obj)
      return Value::none();

    auto fd_it = obj->find("fd");
    if (fd_it != obj->end() && fd_it->second.is_int()) {
      int fd = static_cast<int>(fd_it->second.get_int());
      Value callback_val = argvals[1];
      std::string callback_name =
          callback_val.is_function() ? callback_val.get_function().name : "";

      auto read_op = std::make_shared<TcpOperation>(IOOperation::Type::TcpRead);
      read_op->socket_fd = fd;
      read_op->callback = [this, callback_name, read_op]() {
        if (!callback_name.empty()) {
          auto it = functions.find(callback_name);
          if (it != functions.end()) {
            if (read_op->success) {
              call_user_function(
                  it->second.get(),
                  {Value::none(), Value::make_string(read_op->result_string)});
            } else {
              call_user_function(
                  it->second.get(),
                  {Value::make_string(read_op->error_msg), Value::none()});
            }
          }
        }
      };

      if (io_pool)
        io_pool->submit(read_op);
    }
  }
  return Value::none();
}

Value ScriptRuntime::builtin_net_conn_write(const std::vector<Value> &argvals,
                                            const nari::CallExpr *) {
  if (argvals.size() >= 3 && argvals[0].is_object()) {
    auto &obj = argvals[0].get_object();
    if (!obj)
      return Value::none();

    auto fd_it = obj->find("fd");
    if (fd_it != obj->end() && fd_it->second.is_int()) {
      int fd = static_cast<int>(fd_it->second.get_int());
      std::string data = argvals[1].to_string();
      Value callback_val = argvals[2];
      std::string callback_name =
          callback_val.is_function() ? callback_val.get_function().name : "";

      auto write_op =
          std::make_shared<TcpOperation>(IOOperation::Type::TcpWrite);
      write_op->socket_fd = fd;
      write_op->data = data;
      write_op->callback = [this, callback_name, write_op]() {
        if (!callback_name.empty()) {
          auto it = functions.find(callback_name);
          if (it != functions.end()) {
            if (write_op->success) {
              call_user_function(it->second.get(), {Value::none()});
            } else {
              call_user_function(it->second.get(),
                                 {Value::make_string(write_op->error_msg)});
            }
          }
        }
      };

      if (io_pool)
        io_pool->submit(write_op);
    }
  }
  return Value::none();
}

Value ScriptRuntime::builtin_net_conn_close(const std::vector<Value> &argvals,
                                            const nari::CallExpr *) {
  if (argvals.size() >= 1 && argvals[0].is_object()) {
    auto &obj = argvals[0].get_object();
    if (!obj)
      return Value::none();

    auto fd_it = obj->find("fd");
    if (fd_it != obj->end() && fd_it->second.is_int()) {
      int fd = static_cast<int>(fd_it->second.get_int());

      auto close_op =
          std::make_shared<TcpOperation>(IOOperation::Type::TcpClose);
      close_op->socket_fd = fd;
      close_op->callback = []() {};

      if (io_pool)
        io_pool->submit(close_op);
    }
  }
  return Value::none();
}

#ifndef NO_OPENSSL
// HTTP builtins
Value ScriptRuntime::builtin_http_get(const std::vector<Value> &argvals,
                                      const nari::CallExpr *) {
  if (argvals.size() >= 1) {
    std::string url = argvals[0].to_string();

    std::string host;
    int port = 80;
    std::string path = "/";
    size_t proto_end = url.find("://");

    if (proto_end != std::string::npos) {
      url = url.substr(proto_end + 3);
    }

    size_t path_start = url.find('/');
    std::string host_port;

    if (path_start != std::string::npos) {
      host_port = url.substr(0, path_start);
      path = url.substr(path_start);
    } else {
      host_port = url;
    }

    size_t port_start = host_port.find(':');

    if (port_start != std::string::npos) {
      host = host_port.substr(0, port_start);
      port = std::stoi(host_port.substr(port_start + 1));
    } else {
      host = host_port;
    }

    auto http_op = std::make_shared<HttpOperation>();
    http_op->host = host;
    http_op->port = port;
    http_op->url_path = path;
    http_op->method = "GET";

    auto handle = Value::make_handle_ptr();
    handle->state = HandleData::Running;

    http_op->callback = [handle, http_op]() {
      handle->end_time = std::chrono::steady_clock::now();
      if (http_op->success) {
        auto response = Value::make_object();
        auto &resp_obj = response.get_object();
        if (resp_obj) {
          (*resp_obj)["statusCode"] = Value::make_int(http_op->status_code);
          (*resp_obj)["body"] = Value::make_string(http_op->result_string);

          auto headers = Value::make_object();
          auto &headers_obj = headers.get_object();
          if (headers_obj) {
            for (const auto &[key, value] : http_op->response_headers) {
              (*headers_obj)[key] = Value::make_string(value);
            }
          }
          (*resp_obj)["headers"] = headers;
        }

        handle->result = response;
        handle->state = HandleData::Completed;
      } else {
        handle->error = Value::make_string(http_op->error_msg);
        handle->state = HandleData::Failed;
      }
    };

    if (io_pool)
      io_pool->submit(http_op);
    return Value::make_handle(handle);
  }
  return Value::none();
}

Value ScriptRuntime::builtin_http_request(const std::vector<Value> &argvals,
                                          const nari::CallExpr *) {
  if (argvals.size() >= 2 && argvals[0].is_object()) {
    Value callback_val = argvals[1];
    std::string callback_name =
        callback_val.is_function() ? callback_val.get_function().name : "";

    auto &opts_ptr = argvals[0].get_object();
    if (!opts_ptr)
      return Value::none();

    auto opts = opts_ptr.get();
    std::string url;
    std::string method = "GET";
    std::string body;
    std::map<std::string, std::string> headers;

    auto url_it = opts->find("url");
    if (url_it != opts->end()) {
      url = url_it->second.to_string();
    }

    auto method_it = opts->find("method");
    if (method_it != opts->end()) {
      method = method_it->second.to_string();
    }

    auto body_it = opts->find("body");
    if (body_it != opts->end()) {
      body = body_it->second.to_string();
    }

    auto headers_it = opts->find("headers");
    if (headers_it != opts->end() && headers_it->second.is_object()) {
      auto &headers_obj = headers_it->second.get_object();
      if (headers_obj) {
        for (const auto &[key, value] : *headers_obj) {
          headers[key] = value.to_string();
        }
      }
    }

    std::string host;
    int port = 80;
    std::string path = "/";

    size_t proto_end = url.find("://");
    if (proto_end != std::string::npos) {
      url = url.substr(proto_end + 3);
    }

    size_t path_start = url.find('/');
    std::string host_port;
    if (path_start != std::string::npos) {
      host_port = url.substr(0, path_start);
      path = url.substr(path_start);
    } else {
      host_port = url;
    }

    size_t port_start = host_port.find(':');
    if (port_start != std::string::npos) {
      host = host_port.substr(0, port_start);
      port = std::stoi(host_port.substr(port_start + 1));
    } else {
      host = host_port;
    }

    auto http_op = std::make_shared<HttpOperation>();
    http_op->host = host;
    http_op->port = port;
    http_op->url_path = path;
    http_op->method = method;
    http_op->body = body;
    http_op->headers = headers;

    http_op->callback = [this, callback_name, http_op]() {
      if (!callback_name.empty()) {
        auto it = functions.find(callback_name);
        if (it != functions.end()) {
          if (http_op->success) {
            auto response = Value::make_object();
            auto &resp_obj = response.get_object();
            if (resp_obj) {
              (*resp_obj)["statusCode"] = Value::make_int(http_op->status_code);
              (*resp_obj)["body"] = Value::make_string(http_op->result_string);

              auto headers = Value::make_object();
              auto &headers_obj = headers.get_object();
              if (headers_obj) {
                for (const auto &[key, value] : http_op->response_headers) {
                  (*headers_obj)[key] = Value::make_string(value);
                }
              }
              (*resp_obj)["headers"] = headers;
            }

            call_user_function(it->second.get(), {Value::none(), response});
          } else {
            call_user_function(
                it->second.get(),
                {Value::make_string(http_op->error_msg), Value::none()});
          }
        }
      }
    };

    if (io_pool)
      io_pool->submit(http_op);
  }
  return Value::none();
}
#endif // NO_OPENSSL

// Array builtins
Value ScriptRuntime::builtin_push(const std::vector<Value> &argvals,
                                  const nari::CallExpr *) {
  if (argvals.size() >= 2 && argvals[0].is_array()) {
    // push mutates the array, so we need to cast away const
    auto &arr_ptr = const_cast<Value &>(argvals[0]).get_array();
    if (arr_ptr) {
      arr_ptr->push_back(argvals[1]);
    }
  }
  return Value::none();
}

Value ScriptRuntime::builtin_pop(const std::vector<Value> &argvals,
                                 const nari::CallExpr *) {
  if (!argvals.empty() && argvals[0].is_array()) {
    auto &arr_ptr = const_cast<Value &>(argvals[0]).get_array();
    if (arr_ptr && !arr_ptr->empty()) {
      Value last = arr_ptr->back();
      arr_ptr->pop_back();
      return last;
    }
  }
  return Value::none();
}

Value ScriptRuntime::builtin_length(const std::vector<Value> &argvals,
                                    const nari::CallExpr *) {
  if (!argvals.empty()) {
    if (argvals[0].is_array()) {
      auto &arr = argvals[0].get_array();
      return Value::make_int(arr ? arr->size() : 0);
    } else if (argvals[0].is_string()) {
      return Value::make_int(argvals[0].get_string().size());
    } else if (argvals[0].is_object()) {
      auto &obj = argvals[0].get_object();
      return Value::make_int(obj ? obj->size() : 0);
    }
  }
  return Value::make_int(0);
}

Value ScriptRuntime::builtin_slice(const std::vector<Value> &argvals,
                                   const nari::CallExpr *) {
  if (!argvals.empty() && argvals[0].is_array()) {
    auto &arr_ptr = argvals[0].get_array();
    if (!arr_ptr)
      return Value::make_array();

    int start = 0;
    if (argvals.size() > 1) {
      if (!argvals[1].is_int())
        return Value::make_array();
      start = argvals[1].get_int();
    }
    int end = arr_ptr->size();
    if (argvals.size() > 2) {
      if (!argvals[2].is_int())
        return Value::make_array();
      end = argvals[2].get_int();
    }

    if (start < 0)
      start = 0;
    if (end > arr_ptr->size())
      end = arr_ptr->size();
    if (start > end)
      start = end;

    std::vector<Value> result;
    for (int i = start; i < end; ++i) {
      result.push_back((*arr_ptr)[i]);
    }
    return Value::make_array(std::move(result));
  }
  return Value::make_array();
}

Value ScriptRuntime::builtin_concat(const std::vector<Value> &argvals,
                                    const nari::CallExpr *) {
  if (argvals.size() >= 2 && argvals[0].is_array() && argvals[1].is_array()) {
    auto &arr1 = argvals[0].get_array();
    auto &arr2 = argvals[1].get_array();
    if (!arr1 || !arr2)
      return Value::make_array();

    std::vector<Value> result = *arr1;
    result.insert(result.end(), arr2->begin(), arr2->end());
    return Value::make_array(std::move(result));
  }
  return Value::make_array();
}

// Object builtins
Value ScriptRuntime::builtin_keys(const std::vector<Value> &argvals,
                                  const nari::CallExpr *) {
  if (!argvals.empty() && argvals[0].is_object()) {
    auto &obj = argvals[0].get_object();
    if (!obj)
      return Value::make_array();

    std::vector<Value> result;
    for (const auto &[key, val] : *obj) {
      result.push_back(Value::make_string(key));
    }
    return Value::make_array(std::move(result));
  }
  return Value::make_array();
}

Value ScriptRuntime::builtin_values(const std::vector<Value> &argvals,
                                    const nari::CallExpr *) {
  if (!argvals.empty() && argvals[0].is_object()) {
    auto &obj = argvals[0].get_object();
    if (!obj)
      return Value::make_array();

    std::vector<Value> result;
    for (const auto &[key, val] : *obj) {
      result.push_back(val);
    }
    return Value::make_array(std::move(result));
  }
  return Value::make_array();
}

Value ScriptRuntime::builtin_hasKey(const std::vector<Value> &argvals,
                                    const nari::CallExpr *) {
  if (argvals.size() >= 2 && argvals[0].is_object()) {
    auto &obj = argvals[0].get_object();
    if (!obj)
      return Value::make_bool(false);

    std::string key = argvals[1].to_string();
    return Value::make_bool(obj->find(key) != obj->end());
  }
  return Value::make_bool(false);
}

Value ScriptRuntime::builtin_yield(const std::vector<Value> &,
                                   const nari::CallExpr *) {
  if (Runtime::g_shutdown_requested.load()) {
    if (io_pool) {
#ifndef NO_THREADS
      // Close all server sockets to unblock accept() calls
      {
        std::lock_guard<std::mutex> lock(server_sockets_mutex);
        for (int fd : server_sockets) {
          close(fd);
        }
        server_sockets.clear();
      }
#endif
      io_pool->shutdown();
    }
  }

  process_completed_io();

  int tasks_processed = 0;
  while (!task_queue.empty() && tasks_processed < 10) {
    HandlePtr next_task = task_queue.front();
    task_queue.pop();
    step_task(next_task);
    if (next_task->state == HandleData::Running) {
      task_queue.push(next_task);
    }
    tasks_processed++;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  process_completed_io();

  return Value::none();
}

Value ScriptRuntime::builtin_shutdown_requested(const std::vector<Value> &,
                                                const nari::CallExpr *) {
  return Value::make_bool(Runtime::g_shutdown_requested.load());
}

// String builtins
Value ScriptRuntime::builtin_substr(const std::vector<Value> &argvals,
                                    const nari::CallExpr *) {
  if (!argvals.empty()) {
    std::string str = argvals[0].to_string();
    int start = 0;
    if (argvals.size() > 1) {
      if (!argvals[1].is_int())
        return Value::make_string("");
      start = argvals[1].get_int();
    }
    int len = str.size() - start;
    if (argvals.size() > 2) {
      if (!argvals[2].is_int())
        return Value::make_string("");
      len = argvals[2].get_int();
    }

    if (start < 0)
      start = 0;
    if (start >= str.size())
      return Value::make_string("");
    if (len < 0)
      len = 0;
    if (start + len > str.size())
      len = str.size() - start;

    return Value::make_string(str.substr(start, len));
  }
  return Value::make_string("");
}

Value ScriptRuntime::builtin_indexOf(const std::vector<Value> &argvals,
                                     const nari::CallExpr *) {
  if (argvals.size() >= 2) {
    std::string str = argvals[0].to_string();
    std::string search = argvals[1].to_string();
    size_t pos = str.find(search);
    if (pos == std::string::npos) {
      return Value::make_int(-1);
    }
    return Value::make_int(pos);
  }
  return Value::make_int(-1);
}

Value ScriptRuntime::builtin_lastIndexOf(const std::vector<Value> &argvals,
                                         const nari::CallExpr *) {
  if (argvals.size() >= 2) {
    std::string str = argvals[0].to_string();
    std::string search = argvals[1].to_string();

    if (search.empty()) {
      if (argvals.size() > 2 &&
          (argvals[2].is_int() || argvals[2].is_float())) {
        int64_t fi = (argvals[2].is_int())
                         ? argvals[2].get_int()
                         : static_cast<int64_t>(argvals[2].get_float());
        if (fi < 0)
          return Value::make_int(-1);

        size_t pos = fi > str.size() ? str.size() : fi;
        return Value::make_int(pos);
      }
      return Value::make_int(str.size());
    }

    bool have_from = false;
    size_t from_pos = std::string::npos;
    if (argvals.size() > 2 && (argvals[2].is_int() || argvals[2].is_float())) {
      int64_t fi = (argvals[2].is_int())
                       ? argvals[2].get_int()
                       : static_cast<int64_t>(argvals[2].get_float());
      if (fi < 0) {
        return Value::make_int(-1);
      }
      if (str.empty()) {
        from_pos = 0;
      } else {
        size_t maxpos = (str.size() > 0) ? (str.size() - 1) : 0;
        from_pos = fi > maxpos ? maxpos : fi;
      }
      have_from = true;
    }

    size_t pos;
    if (have_from) {
      pos = str.rfind(search, from_pos);
    } else {
      pos = str.rfind(search);
    }

    if (pos == std::string::npos) {
      return Value::make_int(-1);
    }
    return Value::make_int(pos);
  }
  return Value::make_int(-1);
}

Value ScriptRuntime::builtin_split(const std::vector<Value> &argvals,
                                   const nari::CallExpr *) {
  if (argvals.size() >= 2) {
    std::string str = argvals[0].to_string();
    std::string delim = argvals[1].to_string();
    std::vector<Value> result;

    if (delim.empty()) {
      for (char c : str) {
        result.push_back(Value::make_string(std::string(1, c)));
      }
    } else {
      size_t start = 0;
      size_t pos;
      while ((pos = str.find(delim, start)) != std::string::npos) {
        result.push_back(Value::make_string(str.substr(start, pos - start)));
        start = pos + delim.size();
      }
      result.push_back(Value::make_string(str.substr(start)));
    }

    return Value::make_array(std::move(result));
  }
  return Value::make_array();
}

Value ScriptRuntime::builtin_replace(const std::vector<Value> &argvals,
                                     const nari::CallExpr *) {
  if (argvals.size() >= 3) {
    std::string str = argvals[0].to_string();
    std::string find = argvals[1].to_string();
    std::string replacement = argvals[2].to_string();

    size_t pos = str.find(find);
    if (pos != std::string::npos) {
      str.replace(pos, find.size(), replacement);
    }
    return Value::make_string(str);
  }
  return argvals.empty() ? Value::make_string("")
                         : Value::make_string(argvals[0].to_string());
}

Value ScriptRuntime::builtin_replaceAll(const std::vector<Value> &argvals,
                                        const nari::CallExpr *) {
  if (argvals.size() >= 3) {
    std::string str = argvals[0].to_string();
    std::string find = argvals[1].to_string();
    std::string replacement = argvals[2].to_string();

    if (!find.empty()) {
      size_t pos = 0;
      while ((pos = str.find(find, pos)) != std::string::npos) {
        str.replace(pos, find.size(), replacement);
        pos += replacement.size();
      }
    }
    return Value::make_string(str);
  }
  return argvals.empty() ? Value::make_string("")
                         : Value::make_string(argvals[0].to_string());
}

Value ScriptRuntime::builtin_trim(const std::vector<Value> &argvals,
                                  const nari::CallExpr *) {
  if (!argvals.empty()) {
    std::string str = argvals[0].to_string();
    size_t start = 0;
    while (start < str.size() &&
           std::isspace(static_cast<unsigned char>(str[start]))) {
      ++start;
    }
    size_t end = str.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(str[end - 1]))) {
      --end;
    }
    return Value::make_string(str.substr(start, end - start));
  }
  return Value::make_string("");
}

Value ScriptRuntime::builtin_toUpper(const std::vector<Value> &argvals,
                                     const nari::CallExpr *) {
  if (!argvals.empty()) {
    std::string str = argvals[0].to_string();
    for (char &c : str) {
      c = std::toupper(c);
    }
    return Value::make_string(str);
  }
  return Value::make_string("");
}

Value ScriptRuntime::builtin_toLower(const std::vector<Value> &argvals,
                                     const nari::CallExpr *) {
  if (!argvals.empty()) {
    std::string str = argvals[0].to_string();
    for (char &c : str) {
      c = std::tolower(c);
    }
    return Value::make_string(str);
  }
  return Value::make_string("");
}

Value ScriptRuntime::builtin_startsWith(const std::vector<Value> &argvals,
                                        const nari::CallExpr *) {
  if (argvals.size() >= 2) {
    std::string str = argvals[0].to_string();
    std::string prefix = argvals[1].to_string();
    return Value::make_bool(str.size() >= prefix.size() &&
                            str.compare(0, prefix.size(), prefix) == 0);
  }
  return Value::make_bool(false);
}

Value ScriptRuntime::builtin_endsWith(const std::vector<Value> &argvals,
                                      const nari::CallExpr *) {
  if (argvals.size() >= 2) {
    std::string str = argvals[0].to_string();
    std::string suffix = argvals[1].to_string();
    return Value::make_bool(
        str.size() >= suffix.size() &&
        str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0);
  }
  return Value::make_bool(false);
}

Value ScriptRuntime::builtin_charAt(const std::vector<Value> &argvals,
                                    const nari::CallExpr *) {
  if (argvals.size() >= 2) {
    std::string str = argvals[0].to_string();
    if (!argvals[1].is_int())
      return Value::make_string("");
    int index = argvals[1].get_int();
    if (index >= 0 && index < str.size()) {
      return Value::make_string(std::string(1, str[index]));
    }
  }
  return Value::make_string("");
}

Value ScriptRuntime::builtin_join(const std::vector<Value> &argvals,
                                  const nari::CallExpr *) {
  if (argvals.size() >= 2 && argvals[0].is_array()) {
    auto &arr = argvals[0].get_array();
    if (!arr)
      return Value::make_string("");

    std::string delim = argvals[1].to_string();
    std::string result;
    bool first = true;
    for (const auto &val : *arr) {
      if (!first)
        result += delim;
      result += val.to_string();
      first = false;
    }
    return Value::make_string(result);
  }
  return Value::make_string("");
}

// Type checking and conversion builtins
Value ScriptRuntime::builtin_typeof(const std::vector<Value> &argvals,
                                    const nari::CallExpr *) {
  if (!argvals.empty()) {
    if (argvals[0].is_none())
      return Value::make_string("null");
    if (argvals[0].is_int())
      return Value::make_string("int");
    if (argvals[0].is_float())
      return Value::make_string("float");
    if (argvals[0].is_string())
      return Value::make_string("string");
    if (argvals[0].is_bool())
      return Value::make_string("bool");
    if (argvals[0].is_array())
      return Value::make_string("array");
    if (argvals[0].is_object())
      return Value::make_string("object");
    if (argvals[0].is_function())
      return Value::make_string("function");
    if (argvals[0].is_class_instance()) {
      return Value::make_string(argvals[0].get_class_instance()->class_name);
    }
    return Value::make_string("null");
  }
  return Value::make_string("null");
}

#ifndef NO_FFI
Value ScriptRuntime::builtin_ffi_membersof(const std::vector<Value> &argvals,
                                           const nari::CallExpr *) {
  if (argvals.empty()) {
    fprintf(stderr, "ERROR: __ffi_membersof() requires an argument (type name "
                    "string or type identifier)\n");
    return Value::none();
  }

  std::string type_name;
  if (argvals[0].is_string()) {
    type_name = argvals[0].get_string();
  } else if (argvals[0].is_object()) {
    // attempt to get __type property from object
    const auto &obj = argvals[0].get_object();
    auto it = obj->find("__type");
    if (it != obj->end() && it->second.is_string()) {
      type_name = it->second.get_string();
    } else {
      fprintf(stderr, "ERROR: __ffi_membersof() - object does not have a "
                      "__type property\n");
      return Value::none();
    }
  } else {
    fprintf(stderr, "ERROR: __ffi_membersof() requires a string or object "
                    "argument (type name)\n");
    return Value::none();
  }

  const nari::TypeDecl *type_decl = Parser::get_registered_type(type_name);

  if (!type_decl) {
    fprintf(stderr, "ERROR: Type '%s' not found in registry\n",
            type_name.c_str());
    return Value::none();
  }

  std::string resolved_type_name = type_name;
  const nari::TypeDecl *resolved_decl = type_decl;
  while (resolved_decl && resolved_decl->is_alias()) {
    if (!resolved_decl->alias_target) {
      fprintf(stderr, "ERROR: Type alias '%s' has no target\n",
              resolved_type_name.c_str());
      return Value::none();
    }

    // is alias primitive?
    const nari::TypeDecl *next_decl =
        Parser::get_registered_type(resolved_decl->alias_target->name);
    if (!next_decl) {
      // yes primitive, return directly
      auto result = std::make_shared<std::unordered_map<std::string, Value>>();
      (*result)["type"] = Value::make_string(resolved_decl->alias_target->name);
      return Value::make_object(result);
    }

    resolved_type_name = resolved_decl->alias_target->name;
    resolved_decl = next_decl;
  }

  if (!resolved_decl || resolved_decl->is_alias()) {
    fprintf(stderr, "ERROR: Could not resolve type '%s'\n", type_name.c_str());
    return Value::none();
  }

  // nari type -> ffi type
  auto map_type_to_ffi = [](const std::string &nari_type) -> std::string {
    if (nari_type == "f32" || nari_type == "float")
      return "float";
    if (nari_type == "f64" || nari_type == "double")
      return "double";
    if (nari_type == "i8")
      return "i8";
    if (nari_type == "u8")
      return "u8";
    if (nari_type == "i16")
      return "i16";
    if (nari_type == "u16")
      return "u16";
    if (nari_type == "i32" || nari_type == "int")
      return "int";
    if (nari_type == "i64" || nari_type == "long")
      return "long";
    if (nari_type == "u32" || nari_type == "uint")
      return "uint";
    if (nari_type == "u64" || nari_type == "ulong")
      return "ulong";
    if (nari_type == "bool" || nari_type == "boolean")
      return "bool";
    if (nari_type == "string" || nari_type == "pointer")
      return "pointer";

    return "int";
  };

  auto fields_array = std::make_shared<std::vector<Value>>();

  for (const auto &field : resolved_decl->fields) {
    auto field_obj = std::make_shared<std::unordered_map<std::string, Value>>();
    (*field_obj)["name"] = Value::make_string(field.name);
    (*field_obj)["type"] =
        Value::make_string(map_type_to_ffi(field.type->name));
    fields_array->push_back(Value::make_object(field_obj));
  }

  auto result = std::make_shared<std::unordered_map<std::string, Value>>();
  (*result)["struct"] = Value::make_string(resolved_type_name);
  (*result)["fields"] = Value::make_array(*fields_array);

  return Value::make_object(result);
}
#endif // NO_FFI

Value ScriptRuntime::builtin_toNumber(const std::vector<Value> &argvals,
                                      const nari::CallExpr *) {
  if (!argvals.empty()) {
    const Value &v = argvals[0];
    if (v.is_int())
      return Value::make_int(v.get_int());
    if (v.is_float())
      return Value::make_float(v.get_float());
    if (v.is_bool())
      return Value::make_int(v.get_bool() ? 1 : 0);
    if (v.is_string()) {
      const std::string &s = v.get_string();
      bool is_float = (s.find('.') != std::string::npos) ||
                      (s.find('e') != std::string::npos) ||
                      (s.find('E') != std::string::npos);
      if (is_float) {
        char *end = nullptr;
        double dv = std::strtod(s.c_str(), &end);
        if (end && *end == '\0')
          return Value::make_float(dv);
        return Value::make_float(0.0);
      }
      errno = 0;
      char *end = nullptr;
      int64_t iv = std::strtoll(s.c_str(), &end, 10);
      if (end && *end == '\0' && errno != ERANGE)
        return Value::make_int(static_cast<int64_t>(iv));
      return Value::make_float(std::strtod(s.c_str(), &end));
    }
    return Value::make_int(0);
  }
  return Value::make_int(0);
}

Value ScriptRuntime::builtin_toString(const std::vector<Value> &argvals,
                                      const nari::CallExpr *) {
  if (!argvals.empty()) {
    return Value::make_string(argvals[0].to_string());
  }
  return Value::make_string("");
}

Value ScriptRuntime::builtin_toBool(const std::vector<Value> &argvals,
                                    const nari::CallExpr *) {
  if (!argvals.empty()) {
    return Value::make_bool(argvals[0].as_bool());
  }
  return Value::make_bool(false);
}

Value ScriptRuntime::builtin_isNumber(const std::vector<Value> &argvals,
                                      const nari::CallExpr *) {
  if (!argvals.empty()) {
    return Value::make_bool(argvals[0].is_int() || argvals[0].is_float());
  }
  return Value::make_bool(false);
}

Value ScriptRuntime::builtin_isString(const std::vector<Value> &argvals,
                                      const nari::CallExpr *) {
  if (!argvals.empty()) {
    return Value::make_bool(argvals[0].is_string());
  }
  return Value::make_bool(false);
}

Value ScriptRuntime::builtin_isBool(const std::vector<Value> &argvals,
                                    const nari::CallExpr *) {
  if (!argvals.empty()) {
    return Value::make_bool(argvals[0].is_bool());
  }
  return Value::make_bool(false);
}

Value ScriptRuntime::builtin_isArray(const std::vector<Value> &argvals,
                                     const nari::CallExpr *) {
  if (!argvals.empty()) {
    return Value::make_bool(argvals[0].is_array());
  }
  return Value::make_bool(false);
}

Value ScriptRuntime::builtin_isObject(const std::vector<Value> &argvals,
                                      const nari::CallExpr *) {
  if (!argvals.empty()) {
    return Value::make_bool(argvals[0].is_object());
  }
  return Value::make_bool(false);
}

Value ScriptRuntime::builtin_isFunction(const std::vector<Value> &argvals,
                                        const nari::CallExpr *) {
  if (!argvals.empty()) {
    return Value::make_bool(argvals[0].is_function());
  }
  return Value::make_bool(false);
}

// I/O builtins
Value ScriptRuntime::builtin_readLine(const std::vector<Value> &,
                                      const nari::CallExpr *) {
  char *line_buf = nullptr;
  size_t buf_size = 0;
  ssize_t len = getline(&line_buf, &buf_size, stdin);
  if (len > 0) {
    // Remove trailing newline if present
    if (len > 0 && line_buf[len - 1] == '\n') {
      line_buf[len - 1] = '\0';
      len--;
    }
    std::string result(line_buf, len);
    free(line_buf);
    return Value::make_string(result);
  }
  free(line_buf);
  return Value::make_string("");
}

Value ScriptRuntime::builtin_readAll(const std::vector<Value> &,
                                     const nari::CallExpr *) {
  std::string result;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0) {
    result.append(buf, n);
  }
  return Value::make_string(result);
}

Value ScriptRuntime::builtin_time(const std::vector<Value> &,
                                  const nari::CallExpr *) {
  auto now = std::chrono::system_clock::now();
  auto epoch = now.time_since_epoch();
  auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count();
  return Value::make_int(ms);
}

#ifndef NO_FFI
// FFI builtins
Value ScriptRuntime::builtin_ffi_load_library(const std::vector<Value> &argvals,
                                              const nari::CallExpr *) {
  if (argvals.empty() || !argvals[0].is_string()) {
    return Value::none();
  }

  std::string lib_path = argvals[0].get_string();

  auto &registry = FFIRegistry::instance();
  auto lib = registry.load_library(lib_path);

  if (!lib || !lib->is_loaded()) {
    // return error object, later on this will be standardized into an Error
    // type to be used with either try/catch or Result<T, E>.
    auto err_obj = std::make_shared<std::unordered_map<std::string, Value>>();
    (*err_obj)["error"] =
        Value::make_string("Failed to load library: " + lib_path);
    if (lib) {
      (*err_obj)["message"] = Value::make_string(lib->get_error());
    }
    (*err_obj)["loaded"] = Value::make_bool(false);
    return Value::make_object(err_obj);
  }

  auto lib_obj = std::make_shared<std::unordered_map<std::string, Value>>();
  (*lib_obj)["loaded"] = Value::make_bool(true);
  (*lib_obj)["path"] = Value::make_string(lib_path);
  (*lib_obj)["__ffi_handle__"] =
      Value::make_int(reinterpret_cast<int64_t>(lib.get()));

  const auto &symbols = lib->get_symbols();
  for (const auto &symbol : symbols) {
    // TODO: these are just placeholders, actual symbol values should be
    // function pointers probably
    (*lib_obj)[symbol] = Value::make_string("FFI:" + symbol);
  }

  std::vector<Value> symbols_array;
  for (const auto &symbol : symbols) {
    symbols_array.push_back(Value::make_string(symbol));
  }
  (*lib_obj)["__symbols__"] = Value::make_array(std::move(symbols_array));

  return Value::make_object(lib_obj);
}

// __ffi_get_symbol(lib, "function_name")
Value ScriptRuntime::builtin_ffi_get_symbol(const std::vector<Value> &argvals,
                                            const nari::CallExpr *) {
  if (argvals.size() < 2 || !argvals[0].is_object() ||
      !argvals[1].is_string()) {
    return Value::none();
  }

  auto lib_obj = argvals[0].get_object();
  std::string symbol_name = argvals[1].get_string();

  // Get the library handle
  if (lib_obj->find("__ffi_handle__") == lib_obj->end()) {
    return Value::none();
  }

  int64_t handle_int = (*lib_obj)["__ffi_handle__"].get_int();
  auto *lib = reinterpret_cast<FFILibrary *>(handle_int);

  void *symbol = lib->get_symbol(symbol_name);
  if (!symbol) {
    return Value::none();
  }

  return Value::make_int(reinterpret_cast<int64_t>(symbol));
}

// used like: __ffi_call(loaded_lib_reference, "function_name", signature_obj,
// [args...]) signature must look something like { return: "int", params:
// ["int", "string"] }
Value ScriptRuntime::builtin_ffi_call(const std::vector<Value> &argvals,
                                      const nari::CallExpr *) {

  if (argvals.size() < 3 || !argvals[0].is_object() ||
      !argvals[1].is_string() || !argvals[2].is_object()) {
    return Value::none();
  }

  if (!argvals[0].is_object()) {
    fprintf(stderr,
            "ERROR: First argument to __ffi_call is not an object (type index: "
            "%zu)\n",
            argvals[0].data.index());
    return Value::none();
  }

  auto lib_obj = argvals[0].get_object();
  std::string func_name = argvals[1].get_string();
  auto sig_obj = argvals[2].get_object();

  if (lib_obj->find("__ffi_handle__") == lib_obj->end()) {
    fprintf(stderr, "ERROR: __ffi_handle__ not found in library object\n");
    if (lib_obj->find("error") != lib_obj->end()) {
      fprintf(stderr, "Library load error: %s\n",
              (*lib_obj)["error"].get_string().c_str());
    }
    if (lib_obj->find("message") != lib_obj->end()) {
      fprintf(stderr, "Detailed message: %s\n",
              (*lib_obj)["message"].get_string().c_str());
    }
    return Value::none();
  }

  auto &handle_val = (*lib_obj)["__ffi_handle__"];
  if (!handle_val.is_int()) {
    fprintf(stderr,
            "ERROR: __ffi_handle__ is not an integer (type index: %zu)\n",
            handle_val.data.index());
    return Value::none();
  }

  int64_t handle_int = handle_val.get_int();
  auto *lib = reinterpret_cast<FFILibrary *>(handle_int);

  void *func_ptr = lib->get_symbol(func_name);
  if (!func_ptr) {
    return Value::none();
  }

  FFISignature sig;

  auto parse_ffi_type = [&sig](const Value &type_val,
                               bool is_return) -> FFIType {
    if (type_val.is_string()) {
      std::string type_str = type_val.get_string();

      // Check for pointer syntax (e.g., "MSG*", "u8*", "Rectangle*")
      if (!type_str.empty() && type_str.back() == '*') {
        return FFIType::Pointer;
      }

      if (type_str == "void")
        return FFIType::Void;
      else if (type_str == "i8" || type_str == "int8" || type_str == "char")
        return FFIType::Int8;
      else if (type_str == "u8" || type_str == "uint8" || type_str == "uchar" ||
               type_str == "byte")
        return FFIType::UInt8;
      else if (type_str == "i16" || type_str == "int16" || type_str == "short")
        return FFIType::Int16;
      else if (type_str == "u16" || type_str == "uint16" ||
               type_str == "ushort" || type_str == "word")
        return FFIType::UInt16;
      else if (type_str == "int" || type_str == "i32" || type_str == "int32")
        return FFIType::Int32;
      else if (type_str == "long" || type_str == "i64" || type_str == "int64")
        return FFIType::Int64;
      else if (type_str == "uint" || type_str == "u32")
        return FFIType::UInt32;
      else if (type_str == "ulong" || type_str == "u64")
        return FFIType::UInt64;
      else if (type_str == "float")
        return FFIType::Float;
      else if (type_str == "double")
        return FFIType::Double;
      else if (type_str == "bool")
        return FFIType::Bool;
      else if (type_str == "string" || type_str == "pointer")
        return FFIType::Pointer;
    } else if (type_val.is_object()) {
      // parse struct def
      auto type_obj = type_val.get_object();

      if (type_obj->find("struct") != type_obj->end() &&
          type_obj->find("fields") != type_obj->end()) {

        std::string struct_name = (*type_obj)["struct"].get_string();
        auto &fields_val = (*type_obj)["fields"];

        std::vector<FFIStructField> fields;

        if (fields_val.is_array()) {
          auto fields_array = fields_val.get_array();
          for (const auto &field_val : *fields_array) {
            if (field_val.is_object()) {
              auto field_obj = field_val.get_object();
              if (field_obj->find("name") != field_obj->end() &&
                  field_obj->find("type") != field_obj->end()) {

                std::string field_name = (*field_obj)["name"].get_string();
                std::string field_type_str = (*field_obj)["type"].get_string();
                FFIType field_type = FFIType::Void;

                if (field_type_str == "i8" || field_type_str == "int8" ||
                    field_type_str == "char")
                  field_type = FFIType::Int8;
                else if (field_type_str == "u8" || field_type_str == "uint8" ||
                         field_type_str == "uchar" || field_type_str == "byte")
                  field_type = FFIType::UInt8;
                else if (field_type_str == "i16" || field_type_str == "int16" ||
                         field_type_str == "short")
                  field_type = FFIType::Int16;
                else if (field_type_str == "u16" ||
                         field_type_str == "uint16" ||
                         field_type_str == "ushort" || field_type_str == "word")
                  field_type = FFIType::UInt16;
                else if (field_type_str == "int" || field_type_str == "i32" ||
                         field_type_str == "int32")
                  field_type = FFIType::Int32;
                else if (field_type_str == "long" || field_type_str == "i64" ||
                         field_type_str == "int64")
                  field_type = FFIType::Int64;
                else if (field_type_str == "uint" || field_type_str == "u32")
                  field_type = FFIType::UInt32;
                else if (field_type_str == "ulong" || field_type_str == "u64")
                  field_type = FFIType::UInt64;
                else if (field_type_str == "float")
                  field_type = FFIType::Float;
                else if (field_type_str == "double")
                  field_type = FFIType::Double;
                else if (field_type_str == "bool")
                  field_type = FFIType::Bool;
                else if (field_type_str == "string" ||
                         field_type_str == "pointer") {
                  field_type = FFIType::Pointer;
                }

                fields.emplace_back(field_name, field_type);
              }
            }
          }
        }

        auto struct_def = std::make_shared<FFIStructDef>(struct_name, fields);

        if (is_return) {
          sig.return_struct_def = struct_def;
        } else {
          sig.param_struct_defs.push_back(struct_def);
        }

        return FFIType::Struct;
      }
    }

    return FFIType::Void;
  };

  if (sig_obj->find("returns") != sig_obj->end()) {
    sig.return_type = parse_ffi_type((*sig_obj)["returns"], true);
  } else {
    fprintf(stderr,
            "ERROR: FFI signature object is missing 'returns' field!\n");
    return Value::none();
  }

  // is this call variadic?
  if (sig_obj->find("variadic") != sig_obj->end()) {
    auto &variadic_val = (*sig_obj)["variadic"];
    if (variadic_val.is_int()) {
      sig.is_variadic = true;
      sig.fixed_param_count = variadic_val.get_int();
    } else if (variadic_val.is_bool() && variadic_val.get_bool()) {
      sig.is_variadic = true;
      sig.fixed_param_count = 0; // will be set after parsing params
    }
  }

  // parse param types
  if (sig_obj->find("params") != sig_obj->end() &&
      (*sig_obj)["params"].is_array()) {
    auto params_array = (*sig_obj)["params"].get_array();
    for (const auto &param_val : *params_array) {
      FFIType param_type = parse_ffi_type(param_val, false);
      sig.param_types.push_back(param_type);

      // if not struct, add nullptr to keep alignment
      if (param_type != FFIType::Struct) {
        sig.param_struct_defs.push_back(nullptr);
      }
    }

    // if variadic was set to true without a count, use param array size as
    // fixed count
    if (sig.is_variadic && sig.fixed_param_count == 0) {
      sig.fixed_param_count = params_array->size();
    }
  }

  // function arguments (everything after the first 3 args)
  std::vector<Value> func_args;
  if (argvals.size() > 3 && argvals[3].is_array()) {
    auto args_array = argvals[3].get_array();
    for (const auto &arg : *args_array) {
      func_args.push_back(arg);
    }
  } else {
    // otherwise, take all args after the 3rd as individual args
    for (size_t i = 3; i < argvals.size(); i++) {
      func_args.push_back(argvals[i]);
    }
  }

  if (sig.is_variadic && func_args.size() > sig.fixed_param_count) {
    // infer types for variadic arguments based on their values
    for (size_t i = sig.fixed_param_count; i < func_args.size(); i++) {
      const Value &arg = func_args[i];
      if (arg.is_string()) {
        sig.param_types.push_back(FFIType::Pointer);
      } else if (arg.is_int()) {
        sig.param_types.push_back(FFIType::Int32);
      } else if (arg.is_float()) {
        sig.param_types.push_back(FFIType::Double);
      } else if (arg.is_bool()) {
        sig.param_types.push_back(FFIType::Bool);
      } else {
        // realistically I don't think there will be a scenario where we ever
        // get here, but default to int
        printf("Unable to infer FFI variadic argument type, defaulting to "
               "int32\n");
        sig.param_types.push_back(FFIType::Int32);
      }
    }
  }

  if (sig.is_variadic) {
    return FFICaller::call_function_variadic(func_ptr, sig, func_args);
  } else {
    return FFICaller::call_function(func_ptr, sig, func_args);
  }
}

Value ScriptRuntime::builtin_ffi_utf16(const std::vector<Value> &argvals,
                                       const nari::CallExpr *) {
  if (argvals.empty() || !argvals[0].is_string()) {
    return Value::make_int(0);
  }

  const std::string &utf8 = argvals[0].get_string();
  std::u16string utf16;
  if (!utf8_to_utf16(utf8, utf16)) {
    fprintf(stderr, "ERROR: __ffi_utf16() received invalid UTF-8 input\n");
    return Value::make_int(0);
  }

  size_t units = utf16.size() + 1; // include null terminator
  auto *raw = static_cast<char16_t *>(std::malloc(units * sizeof(char16_t)));
  if (!raw) {
    fprintf(stderr, "ERROR: __ffi_utf16() failed to allocate memory\n");
    return Value::make_int(0);
  }

  if (!utf16.empty()) {
    std::memcpy(raw, utf16.data(), utf16.size() * sizeof(char16_t));
  }
  raw[utf16.size()] = u'\0';

  return Value::make_int(reinterpret_cast<int64_t>(raw));
}

Value ScriptRuntime::builtin_ffi_alloc(const std::vector<Value> &argvals,
                                       const nari::CallExpr *) {
  if (argvals.empty() || !argvals[0].is_int()) {
    return Value::make_int(0);
  }

  int64_t size = argvals[0].get_int();
  if (size <= 0) {
    return Value::make_int(0);
  }

  void *raw = std::calloc(1, static_cast<size_t>(size));
  if (!raw) {
    return Value::make_int(0);
  }

  return Value::make_int(reinterpret_cast<int64_t>(raw));
}

Value ScriptRuntime::builtin_ffi_utf16_read(const std::vector<Value> &argvals,
                                            const nari::CallExpr *) {
  if (argvals.empty() || !argvals[0].is_int()) {
    return Value::make_string("");
  }

  auto *ptr = reinterpret_cast<const char16_t *>(argvals[0].get_int());
  if (!ptr) {
    return Value::make_string("");
  }

  int64_t max_units = -1;
  if (argvals.size() >= 2 && argvals[1].is_int()) {
    max_units = argvals[1].get_int();
  }

  size_t units = 0;
  if (max_units > 0) {
    size_t limit = static_cast<size_t>(max_units);
    while (units < limit && ptr[units] != u'\0') {
      ++units;
    }
  } else {
    while (ptr[units] != u'\0') {
      ++units;
    }
  }

  std::u16string utf16(ptr, units);
  std::string utf8;
  if (!utf16_to_utf8(utf16, utf8)) {
    fprintf(stderr,
            "ERROR: __ffi_utf16_read() failed to convert UTF-16 to UTF-8\n");
    return Value::make_string("");
  }

  return Value::make_string(utf8);
}

Value ScriptRuntime::builtin_ffi_free(const std::vector<Value> &argvals,
                                      const nari::CallExpr *) {
  if (argvals.empty() || !argvals[0].is_int()) {
    return Value::none();
  }

  int64_t ptr_value = argvals[0].get_int();
  if (ptr_value != 0) {
    std::free(reinterpret_cast<void *>(ptr_value));
  }

  return Value::none();
}

// __ffi_alloc_struct(typename) - allocate memory for a struct type
Value ScriptRuntime::builtin_ffi_alloc_struct(const std::vector<Value> &argvals,
                                              const nari::CallExpr *) {
  if (argvals.empty() || !argvals[0].is_string()) {
    fprintf(stderr,
            "ERROR: __ffi_alloc_struct() requires a type name string\n");
    return Value::none();
  }

  std::string type_name = argvals[0].get_string();
  size_t struct_size = nari::get_struct_size(type_name);

  if (struct_size == 0) {
    fprintf(stderr,
            "ERROR: __ffi_alloc_struct() failed to get size for type '%s'\n",
            type_name.c_str());
    return Value::none();
  }

  void *ptr = std::calloc(1, struct_size);
  if (!ptr) {
    fprintf(stderr,
            "ERROR: __ffi_alloc_struct() failed to allocate %zu bytes\n",
            struct_size);
    return Value::none();
  }

  return Value::make_int(reinterpret_cast<int64_t>(ptr));
}

// __ffi_read_struct(ptr, typename) - read struct from memory into Nari object
Value ScriptRuntime::builtin_ffi_read_struct(const std::vector<Value> &argvals,
                                             const nari::CallExpr *) {
  if (argvals.size() < 2 || !argvals[0].is_int() || !argvals[1].is_string()) {
    fprintf(stderr, "ERROR: __ffi_read_struct() requires (pointer_int, "
                    "type_name_string)\n");
    return Value::none();
  }

  int64_t ptr_value = argvals[0].get_int();
  std::string type_name = argvals[1].get_string();

  if (ptr_value == 0) {
    fprintf(stderr, "ERROR: __ffi_read_struct() received null pointer\n");
    return Value::none();
  }

  void *ptr = reinterpret_cast<void *>(ptr_value);
  return nari::read_struct_from_memory(ptr, type_name);
}

// __ffi_write_struct(ptr, typename, obj) - write Nari object to struct memory
Value ScriptRuntime::builtin_ffi_write_struct(const std::vector<Value> &argvals,
                                              const nari::CallExpr *) {
  if (argvals.size() < 3 || !argvals[0].is_int() || !argvals[1].is_string() ||
      !argvals[2].is_object()) {
    fprintf(stderr, "ERROR: __ffi_write_struct() requires (pointer_int, "
                    "type_name_string, object)\n");
    return Value::none();
  }

  int64_t ptr_value = argvals[0].get_int();
  std::string type_name = argvals[1].get_string();
  const Value &obj = argvals[2];

  if (ptr_value == 0) {
    fprintf(stderr, "ERROR: __ffi_write_struct() received null pointer\n");
    return Value::none();
  }

  void *ptr = reinterpret_cast<void *>(ptr_value);
  nari::write_struct_to_memory(ptr, type_name, obj);

  return Value::none();
}

// __ffi_sizeof(typename) - get size of a struct type
Value ScriptRuntime::builtin_ffi_sizeof(const std::vector<Value> &argvals,
                                        const nari::CallExpr *) {
  if (argvals.empty() || !argvals[0].is_string()) {
    fprintf(stderr, "ERROR: __ffi_sizeof() requires a type name string\n");
    return Value::none();
  }

  std::string type_name = argvals[0].get_string();
  size_t struct_size = nari::get_struct_size(type_name);

  if (struct_size == 0) {
    fprintf(stderr, "ERROR: __ffi_sizeof() failed to get size for type '%s'\n",
            type_name.c_str());
    return Value::none();
  }

  return Value::make_int(static_cast<int64_t>(struct_size));
}

#endif // NO_FFI

Value ScriptRuntime::builtin_platform_arch(const std::vector<Value> &,
                                           const nari::CallExpr *) {
#if defined(__EMSCRIPTEN__)
#ifdef __wasm64__
  return Value::make_string("wasm64");
#else
  return Value::make_string("wasm32");
#endif
#endif

#if defined(__x86_64__) || defined(_M_X64)
  return Value::make_string("x86_64");
#elif defined(__i386) || defined(_M_IX86)
  return Value::make_string("x86");
#elif defined(__aarch64__)
  return Value::make_string("arm64");
#elif defined(__arm__) || defined(_M_ARM)
  return Value::make_string("arm");
#elif defined(__ppc64__) || defined(__PPC64__)
  return Value::make_string("ppc64");
#elif defined(__ppc__) || defined(__PPC__)
  return Value::make_string("ppc");
#else
  return Value::make_string("unknown");
#endif
}

Value ScriptRuntime::builtin_platform_os(const std::vector<Value> &,
                                         const nari::CallExpr *) {
#if defined(__EMSCRIPTEN__)
  return Value::make_string("emscripten");
#endif

#if defined(_WIN32)
  return Value::make_string("windows");
#elif defined(__APPLE__) && defined(__MACH__)
  return Value::make_string("macos");
#elif defined(__linux__)
  return Value::make_string("linux");
#elif defined(__unix__) || defined(__unix)
  return Value::make_string("unix");
#else
  return Value::make_string("unknown");
#endif
}

Value ScriptRuntime::builtin_platform_endianness(const std::vector<Value> &,
                                                 const nari::CallExpr *) {
  return std::endian::native == std::endian::little
             ? Value::make_string("little")
             : Value::make_string("big");
}

Value ScriptRuntime::builtin_platform_hostname(const std::vector<Value> &,
                                               const nari::CallExpr *) {
  char hostname[256];
  gethostname(hostname, sizeof(hostname));
  return Value::make_string(hostname);
}

Value ScriptRuntime::builtin_platform_getenv(const std::vector<Value> &argvals,
                                             const nari::CallExpr *) {
  if (argvals.size() >= 1 && argvals[0].is_string()) {
    const char *value = getenv(argvals[0].get_string().c_str());
    if (value) {
      return Value::make_string(value);
    }
  }
  return Value::make_string("");
}

Value ScriptRuntime::builtin_system_exec(const std::vector<Value> &argvals,
                                         const nari::CallExpr *) {
  if (argvals.size() >= 1 && argvals[0].is_string()) {
    std::string command = argvals[0].get_string();
    int ret = system(command.c_str());
    return Value::make_int(ret);
  }
  return Value::make_int(-1);
}
// Garbage Collector builtin functions
Value ScriptRuntime::builtin_gc_collect(const std::vector<Value> &,
                                        const CallExpr *) {
  auto &gc = GarbageCollector::instance();

  // Force immediate collection
  auto roots = collect_gc_roots();
  size_t collected = gc.force_collect(roots);

  // Return statistics object
  auto stats = gc.get_stats();
  auto result = Value::make_object();
  auto &obj = result.get_object();

  (*obj)["collected"] = Value::make_int(collected);
  (*obj)["tracked"] = Value::make_int(stats.tracked_count);
  (*obj)["totalCollections"] = Value::make_int(stats.total_collections);
  (*obj)["totalCollected"] = Value::make_int(stats.total_collected);
  (*obj)["totalAllocated"] = Value::make_int(stats.total_allocated);
  (*obj)["peakTracked"] = Value::make_int(stats.peak_tracked);
  (*obj)["refcountFreed"] = Value::make_int(stats.refcount_freed);

  return result;
}

Value ScriptRuntime::builtin_gc_stats(const std::vector<Value> &,
                                      const CallExpr *) {
  auto &gc = GarbageCollector::instance();
  auto stats = gc.get_stats();

  auto result = Value::make_object();
  auto &obj = result.get_object();

  (*obj)["tracked"] = Value::make_int(stats.tracked_count);
  (*obj)["allocationCount"] = Value::make_int(stats.allocation_count);
  (*obj)["totalCollections"] = Value::make_int(stats.total_collections);
  (*obj)["totalCollected"] = Value::make_int(stats.total_collected);
  (*obj)["totalAllocated"] = Value::make_int(stats.total_allocated);
  (*obj)["peakTracked"] = Value::make_int(stats.peak_tracked);
  (*obj)["refcountFreed"] = Value::make_int(stats.refcount_freed);
  (*obj)["threshold"] = Value::make_int(stats.collection_threshold);
  (*obj)["enabled"] = Value::make_bool(stats.enabled);

  return result;
}

Value ScriptRuntime::builtin_gc_enable(const std::vector<Value> &argvals,
                                       const CallExpr *) {
  auto &gc = GarbageCollector::instance();

  if (argvals.size() >= 1 && argvals[0].is_bool()) {
    gc.set_enabled(argvals[0].get_bool());
    return Value::make_bool(true);
  }

  return Value::make_bool(false);
}

#ifndef NO_FFI
// __ffi_create_callback(signature, nari_function) - create a native callback
// from a Nari function
Value ScriptRuntime::builtin_ffi_create_callback(
    const std::vector<Value> &argvals, const nari::CallExpr *) {
  if (argvals.size() < 2 || !argvals[0].is_object() ||
      !argvals[1].is_function()) {
    fprintf(stderr, "ERROR: __ffi_create_callback() requires "
                    "(signature_object, nari_function)\n");
    return Value::none();
  }

  auto sig_obj = argvals[0].get_object();
  const Value &nari_func = argvals[1];

  // Parse signature (same as in __ffi_call)
  FFISignature sig;

  auto parse_ffi_type = [&sig](const Value &type_val,
                               bool is_return) -> FFIType {
    if (type_val.is_string()) {
      std::string type_str = type_val.get_string();

      // Check for pointer syntax
      if (!type_str.empty() && type_str.back() == '*') {
        return FFIType::Pointer;
      }

      if (type_str == "void")
        return FFIType::Void;
      else if (type_str == "i8" || type_str == "int8" || type_str == "char")
        return FFIType::Int8;
      else if (type_str == "u8" || type_str == "uint8" || type_str == "uchar" ||
               type_str == "byte")
        return FFIType::UInt8;
      else if (type_str == "i16" || type_str == "int16" || type_str == "short")
        return FFIType::Int16;
      else if (type_str == "u16" || type_str == "uint16" ||
               type_str == "ushort" || type_str == "word")
        return FFIType::UInt16;
      else if (type_str == "int" || type_str == "i32" || type_str == "int32")
        return FFIType::Int32;
      else if (type_str == "long" || type_str == "i64" || type_str == "int64")
        return FFIType::Int64;
      else if (type_str == "uint" || type_str == "u32")
        return FFIType::UInt32;
      else if (type_str == "ulong" || type_str == "u64")
        return FFIType::UInt64;
      else if (type_str == "float")
        return FFIType::Float;
      else if (type_str == "double")
        return FFIType::Double;
      else if (type_str == "bool")
        return FFIType::Bool;
      else if (type_str == "string" || type_str == "pointer")
        return FFIType::Pointer;
    }

    fprintf(stderr, "ERROR: Unsupported FFI type for callback\n");
    return FFIType::Void;
  };

  // Parse return type
  if (sig_obj->find("returns") != sig_obj->end()) {
    sig.return_type = parse_ffi_type((*sig_obj)["returns"], true);
  } else {
    sig.return_type = FFIType::Void;
  }

  // Parse parameter types
  if (sig_obj->find("params") != sig_obj->end()) {
    auto &params_val = (*sig_obj)["params"];
    if (params_val.is_array()) {
      auto params_arr = params_val.get_array();
      for (const auto &param_val : *params_arr) {
        sig.param_types.push_back(parse_ffi_type(param_val, false));
      }
    }
  }

  // Create the callback
  void *callback_ptr =
      FFICallbackManager::instance().create_callback(sig, nari_func, this);

  if (!callback_ptr) {
    fprintf(stderr, "ERROR: Failed to create FFI callback\n");
    return Value::none();
  }

  // Return the function pointer as an integer
  return Value::make_int(reinterpret_cast<int64_t>(callback_ptr));
}

// __ffi_free_callback(callback_pointer) - free a previously created callback
Value ScriptRuntime::builtin_ffi_free_callback(
    const std::vector<Value> &argvals, const nari::CallExpr *) {
  if (argvals.empty() || !argvals[0].is_int()) {
    fprintf(
        stderr,
        "ERROR: __ffi_free_callback() requires a callback pointer (integer)\n");
    return Value::none();
  }

  int64_t ptr_value = argvals[0].get_int();
  if (ptr_value != 0) {
    void *callback_ptr = reinterpret_cast<void *>(ptr_value);
    FFICallbackManager::instance().free_callback(callback_ptr);
  }

  return Value::none();
}
#endif

Value ScriptRuntime::builtin_gc_set_threshold(const std::vector<Value> &argvals,
                                              const CallExpr *) {
  auto &gc = GarbageCollector::instance();

  if (argvals.size() >= 1) {
    int64_t threshold = 0;
    if (argvals[0].is_int()) {
      threshold = argvals[0].get_int();
    } else if (argvals[0].is_float()) {
      threshold = static_cast<int64_t>(argvals[0].get_float());
    } else {
      return Value::make_bool(false);
    }

    if (threshold > 0) {
      gc.set_collection_threshold(static_cast<size_t>(threshold));
      return Value::make_bool(true);
    }
  }

  return Value::make_bool(false);
}
