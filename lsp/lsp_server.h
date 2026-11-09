#pragma once
// NariLspServer class declaration. Method definitions live in lsp_server.cpp
// (core), lsp_handlers.cpp, lsp_parse.cpp and lsp_resolve.cpp.
// (Split from lsp/lsp_server.cpp.)

#include "lsp_analysis.h"
#include "lsp_symbols.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

// Transport + logging (defined in lsp_server.cpp).
void lsp_log(const std::string &msg);
void init_logger();
std::string read_lsp_message();
void write_lsp_message(const std::string &body);

class NariLspServer {
  public:
    NariLspServer() = default;
    ~NariLspServer();
    void run();

  private:
    bool shutdown = false;
    bool exit = false;
    std::unordered_map<std::string, DocState> docs;
    // Protects docs: request handlers take shared_lock; parse thread takes
    // unique_lock only for the brief commit step after parsing completes.
    std::shared_timed_mutex docs_mutex;
    std::mutex builtins_mutex_;
    std::atomic<bool> builtins_ready_{ false };
    std::thread builtins_thread;
    std::map<std::string, SymInfo> builtin_syms_;
    std::map<std::string, std::vector<SymInfo>> stdlib_member_syms_;
    std::map<std::string, SymInfo> method_syms_;
    // Type-specific method maps: type_name -> {method_name -> SymInfo}
    // Keys are "regex", "string", "array", "number", "bool", etc.
    std::map<std::string, std::map<std::string, SymInfo>> method_type_syms_;
    std::string builtin_decls_uri_;
    std::string stdlib_decls_uri_;
    // Background parse queue: latest document content wins per URI.
    struct ParseRequest {
        std::string uri;
        std::string content;
        std::size_t hash;
    };
    std::vector<ParseRequest> parse_queue_;
    std::mutex parse_queue_mutex_;
    std::condition_variable parse_queue_cv_;
    bool parse_thread_stop_{ false };
    std::thread parse_thread;

    // write builtins.d.nari, parse it, build symbol map
    void init_builtins();
    void dispatch(const json &msg);
    void send_response(const json &id, json result);
    void send_error(const json &id, int code, const std::string &msg);
    void send_notification(const std::string &method, json params);
    struct ResolvedSymbol {
        SymInfo sym;
        std::string def_uri;
    };
    static bool same_symbol_identity(const SymInfo &a, const SymInfo &b);
    bool resolve_symbol_at(const std::string &uri, int line, int col, ResolvedSymbol &out);
    template <typename Fn> static void for_each_identifier_occurrence(const std::string &content, Fn &&fn) {
        const auto is_ident = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
        std::istringstream ss(content);
        std::string line;
        int line_no = 0;
        while (std::getline(ss, line)) {
            bool in_double = false;
            bool in_single = false;
            for (int i = 0; i < static_cast<int>(line.size());) {
                if (!in_single && !in_double && i + 1 < static_cast<int>(line.size()) && line[i] == '/' && line[i + 1] == '/') {
                    break;
                }
                if (!in_single && line[i] == '"') {
                    in_double = !in_double;
                    ++i;
                    continue;
                }
                if (!in_double && line[i] == '\'') {
                    in_single = !in_single;
                    ++i;
                    continue;
                }
                if (in_single || in_double) {
                    if (line[i] == '\\' && i + 1 < static_cast<int>(line.size())) {
                        i += 2;
                    } else {
                        ++i;
                    }
                    continue;
                }
                if (!std::isalpha((unsigned char)line[i]) && line[i] != '_') {
                    ++i;
                    continue;
                }
                int start = i++;
                while (i < static_cast<int>(line.size()) && is_ident(line[i])) {
                    ++i;
                }
                fn(line_no, start, std::string_view(line.data() + start, i - start));
            }
            ++line_no;
        }
    }
    void handle_initialize(const json &id);
    void handle_shutdown(const json &id);
    void handle_did_open(const json &params);
    void handle_did_change(const json &params);
    void handle_did_close(const json &params);
    void handle_completion(const json &id, const json &params);
    void handle_hover(const json &id, const json &params);
    void handle_signature_help(const json &id, const json &params);
    void handle_definition(const json &id, const json &params);
    void handle_references(const json &id, const json &params);
    void handle_code_action(const json &id, const json &params);
    void handle_inlay_hints(const json &id, const json &params);
    void handle_semantic_tokens_full(const json &id, const json &params);
    void handle_type_definition(const json &id, const json &params);
    void enqueue_parse(const std::string &uri, const std::string &content, std::size_t hash = 0);
    void parse_thread_func();
    void parse_document(const std::string &uri, const std::string &content, std::size_t content_hash = 0);
};
