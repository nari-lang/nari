// HTTP builtins (split out of core.cpp).
#include "common.h"

#include <climits>

namespace chrono = std::chrono;

#ifndef DISABLE_HTTP
// HTTP builtins
// stoi-compatible port parse that reports failure by return value instead of
// throwing: optional sign, ASCII digits, int range; stops at the first
// non-digit like std::stoi does.
static bool parse_port_number(const std::string &text, int &out) {
    size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
        ++i;
    }
    bool negative = false;
    if (i < text.size() && (text[i] == '-' || text[i] == '+')) {
        negative = (text[i] == '-');
        ++i;
    }
    size_t digits = i;
    int v = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
        int d = text[i] - '0';
        if (negative) {
            if (v < (INT_MIN + d) / 10) {
                return false;
            }
            v = v * 10 - d;
        } else {
            if (v > (INT_MAX - d) / 10) {
                return false;
            }
            v = v * 10 + d;
        }
        ++i;
    }
    if (digits == i) {
        return false;
    }
    out = v;
    return true;
}

// Like builtin_http_get but accepts an options object { url, method, headers, body }
Value ScriptRuntime::builtin_http_fetch(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc < 2) {
        return Value::none();
    }

    std::string url = argvals[0].to_string();
    std::string method = "GET";
    StringMap headers;
    std::string body;

    if (argvals[1].is_object()) {
        const ObjectObj *opts = argvals[1].get_obj_ptr();
        const Value *method_v = opts->get_field("method");
        if (method_v) {
            method = method_v->to_string();
        }

        const Value *headers_v = opts->get_field("headers");
        if (headers_v && headers_v->is_object()) {
            const ObjectObj *hdrs = headers_v->get_obj_ptr();
            for (const auto &name : hdrs->get_keys()) {
                if (const Value *val = hdrs->get_field(name)) {
                    headers[name] = val->to_string();
                }
            }
        }

        const Value *body_v = opts->get_field("body");
        if (body_v && !body_v->is_none()) {
            body = body_v->to_string();
        }
    }

    std::string full_url = url;
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

    size_t port_pos = host_port.find(':');
    if (port_pos != std::string::npos) {
        host = host_port.substr(0, port_pos);
        if (!parse_port_number(host_port.substr(port_pos + 1), port)) {
            return runtime_fatal("TypeError: invalid port in URL: " + host_port, call);
        }
    } else {
        host = host_port;
    }

    auto http_op = std::make_shared<HttpOperation>();
    http_op->host = host;
    http_op->port = port;
    http_op->url_path = path;
    http_op->full_url = full_url;
    http_op->method = method;
    http_op->body = body;
    http_op->headers = headers;

    auto handle = Value::make_handle_ptr();
    handle->state = HandleData::Running;

    http_op->callback = [this, handle, http_op]() {
        handle->end_time = chrono::steady_clock::now();
        if (http_op->success) {
            auto response = Value::make_object();
            ObjectObj *resp_oobj = response.get_obj_ptr();
            resp_oobj->set_field("status_code", Value::make_int(http_op->status_code));
            resp_oobj->set_field("body", Value::make_string(http_op->result_string));
            auto resp_headers = Value::make_object();
            ObjectObj *hdrs_oobj = resp_headers.get_obj_ptr();
            for (const auto &[k, v] : http_op->response_headers) {
                hdrs_oobj->set_field(k, Value::make_string(v));
            }
            resp_oobj->set_field("headers", resp_headers);
            handle->result = ScriptRuntime::make_ok(response);
            handle->state = HandleData::Completed;
        } else {
            handle->result = ScriptRuntime::make_err(Value::make_string(http_op->error_msg));
            handle->state = HandleData::Completed;
        }
        // Free large response data, process_completed_io will null this callback.
        http_op->result_string = {};
        http_op->response_headers.clear();
    };

    if (io_pool) {
        io_pool->submit(http_op);
    }
    return Value::make_handle(handle);
}
#endif // !DISABLE_HTTP
