// HTTP builtins (split out of core.cpp).
#include "common.h"

#include <climits>
#include <cmath>

namespace chrono = std::chrono;

#ifndef DISABLE_HTTP
// Like builtin_http_get but accepts an options object { url, method, headers, body }
Value ScriptRuntime::builtin_http_fetch(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc < 2) {
        return Value::none();
    }

    std::string url = argvals[0].to_string();
    std::string method = "GET";
    StringMap headers;
    std::string body;
    auto http_op = std::make_shared<HttpOperation>();

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

    if (argvals[1].is_object()) {
        const ObjectObj *opts = argvals[1].get_obj_ptr();
        auto limit = [&](const char *name, int64_t maximum, int64_t &value) {
            const Value *option = opts->get_field(name);
            if (!option || option->is_none()) { return true; }
            if (option->is_int()) {
                value = option->get_int();
                return value >= 0 && value <= maximum;
            }
            if (!option->is_float()) { return false; }
            double number = option->get_float();
            if (!std::isfinite(number) || number < 0 || number >= 0x1p63 || number > maximum || std::floor(number) != number) { return false; }
            value = static_cast<int64_t>(number);
            return true;
        };
        int64_t timeout = 0;
        if (!limit("max_bytes", INT64_MAX, http_op->max_response_bytes) || !limit("timeout_ms", LONG_MAX, timeout)) {
            return runtime_fatal("TypeError: HTTP limits must be nonnegative whole numbers in range", call);
        }
        http_op->timeout_ms = static_cast<long>(timeout);
        if (const Value *follow = opts->get_field("follow_redirects"); follow && !follow->is_none()) {
            if (!follow->is_bool()) { return runtime_fatal("TypeError: follow_redirects must be a boolean", call); }
            http_op->follow_redirects = follow->get_bool();
        }
    }
    http_op->full_url = url;
    http_op->method = method;
    http_op->body = body;
    http_op->headers = headers;

    auto handle = Value::make_handle_ptr();
    handle->state = HandleData::Running;

    http_op->callback = [this, handle, http_op = http_op.get()]() {
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

    submit_handle_io(http_op, handle);
    return Value::make_handle(handle);
}
#endif // !DISABLE_HTTP
