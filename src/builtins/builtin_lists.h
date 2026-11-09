#pragma once

// conditional builtin lists based on feature availability
#ifndef DISABLE_HTTP
#define BUILTIN_HTTP_LIST(X)                                                                                                               \
    X("__net_createServer", builtin_net_createServer)                                                                                      \
    X("__net_conn_read", builtin_net_conn_read)                                                                                            \
    X("__net_conn_write", builtin_net_conn_write)                                                                                          \
    X("__net_conn_close", builtin_net_conn_close)                                                                                          \
    X("__net_connect", builtin_net_connect)                                                                                                \
    X("__net_listen", builtin_net_listen)                                                                                                  \
    X("__net_accept", builtin_net_accept)                                                                                                  \
    X("__net_server_close", builtin_net_server_close)                                                                                      \
    X("__udp_bind", builtin_udp_bind)                                                                                                      \
    X("__udp_send", builtin_udp_send)                                                                                                      \
    X("__udp_recv", builtin_udp_recv)                                                                                                      \
    X("__udp_close", builtin_udp_close)                                                                                                    \
    X("__http_fetch", builtin_http_fetch)
#else
#define BUILTIN_HTTP_LIST(X)
#endif

#ifndef NARI_ESP_IDF
#define BUILTIN_READLINE_LIST(X)                                                                                                           \
    X("read_line", builtin_readLine)                                                                                                       \
    X("read_all", builtin_readAll)
#else
#define BUILTIN_READLINE_LIST(X)
#endif

#ifndef DISABLE_FFI
#define BUILTIN_FFI_LIST(X)                                                                                                                \
    X("__ffi_load_library", builtin_ffi_load_library)                                                                                      \
    X("__ffi_get_symbol", builtin_ffi_get_symbol)                                                                                          \
    X("__ffi_call", builtin_ffi_call)                                                                                                      \
    X("__ffi_membersof", builtin_ffi_membersof)                                                                                            \
    X("__ffi_alloc", builtin_ffi_alloc)                                                                                                    \
    X("__ffi_alloc_struct", builtin_ffi_alloc_struct)                                                                                      \
    X("__ffi_managed_struct", builtin_ffi_managed_struct)                                                                                  \
    X("__ffi_read_struct", builtin_ffi_read_struct)                                                                                        \
    X("__ffi_write_struct", builtin_ffi_write_struct)                                                                                      \
    X("__ffi_sizeof", builtin_ffi_sizeof)                                                                                                  \
    X("__ffi_utf16", builtin_ffi_utf16)                                                                                                    \
    X("__ffi_utf16_read", builtin_ffi_utf16_read)                                                                                          \
    X("__ffi_free", builtin_ffi_free)                                                                                                      \
    X("__ffi_create_callback", builtin_ffi_create_callback)                                                                                \
    X("__ffi_free_callback", builtin_ffi_free_callback)
#else
#define BUILTIN_FFI_LIST(X)
#endif

#define BUILTIN_FUNCTIONS(X)                                                                                                               \
    X("__process_exec", builtin_process_exec)                                                                                              \
    X("eval", builtin_eval)                                                                                                                \
    X("print", builtin_print)                                                                                                              \
    X("__write_stdout", builtin_write_stdout)                                                                                              \
    X("__write_stderr", builtin_write_stderr)                                                                                              \
    X("panic", builtin_panic)                                                                                                              \
    X("__nari_catch", builtin_nari_catch)                                                                                                  \
    X("__nari_invoke_with_this", builtin_nari_invoke_with_this)                                                                            \
    X("set_timeout", builtin_setTimeout)                                                                                                   \
    X("__math_floor", builtin_math_floor)                                                                                                  \
    X("__math_ceil", builtin_math_ceil)                                                                                                    \
    X("__math_sqrt", builtin_math_sqrt)                                                                                                    \
    X("__math_rand", builtin_math_rand)                                                                                                    \
    X("__math_sin", builtin_math_sin)                                                                                                      \
    X("__math_cos", builtin_math_cos)                                                                                                      \
    X("__math_tan", builtin_math_tan)                                                                                                      \
    X("__math_log", builtin_math_log)                                                                                                      \
    X("__math_exp", builtin_math_exp)                                                                                                      \
    X("__math_atan", builtin_math_atan)                                                                                                    \
    X("__math_atan2", builtin_math_atan2)                                                                                                  \
    X("__fs_readFile", builtin_fs_readFile)                                                                                                \
    X("__fs_writeFile", builtin_fs_writeFile)                                                                                              \
    X("__fs_appendFile", builtin_fs_appendFile)                                                                                            \
    X("__fs_fileExists", builtin_fs_fileExists)                                                                                            \
    X("__fs_isDirectory", builtin_fs_isDirectory)                                                                                          \
    X("__fs_mkdirAll", builtin_fs_mkdirAll)                                                                                                \
    X("__fs_deleteFile", builtin_fs_deleteFile)                                                                                            \
    X("__fs_listDir", builtin_fs_listDir)                                                                                                  \
    X("__platform_arch", builtin_platform_arch)                                                                                            \
    X("__platform_os", builtin_platform_os)                                                                                                \
    X("__platform_endianness", builtin_platform_endianness)                                                                                \
    X("__platform_hostname", builtin_platform_hostname)                                                                                    \
    X("__platform_getenv", builtin_platform_getenv)                                                                                        \
    X("__platform_environ", builtin_platform_environ)                                                                                      \
    X("__platform_cwd", builtin_platform_cwd)                                                                                              \
    X("__platform_isatty", builtin_platform_isatty)                                                                                        \
    X("__process_exit", builtin_process_exit)                                                                                              \
    X("__process_argc", builtin_process_argc)                                                                                              \
    X("__process_argv", builtin_process_argv)                                                                                              \
    X("set_interval", builtin_setInterval)                                                                                                 \
    X("clear_interval", builtin_clearInterval)                                                                                             \
    BUILTIN_READLINE_LIST(X)                                                                                                               \
    BUILTIN_HTTP_LIST(X)                                                                                                                   \
    X("__yield", builtin_yield)                                                                                                            \
    X("__shutdown_requested", builtin_shutdown_requested)                                                                                  \
    X("typeof", builtin_typeof)                                                                                                            \
    X("__js_to_number", builtin_js_toNumber)                                                                                               \
    X("keys", builtin_keys)                                                                                                                \
    X("to_number", builtin_toNumber)                                                                                                       \
    X("to_string", builtin_toString)                                                                                                       \
    X("__format_value", builtin_formatValue)                                                                                               \
    X("to_bool", builtin_toBool)                                                                                                           \
    X("is_number", builtin_isNumber)                                                                                                       \
    X("is_string", builtin_isString)                                                                                                       \
    X("is_bool", builtin_isBool)                                                                                                           \
    X("is_array", builtin_isArray)                                                                                                         \
    X("is_object", builtin_isObject)                                                                                                       \
    X("is_function", builtin_isFunction)                                                                                                   \
    X("Delegate", builtin_delegate_new)                                                                                                    \
    X("is_delegate", builtin_isDelegate)                                                                                                   \
    X("delegate_target", builtin_delegateTarget)                                                                                           \
    X("delegate_handler", builtin_delegateHandler)                                                                                         \
    X("time", builtin_time)                                                                                                                \
    BUILTIN_FFI_LIST(X)                                                                                                                    \
    X("__gc_collect", builtin_gc_collect)                                                                                                  \
    X("__gc_stats", builtin_gc_stats)                                                                                                      \
    X("__gc_enable", builtin_gc_enable)                                                                                                    \
    X("__gc_set_threshold", builtin_gc_set_threshold)                                                                                      \
    X("__gc_set_memory_limit", builtin_gc_set_memory_limit)                                                                                \
    X("__gc_get_memory_usage", builtin_gc_get_memory_usage)                                                                                \
    X("parse_int", builtin_parse_int)                                                                                                      \
    X("parse_float", builtin_parse_float)                                                                                                  \
    X("random", builtin_random)                                                                                                            \
    X("range", builtin_range)                                                                                                              \
    X("contains", builtin_contains)                                                                                                        \
    X("__json_parse", builtin_json_parse)                                                                                                  \
    X("__json_stringify", builtin_json_stringify)                                                                                          \
    X("__hash_sha1", builtin_hash_sha1)                                                                                                    \
    X("__hash_sha256", builtin_hash_sha256)                                                                                                \
    X("__hash_sha256_file", builtin_hash_sha256_file)                                                                                      \
    X("__archive_list", builtin_archive_list)                                                                                              \
    X("__archive_extract", builtin_archive_extract)                                                                                        \
    X("__archive_create", builtin_archive_create)                                                                                          \
    X("__module_import_namespace", builtin_module_import_namespace)                                                                        \
    X("__module_import_named", builtin_module_import_named)                                                                                \
    X("from_char_code", builtin_from_char_code)                                                                                            \
    X("__hex_encode", builtin_hex_encode)                                                                                                  \
    X("__hex_decode", builtin_hex_decode)                                                                                                  \
    X("__base64_encode", builtin_base64_encode)                                                                                            \
    X("__base64_decode", builtin_base64_decode)                                                                                            \
    X("__regex_new", builtin_regex_new)                                                                                                    \
    X("__time_now_ms", builtin_time_now_ms)                                                                                                \
    X("__time_components", builtin_time_components)                                                                                        \
    X("__time_from_components", builtin_time_from_components)                                                                              \
    X("__time_format", builtin_time_format)                                                                                                \
    X("__time_parse_iso", builtin_time_parse_iso)                                                                                          \
    X("__url_encode", builtin_url_encode)                                                                                                  \
    X("__url_decode", builtin_url_decode)

// methods that are only accessible via type.method() syntax
#define string_methods(X)                                                                                                                  \
    X("substr", builtin_substr)                                                                                                            \
    X("last_index_of", builtin_last_index_of)                                                                                              \
    X("split", builtin_split)                                                                                                              \
    X("replace", builtin_replace)                                                                                                          \
    X("replace_all", builtin_replace_all)                                                                                                  \
    X("trim", builtin_trim)                                                                                                                \
    X("trim_start", builtin_trim_start)                                                                                                    \
    X("trim_end", builtin_trim_end)                                                                                                        \
    X("to_upper", builtin_to_upper)                                                                                                        \
    X("to_lower", builtin_to_lower)                                                                                                        \
    X("starts_with", builtin_starts_with)                                                                                                  \
    X("ends_with", builtin_ends_with)                                                                                                      \
    X("char_at", builtin_char_at)                                                                                                          \
    X("char_code_at", builtin_char_code_at)                                                                                                \
    X("pad_start", builtin_pad_start)                                                                                                      \
    X("pad_end", builtin_pad_end)                                                                                                          \
    X("repeat", builtin_repeat)                                                                                                            \
    X("at", builtin_string_at)                                                                                                             \
    X("to_char_array", builtin_to_char_array)

#define array_methods(X)                                                                                                                   \
    X("push", builtin_push)                                                                                                                \
    X("pop", builtin_pop)                                                                                                                  \
    X("slice", builtin_slice)                                                                                                              \
    X("concat", builtin_concat)                                                                                                            \
    X("join", builtin_join)                                                                                                                \
    X("sort", builtin_sort)                                                                                                                \
    X("map", builtin_map)                                                                                                                  \
    X("filter", builtin_filter)                                                                                                            \
    X("reduce", builtin_reduce)                                                                                                            \
    X("find", builtin_find)                                                                                                                \
    X("find_index", builtin_find_index)                                                                                                    \
    X("reverse", builtin_reverse)                                                                                                          \
    X("every", builtin_every)                                                                                                              \
    X("some", builtin_some)                                                                                                                \
    X("for_each", builtin_for_each)                                                                                                        \
    X("splice", builtin_splice)                                                                                                            \
    X("fill", builtin_fill)                                                                                                                \
    X("flat", builtin_flat)                                                                                                                \
    X("flat_map", builtin_flat_map)

#define object_methods(X)                                                                                                                  \
    X("keys", builtin_keys)                                                                                                                \
    X("values", builtin_values)                                                                                                            \
    X("has_key", builtin_has_key)                                                                                                          \
    X("entries", builtin_entries)                                                                                                          \
    X("assign", builtin_assign)                                                                                                            \
    X("freeze", builtin_freeze)                                                                                                            \
    X("is_frozen", builtin_is_frozen)

#define regex_methods(X)                                                                                                                   \
    X("test", builtin_regex_test)                                                                                                          \
    X("exec", builtin_regex_exec)

// methods that work on multiple types
#define universal_methods(X)                                                                                                               \
    X("length", builtin_length)                                                                                                            \
    X("includes", builtin_includes)                                                                                                        \
    X("index_of", builtin_index_of)

// combined method table
#define METHOD_ONLY_BUILTINS(X) string_methods(X) array_methods(X) object_methods(X) regex_methods(X) universal_methods(X)
