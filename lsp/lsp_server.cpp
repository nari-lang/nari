// -- Nari Language Server --

// Supported requests / notifications:
//   initialize / initialized
//   shutdown / exit
//   textDocument/didOpen
//   textDocument/didChange   (full-document sync)
//   textDocument/didClose
//   textDocument/completion
//   textDocument/hover
//   textDocument/signatureHelp
//   textDocument/definition
//   textDocument/references
//   textDocument/typeDefinition
//   textDocument/codeAction
//   textDocument/inlayHint
//   textDocument/semanticTokens/full

// TODO: move lsp into src/
#ifndef DISABLE_PARSER
#include "ast.h"
#include "core_types.h"
#include "parser_api.h"
#endif

#include "json_dom.h"
#include "protocol_io.h"

using json = JsonValue;

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

static std::ofstream g_log_file;

void lsp_log(const std::string &msg) {
    if (!g_log_file.is_open()) {
        return;
    }
    auto now = chrono::system_clock::now();
    auto ms = chrono::duration_cast<chrono::milliseconds>(now.time_since_epoch()).count();
    g_log_file << '[' << ms << "] " << msg << '\n';
    g_log_file.flush();
}

void init_logger() {
    std::error_code err;
    const auto temp_dir = std::filesystem::temp_directory_path(err);
    if (!err) {
        g_log_file.open(temp_dir / "nari-lsp.log", std::ios::app);
    }
    if (!g_log_file.is_open()) {
        err.clear();
        const auto current_dir = std::filesystem::current_path(err);
        if (!err) {
            g_log_file.clear();
            g_log_file.open(current_dir / "nari-lsp.log", std::ios::app);
        }
    }
    lsp_log("=== nari-lsp started ===");
}

// needed to make calling send_response and send_notification thread safe
static std::mutex g_stdout_mutex;

void write_lsp_message(const std::string &body) {
    std::lock_guard<std::mutex> lock(g_stdout_mutex);
    std::printf("Content-Length: %zu\r\n\r\n%s", body.size(), body.c_str());
    std::fflush(stdout);
}

#include "lsp_server.h"

#include <cstdio>
#include <fstream>
#include <iostream>

    NariLspServer::~NariLspServer() {
        // Signal and join the background parse thread cleanly.
        {
            std::lock_guard<std::mutex> lock(parse_queue_mutex_);
            parse_thread_stop_ = true;
        }
        parse_queue_cv_.notify_all();
        if (parse_thread.joinable()) {
            parse_thread.join();
        }
        if (builtins_thread.joinable()) {
            builtins_thread.join();
        }
    }

    void NariLspServer::run() {
        lsp_log("run: starting init_builtins on background thread");
        builtins_thread = std::thread([this]() {
            init_builtins();
            lsp_log("run: init_builtins thread finished");
        });
        // Start the dedicated background parse thread.
        parse_thread = std::thread([this]() { parse_thread_func(); });
        lsp_log("run: entering main loop");
        while (!this->exit) {
            std::string body;
            if (!nari::protocol::read_message(std::cin, body)) {
                break;
            }
            json msg = json::parse(body);
            if (msg.is_object()) {
                dispatch(msg);
            }
        }
    }

    void NariLspServer::dispatch(const json &msg) {
        const std::string method = msg.value("method", std::string{});
        const json id = msg.contains("id") ? msg["id"] : json(nullptr);
        const json params = msg.contains("params") ? msg["params"] : json::object();

        lsp_log("dispatch: " + method);

        if (method == "initialize") {
            handle_initialize(id);
        } else if (method == "initialized") { /* no-op */
        } else if (method == "shutdown") {
            handle_shutdown(id);
        } else if (method == "exit") {
            exit = true;
        } else if (method == "textDocument/didOpen") {
            handle_did_open(params);
        } else if (method == "textDocument/didChange") {
            handle_did_change(params);
        } else if (method == "textDocument/didClose") {
            handle_did_close(params);
        } else if (method == "textDocument/completion") {
            handle_completion(id, params);
        } else if (method == "textDocument/hover") {
            handle_hover(id, params);
        } else if (method == "textDocument/signatureHelp") {
            handle_signature_help(id, params);
        } else if (method == "textDocument/definition") {
            handle_definition(id, params);
        } else if (method == "textDocument/references") {
            handle_references(id, params);
        } else if (method == "textDocument/typeDefinition") {
            handle_type_definition(id, params);
        } else if (method == "textDocument/codeAction") {
            handle_code_action(id, params);
        } else if (method == "textDocument/inlayHint") {
            handle_inlay_hints(id, params);
        } else if (method == "textDocument/semanticTokens/full") {
            handle_semantic_tokens_full(id, params);
        } else if (!id.is_null()) {
            send_error(id, -32601, "Method not found: " + method);
        }
    }

    void NariLspServer::send_response(const json &id, json result) {
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"] = id;
        resp["result"] = std::move(result);
        write_lsp_message(resp.dump());
    }

    void NariLspServer::send_error(const json &id, int code, const std::string &msg) {
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"] = id;
        resp["error"]["code"] = code;
        resp["error"]["message"] = msg;
        write_lsp_message(resp.dump());
    }

    void NariLspServer::send_notification(const std::string &method, json params) {
        json notif;
        notif["jsonrpc"] = "2.0";
        notif["method"] = method;
        notif["params"] = std::move(params);
        write_lsp_message(notif.dump());
    }

int main(int, char **) {
    nari::protocol::use_binary_stdio();
    // Disable stdio sync for maximum throughput on the LSP pipe
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    init_logger();

    NariLspServer server;
    server.run();
    return 0;
}
