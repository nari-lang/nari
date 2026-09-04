#include "dap_server.h"

#include "bytecode.h"
#include "bytecode_serializer.h"
#include "core_types.h"
#include "debugger.h"
#include "nari_fs.h"
#include "parser_api.h"
#include "runtime.h"

#include "thirdparty/json.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

using json = nlohmann::json;

namespace nari {
namespace dap {

static bytecode::VM *g_current_vm = nullptr;

namespace {

std::mutex g_out_mtx;

// id used for server-initiated events.
int g_seq = 1;

// If the NARI_DAP_LOG environment variable points to a writable path,
// every incoming request and outgoing message is appended there
static FILE *g_log = nullptr;
static std::mutex g_log_mu;

void log_init() {
    const char *path = std::getenv("NARI_DAP_LOG");
    if (path && *path) {
        g_log = std::fopen(path, "a");
        if (g_log) {
            std::fprintf(g_log, "\n==== dap session start (pid=%d) ====\n", (int)getpid());
            std::fflush(g_log);
        }
    }
}

void log_line(const char *prefix, const std::string &body) {
    if (!g_log) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_log_mu);
    std::fprintf(g_log, "%s %s\n", prefix, body.c_str());
    std::fflush(g_log);
}

void write_message(const json &msg) {
    std::string body = msg.dump();
    log_line(">>", body);
    std::lock_guard<std::mutex> lock(g_out_mtx);
    std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    std::cout.flush();
}

void send_response(
    int request_seq, const std::string &command, bool success, json body = json::object(), const std::string &msg = std::string()
) {

    json resp;
    resp["seq"] = g_seq++;
    resp["type"] = "response";
    resp["request_seq"] = request_seq;
    resp["command"] = command;
    resp["success"] = success;
    if (!body.is_null()) {
        resp["body"] = body;
    }
    if (!msg.empty()) {
        resp["message"] = msg;
    }
    write_message(resp);
}

void send_event(const std::string &event, json body = json::object()) {
    json evt;
    evt["seq"] = g_seq++;
    evt["type"] = "event";
    evt["event"] = event;
    if (!body.is_null()) {
        evt["body"] = body;
    }
    write_message(evt);
}

void send_output(const std::string &category, const std::string &text) {
    json body;
    body["category"] = category;
    body["output"] = text;
    send_event("output", body);
}

bool read_message(std::string &out_body) {
    std::string line;
    size_t content_length = 0;
    while (true) {
        if (!std::getline(std::cin, line)) {
            return false;
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            break;
        }
        static const std::string cl = "Content-Length:";
        if (line.size() > cl.size() && line.compare(0, cl.size(), cl) == 0) {
            content_length = static_cast<size_t>(std::strtoull(line.c_str() + cl.size(), nullptr, 10));
        }
    }
    if (content_length == 0) {
        return false;
    }
    out_body.resize(content_length);
    std::cin.read(&out_body[0], static_cast<std::streamsize>(content_length));
    return static_cast<size_t>(std::cin.gcount()) == content_length;
}

// DAP runtime state
struct Server {
    std::vector<std::string> argv;
    std::string script_path;

    bool launch_received = false;
    bool configuration_done = false;
    bool stop_on_entry = true;
    std::thread vm_thread;

    // frame-id -> index into the current StopSnapshot.frames vector, refs are rebuilt on each stop.
    std::vector<int> current_frame_ids; // index -> frame id
    // variablesReference -> either a frame-local scope or a child value view.
    struct ScopeRef {
        enum class Kind {
            FrameLocals,
            ValueChildren,
        };

        Kind kind = Kind::FrameLocals;
        int frame_index = -1;
        Value value = Value::none();
    };
    std::vector<ScopeRef> scope_refs;
    int next_var_ref = 1000;

    bytecode::Chunk *chunk = nullptr; // chunk owned here, freed on shutdown

    ~Server() {
        if (vm_thread.joinable()) {
            vm_thread.join();
        }
        delete chunk;
    }
};

// Source-line collection for a single script load. Populated after we build the chunk,
// so setBreakpoints can clamp client-requested lines to lines the compiler actually emitted code for.
struct SourceLineIndex {
    // canonical path -> set of lines that have >=1 bytecode instruction.
    std::unordered_map<std::string, std::set<int>> lines_by_file;

    void ingest(const bytecode::Chunk &c) {
        for (const auto &fn : c.functions) {
            if (fn.source_file.empty()) {
                continue;
            }
            std::string k = dbg::canonicalise_path(fn.source_file);
            auto &set = lines_by_file[k];
            for (const auto &e : fn.line_map) {
                if (e.line > 0) {
                    set.insert(e.line);
                }
            }
        }
    }

    // map a client-requested line to the nearest line the compiler actually
    // emitted at >= requested. Returns -1 if no such line exists.
    int resolve(const std::string &path, int line) const {
        auto it = lines_by_file.find(path);
        if (it == lines_by_file.end()) {
            return -1;
        }
        auto lb = it->second.lower_bound(line);
        if (lb == it->second.end()) {
            return -1;
        }
        return *lb;
    }
};

SourceLineIndex g_line_index;

// TODO: deduplicate this
// Load script -> Chunk, same as the interpreter's load path but with logs to stderr stuff removed
static bytecode::Chunk *compile_source_for_debug(const std::string &path) {
    std::ifstream f(path);
    if (!f) {
        return nullptr;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string source = ss.str();
    try {
        // Set the parser's thread-local filename so every AST node carries the absolute path.
        Parser::set_source_filename(path);
        auto funcs = Parser::parse_program_from_source(source);
        auto *chunk = bytecode::compile_bytecode(funcs);
        if (g_log && chunk) {
            log_line("--", "compiled " + std::to_string(chunk->functions.size()) + " functions:");
            for (const auto &fn : chunk->functions) {
                log_line(
                    "  ", std::string("name='") + fn.name + "' source='" + fn.source_file +
                              "' line_map_size=" + std::to_string(fn.line_map.size())
                );
            }
        }
        return chunk;
    } catch (...) {
        return nullptr;
    }
}

static bytecode::Chunk *load_naric(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return nullptr;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string data = ss.str();
    return bytecode::BytecodeSerializer::deserialize(reinterpret_cast<const uint8_t *>(data.data()), data.size());
}

static bytecode::Chunk *load_program(const std::string &path) {
    bool is_naric = path.size() >= 6 && path.compare(path.size() - 6, 6, ".naric") == 0;
    if (is_naric) {
        return load_naric(path);
    }
    return compile_source_for_debug(path);
}

static void prime_parser_state_for_vm_thread(const std::string &path) {
    bool is_naric = path.size() >= 6 && path.compare(path.size() - 6, 6, ".naric") == 0;
    if (is_naric) {
        return;
    }

    std::ifstream f(path);
    if (!f) {
        return;
    }

    std::stringstream ss;
    ss << f.rdbuf();
    const std::string source = ss.str();

    Parser::reset_parse_session();
    Parser::set_source_filename(path);
    (void)Parser::parse_program_from_source(source);
}

static int run_vm(bytecode::Chunk *chunk, const std::vector<std::string> &argv) {
    if (!argv.empty()) {
        prime_parser_state_for_vm_thread(argv.front());
    }

    // Convert argv into the VM's (int, char**) form. Pointers are owned by `arg_storage`.
    std::vector<std::string> arg_storage = argv;
    std::vector<char *> c_argv;
    c_argv.reserve(arg_storage.size());
    for (auto &s : arg_storage) {
        c_argv.push_back(const_cast<char *>(s.c_str()));
    }

    int rc = 0;
    try {
        bytecode::VM vm(static_cast<int>(c_argv.size()), c_argv.empty() ? nullptr : c_argv.data());
        g_current_vm = &vm;
        vm.runtime->stdout_writer = [](const std::string &text) { send_output("stdout", text); };
        if (!vm.run(chunk)) {
            rc = 1;
        }
        g_current_vm = nullptr;
    } catch (const std::exception &e) {
        g_current_vm = nullptr;
        send_output("stderr", std::string(e.what()) + "\n");
        rc = 1;
    } catch (...) {
        g_current_vm = nullptr;
        rc = 1;
    }
    return rc;
}
// DAP request handlers
static void handle_initialize(Server &server, int seq, const json &args) {
    json caps;
    caps["supportsConfigurationDoneRequest"] = true;
    caps["supportsFunctionBreakpoints"] = false;
    caps["supportsConditionalBreakpoints"] = false;
    caps["supportsEvaluateForHovers"] = true;
    caps["supportsStepBack"] = false;
    caps["supportsSetVariable"] = false;
    caps["supportsRestartRequest"] = false;
    caps["supportsTerminateRequest"] = true;
    caps["supportsDisassembleRequest"] = false;
    send_response(seq, "initialize", true, caps);
    // Tell the client we're ready to receive setBreakpoints etc.
    send_event("initialized");
}

static void handle_launch(Server &server, int seq, const json &args) {
    std::string path = args.value("program", std::string());
    if (path.empty()) {
        path = server.script_path;
    }
    if (path.empty()) {
        send_response(seq, "launch", false, json::object(), "no program specified");
        return;
    }
    server.script_path = path;
    server.stop_on_entry = args.value("stopOnEntry", true);
    if (args.contains("args") && args["args"].is_array()) {
        server.argv.clear();
        server.argv.push_back(path);
        for (const auto &a : args["args"]) {
            if (a.is_string()) {
                server.argv.push_back(a.get<std::string>());
            }
        }
    } else if (server.argv.empty()) {
        server.argv.push_back(path);
    }

    server.chunk = load_program(path);
    if (!server.chunk) {
        send_response(seq, "launch", false, json::object(), "failed to load/compile program: " + path);
        send_event("terminated");
        return;
    }
    g_line_index.ingest(*server.chunk);

    auto &dc = dbg::DebugController::instance();
    dc.set_enabled(true);
    dc.set_stop_on_entry(server.stop_on_entry);

    // Stop listener: a DAP `stopped` event for each VM pause.
    dc.set_stop_listener([&server](const dbg::StopSnapshot &snap) {
        // Rebuild frame id table so stackTrace sees current frames.
        server.current_frame_ids.clear();
        server.scope_refs.clear();
        for (size_t i = 0; i < snap.frames.size(); i++) {
            server.current_frame_ids.push_back(static_cast<int>(i) + 1);
        }
        json body;
        body["threadId"] = 1;
        switch (snap.reason) {
            case dbg::StopReason::Entry:
                body["reason"] = "entry";
                break;
            case dbg::StopReason::Breakpoint:
                body["reason"] = "breakpoint";
                break;
            case dbg::StopReason::Step:
                body["reason"] = "step";
                break;
            case dbg::StopReason::Pause:
                body["reason"] = "pause";
                break;
            case dbg::StopReason::Exception:
                body["reason"] = "exception";
                break;
        }
        body["allThreadsStopped"] = true;
        send_event("stopped", body);
    });

    dc.set_exit_listener([](int code) {
        json body;
        body["exitCode"] = code;
        send_event("exited", body);
        send_event("terminated");
    });

    send_response(seq, "launch", true);
    // We start the VM only after configurationDone so the client has a chance to send its breakpoints first.
    server.launch_received = true;
}

static void handle_set_breakpoints(Server &server, int seq, const json &args) {
    std::string path;
    if (args.contains("source") && args["source"].is_object()) {
        path = args["source"].value("path", std::string());
    }
    path = dbg::canonicalise_path(path);

    std::vector<int> requested_lines;
    if (args.contains("breakpoints") && args["breakpoints"].is_array()) {
        for (const auto &b : args["breakpoints"]) {
            if (b.contains("line") && b["line"].is_number_integer()) {
                requested_lines.push_back(b["line"].get<int>());
            }
        }
    } else if (args.contains("lines") && args["lines"].is_array()) {
        for (const auto &l : args["lines"]) {
            if (l.is_number_integer()) {
                requested_lines.push_back(l.get<int>());
            }
        }
    }

    // setBreakpoints can race `launch`, so the line index may be empty.
    json verified = json::array();
    for (int wanted : requested_lines) {
        json bp;
        bp["verified"] = true;
        bp["line"] = wanted;
        verified.push_back(bp);
    }

    dbg::DebugController::instance().set_breakpoints(path, requested_lines);

    json body;
    body["breakpoints"] = verified;
    send_response(seq, "setBreakpoints", true, body);
}

static void handle_configuration_done(Server &server, int seq, const json &) {
    server.configuration_done = true;
    send_response(seq, "configurationDone", true);

    // Start the VM thread now that the client is done configuring.
    if (server.launch_received && server.chunk && !server.vm_thread.joinable()) {
        bytecode::Chunk *chunk = server.chunk; // the VM owns the chunk during run()
        std::vector<std::string> vm_argv = server.argv;
        server.vm_thread = std::thread([chunk, vm_argv]() {
            int rc = run_vm(chunk, vm_argv);
            dbg::DebugController::instance().notify_exited(rc);
        });
    }
}

static void handle_threads(Server &server, int seq, const json &) {
    // Nari is single-threaded; report one thread with id 1.
    json body;
    json threads = json::array();
    json t;
    t["id"] = 1;
    t["name"] = "main";
    threads.push_back(t);
    body["threads"] = threads;
    send_response(seq, "threads", true, body);
}

static void handle_stack_trace(Server &server, int seq, const json &) {
    auto snap = dbg::DebugController::instance().last_snapshot();
    json frames = json::array();
    for (size_t i = 0; i < snap.frames.size(); i++) {
        const auto &f = snap.frames[i];
        json fr;
        fr["id"] = static_cast<int>(i) + 1; // matches the id we handed out on stop
        fr["name"] = f.name;
        fr["line"] = f.line;
        fr["column"] = 1;
        if (!f.source_file.empty()) {
            json src;
            src["name"] = nari::fs::Path(f.source_file).filename().string();
            src["path"] = f.source_file;
            fr["source"] = src;
        }
        frames.push_back(fr);
    }
    json body;
    body["stackFrames"] = frames;
    body["totalFrames"] = frames.size();
    send_response(seq, "stackTrace", true, body);
}

static int allocate_var_ref(Server &server, Server::ScopeRef ref);

static void handle_scopes(Server &server, int seq, const json &args) {
    int frame_id = args.value("frameId", 1);
    int frame_index = frame_id - 1;
    json scopes = json::array();
    if (frame_index >= 0 && frame_index < static_cast<int>(server.current_frame_ids.size())) {
        // Allocate a fresh variablesReference for the "locals" scope of this frame.
        Server::ScopeRef ref_data;
        ref_data.kind = Server::ScopeRef::Kind::FrameLocals;
        ref_data.frame_index = frame_index;
        int ref = allocate_var_ref(server, std::move(ref_data));
        json scope;
        scope["name"] = "Locals";
        scope["variablesReference"] = ref;
        scope["expensive"] = false;
        scopes.push_back(scope);
    }
    json body;
    body["scopes"] = scopes;
    send_response(seq, "scopes", true, body);
}

// Render a Value into a short human-readable string for DAP variable display.
static std::string format_value(const Value &val) {
    if (val.is_none()) {
        return "none";
    }
    if (val.is_bool()) {
        return val.get_bool() ? "true" : "false";
    }
    if (val.is_int()) {
        return std::to_string(val.get_int());
    }
    if (val.is_float()) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%g", val.get_float());
        return buf;
    }
    if (val.is_string()) {
        std::string str = val.get_string();
        if (str.size() > 200) {
            str = str.substr(0, 200) + "...";
        }
        return "\"" + str + "\"";
    }
    if (val.is_array()) {
        return "[array len=" + std::to_string(val.get_array().size()) + "]";
    }
    if (val.is_object()) {
        return "{object}";
    }
    if (val.is_class_instance()) {
        return val.to_string();
    }
    if (val.is_function()) {
        return "<function>";
    }
    if (val.is_regex() || val.is_handle()) {
        return val.to_string();
    }
    return val.to_string();
}

static std::string value_type_name(const Value &val) {
    if (val.is_none()) {
        return "none";
    }
    if (val.is_bool()) {
        return "bool";
    }
    if (val.is_int()) {
        return "int";
    }
    if (val.is_float()) {
        return "float";
    }
    if (val.is_string()) {
        return "string";
    }
    if (val.is_array()) {
        return "array";
    }
    if (val.is_object()) {
        return "object";
    }
    if (val.is_class_instance()) {
        return val.get_class_instance()->class_name;
    }
    if (val.is_function()) {
        return "function";
    }
    if (val.is_regex()) {
        return "regex";
    }
    if (val.is_handle()) {
        return "handle";
    }
    return "";
}

static bool has_expandable_children(const Value &val) {
    if (val.is_class_instance()) {
        const auto *instance = val.get_class_instance();
        return instance && instance->layout && !instance->layout->names.empty();
    }
    if (val.is_array()) {
        return !val.get_array().empty();
    }
    if (val.is_object()) {
        const auto *obj = val.get_obj_ptr();
        return obj && !obj->is_empty();
    }
    return false;
}

static int allocate_var_ref(Server &server, Server::ScopeRef ref) {
    int ref_id = server.next_var_ref++;
    if (static_cast<int>(server.scope_refs.size()) < ref_id - 999) {
        server.scope_refs.resize(ref_id - 999);
    }
    server.scope_refs[ref_id - 1000] = std::move(ref);
    return ref_id;
}

static int child_variables_reference(Server &server, const Value &val) {
    if (!has_expandable_children(val)) {
        return 0;
    }
    Server::ScopeRef ref;
    ref.kind = Server::ScopeRef::Kind::ValueChildren;
    ref.value = val;
    return allocate_var_ref(server, std::move(ref));
}

static void append_variable(json &vars, Server &server, const std::string &name, const Value &value) {
    json var;
    var["name"] = name;
    var["value"] = format_value(value);
    var["type"] = value_type_name(value);
    var["variablesReference"] = child_variables_reference(server, value);
    vars.push_back(std::move(var));
}

static int frame_index_for_id(int frame_id, size_t frame_count) {
    int frame_index = frame_id - 1;
    if (frame_index < 0 || frame_index >= static_cast<int>(frame_count)) {
        return -1;
    }
    return frame_index;
}

static bool lookup_frame_value(const dbg::FrameSnapshot &frame, const std::string &name, Value &out) {
    if (frame.meta && g_current_vm != nullptr) {
        const auto &names = frame.meta->var_names;
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i] != name) {
                continue;
            }
            size_t stack_idx = frame.stack_slot_base + i;
            if (stack_idx >= g_current_vm->stack.size()) {
                return false;
            }
            out = g_current_vm->stack[stack_idx];
            return true;
        }
    } else {
        for (const auto &[local_name, value] : frame.locals) {
            if (local_name != name) {
                continue;
            }
            out = value;
            return true;
        }
    }
    return false;
}

static bool skip_ws(const std::string &expr, size_t &i) {
    while (i < expr.size() && std::isspace((unsigned char)expr[i])) {
        ++i;
    }
    return i < expr.size();
}

static bool parse_ident_token(const std::string &expr, size_t &i, std::string &out) {
    skip_ws(expr, i);
    if (i >= expr.size() || !(std::isalpha((unsigned char)expr[i]) || expr[i] == '_')) {
        return false;
    }
    size_t start = i++;
    while (i < expr.size() && (std::isalnum((unsigned char)expr[i]) || expr[i] == '_')) {
        ++i;
    }
    out.assign(expr, start, i - start);
    return true;
}

static bool parse_string_key(const std::string &expr, size_t &i, std::string &out) {
    skip_ws(expr, i);
    if (i >= expr.size() || (expr[i] != '"' && expr[i] != '\'')) {
        return false;
    }
    char quote = expr[i++];
    std::string result;
    while (i < expr.size() && expr[i] != quote) {
        if (expr[i] == '\\' && i + 1 < expr.size()) {
            ++i;
        }
        result.push_back(expr[i++]);
    }
    if (i >= expr.size() || expr[i] != quote) {
        return false;
    }
    ++i;
    out = std::move(result);
    return true;
}

static bool parse_int_index(const std::string &expr, size_t &i, int64_t &out) {
    skip_ws(expr, i);
    size_t start = i;
    if (i < expr.size() && (expr[i] == '-' || expr[i] == '+')) {
        ++i;
    }
    size_t digits = i;
    while (i < expr.size() && std::isdigit((unsigned char)expr[i])) {
        ++i;
    }
    if (digits == i) {
        return false;
    }
    try {
        out = std::stoll(expr.substr(start, i - start));
        return true;
    } catch (...) {
        return false;
    }
}

static bool resolve_member_value(const Value &base, const std::string &member, Value &out) {
    if (base.is_class_instance()) {
        const Value *field = base.get_class_instance()->get_field(member);
        if (!field) {
            return false;
        }
        out = *field;
        return true;
    }
    if (base.is_object()) {
        const Value *field = base.get_obj_ptr()->get_field(member);
        if (!field) {
            return false;
        }
        out = *field;
        return true;
    }
    if (base.is_array() && member == "length") {
        out = Value::make_int(static_cast<int64_t>(base.get_array().size()));
        return true;
    }
    if (base.is_string() && member == "length") {
        out = Value::make_int(static_cast<int64_t>(base.get_string().size()));
        return true;
    }
    if (base.is_handle()) {
        const auto *handle = base.get_handle();
        if (!handle) {
            return false;
        }
        if (member == "ready") {
            out = Value::make_bool(handle->state != HandleData::Running);
            return true;
        }
        if (member == "failed") {
            out = Value::make_bool(handle->state == HandleData::Failed);
            return true;
        }
        if (member == "error") {
            out = handle->error;
            return true;
        }
        if (member == "await") {
            out = handle->result;
            return true;
        }
    }
    return false;
}

static bool resolve_index_value(const Value &base, const Value &index, Value &out) {
    if (base.is_array()) {
        if (!index.is_int()) {
            return false;
        }
        int64_t idx = index.get_int();
        const auto &arr = base.get_array();
        if (idx < 0 || idx >= static_cast<int64_t>(arr.size())) {
            return false;
        }
        out = arr[static_cast<size_t>(idx)];
        return true;
    }
    if (base.is_object()) {
        const Value *field = base.get_obj_ptr()->get_field(index.to_string());
        if (!field) {
            return false;
        }
        out = *field;
        return true;
    }
    if (base.is_class_instance()) {
        const Value *field = base.get_class_instance()->get_field(index.to_string());
        if (!field) {
            return false;
        }
        out = *field;
        return true;
    }
    if (base.is_string()) {
        if (!index.is_int()) {
            return false;
        }
        int64_t idx = index.get_int();
        const auto &str = base.get_string();
        if (idx < 0 || idx >= static_cast<int64_t>(str.size())) {
            return false;
        }
        out = Value::make_string(std::string(1, str[static_cast<size_t>(idx)]));
        return true;
    }
    return false;
}

static bool evaluate_debug_expression(Server &server, const json &args, Value &out, std::string &error) {
    if (g_current_vm == nullptr || !dbg::DebugController::instance().is_stopped()) {
        error = "program is not paused";
        return false;
    }

    std::string expr = args.value("expression", std::string{});
    if (expr.empty()) {
        error = "empty expression";
        return false;
    }

    int frame_id = args.value("frameId", 1);
    auto snap = dbg::DebugController::instance().last_snapshot();
    int frame_index = frame_index_for_id(frame_id, snap.frames.size());
    if (frame_index < 0) {
        error = "invalid frame";
        return false;
    }
    const dbg::FrameSnapshot &frame = snap.frames[static_cast<size_t>(frame_index)];

    size_t i = 0;
    std::string ident;
    if (!parse_ident_token(expr, i, ident)) {
        error = "unsupported expression";
        return false;
    }
    if (!lookup_frame_value(frame, ident, out)) {
        error = "unknown identifier: " + ident;
        return false;
    }

    while (true) {
        skip_ws(expr, i);
        if (i >= expr.size()) {
            break;
        }

        if (expr[i] == '.') {
            ++i;
            std::string member;
            if (!parse_ident_token(expr, i, member)) {
                error = "expected member name after '.'";
                return false;
            }
            Value next;
            if (!resolve_member_value(out, member, next)) {
                error = "cannot resolve member: " + member;
                return false;
            }
            out = std::move(next);
            continue;
        }

        if (expr[i] == '[') {
            ++i;
            Value index;
            std::string key;
            int64_t idx = 0;
            if (parse_string_key(expr, i, key)) {
                index = Value::make_string(key);
            } else if (parse_int_index(expr, i, idx)) {
                index = Value::make_int(idx);
            } else {
                std::string nested_ident;
                if (!parse_ident_token(expr, i, nested_ident) || !lookup_frame_value(frame, nested_ident, index)) {
                    error = "unsupported index expression";
                    return false;
                }
            }
            skip_ws(expr, i);
            if (i >= expr.size() || expr[i] != ']') {
                error = "expected ']'";
                return false;
            }
            ++i;
            Value next;
            if (!resolve_index_value(out, index, next)) {
                error = "cannot resolve index access";
                return false;
            }
            out = std::move(next);
            continue;
        }

        error = "unsupported expression";
        return false;
    }

    return true;
}

static void handle_evaluate(Server &server, int seq, const json &args) {
    Value value;
    std::string error;
    if (!evaluate_debug_expression(server, args, value, error)) {
        send_response(seq, "evaluate", false, json::object(), error);
        return;
    }

    json body;
    body["result"] = format_value(value);
    body["type"] = value_type_name(value);
    body["variablesReference"] = child_variables_reference(server, value);
    send_response(seq, "evaluate", true, std::move(body));
}

// variables request needs to read live VM state.
// It's only valid to call this while the VM is paused (stopped_ == true),
// DAP clients follow that protocol but this also guards against it
static void handle_variables(Server &server, int seq, const json &args) {
    int ref = args.value("variablesReference", 0);
    int idx = ref - 1000;
    if (idx < 0 || idx >= static_cast<int>(server.scope_refs.size()) || g_current_vm == nullptr) {
        send_response(seq, "variables", true, json::object({ { "variables", json::array() } }));
        return;
    }
    const auto &scope = server.scope_refs[idx];
    json vars = json::array();
    if (scope.kind == Server::ScopeRef::Kind::FrameLocals) {
        auto snap = dbg::DebugController::instance().last_snapshot();
        if (scope.frame_index < 0 || scope.frame_index >= static_cast<int>(snap.frames.size())) {
            send_response(seq, "variables", true, json::object({ { "variables", json::array() } }));
            return;
        }
        const auto &frame = snap.frames[scope.frame_index];
        if (frame.meta) {
            const auto &names = frame.meta->var_names;
            for (size_t i = 0; i < names.size(); i++) {
                size_t stack_idx = frame.stack_slot_base + i;
                if (stack_idx >= g_current_vm->stack.size()) {
                    break;
                }
                append_variable(vars, server, names[i], g_current_vm->stack[stack_idx]);
            }
        } else {
            for (const auto &[name, value] : frame.locals) {
                append_variable(vars, server, name, value);
            }
        }
    } else if (scope.value.is_class_instance()) {
        const auto *instance = scope.value.get_class_instance();
        if (instance && instance->layout) {
            for (size_t i = 0; i < instance->layout->names.size() && i < instance->field_values.size(); ++i) {
                append_variable(vars, server, instance->layout->names[i], instance->field_values[i]);
            }
        }
    } else if (scope.value.is_array()) {
        const auto &items = scope.value.get_array();
        for (size_t i = 0; i < items.size(); ++i) {
            append_variable(vars, server, "[" + std::to_string(i) + "]", items[i]);
        }
    } else if (scope.value.is_object()) {
        const auto *obj = scope.value.get_obj_ptr();
        if (obj) {
            for (const auto &name : obj->get_keys()) {
                if (const Value *val = obj->get_field(name)) {
                    append_variable(vars, server, name, *val);
                }
            }
        }
    }
    send_response(seq, "variables", true, json::object({ { "variables", vars } }));
}

// Main dispatch loop.
static void dispatch(Server &server, const json &msg) {
    if (msg.value("type", std::string()) != "request") {
        return;
    }
    int seq = msg.value("seq", 0);
    std::string cmd = msg.value("command", std::string());
    json args = msg.value("arguments", json::object());

    if (cmd == "initialize") {
        handle_initialize(server, seq, args);
    } else if (cmd == "launch") {
        handle_launch(server, seq, args);
    } else if (cmd == "setBreakpoints") {
        handle_set_breakpoints(server, seq, args);
    } else if (cmd == "configurationDone") {
        handle_configuration_done(server, seq, args);
    } else if (cmd == "threads") {
        handle_threads(server, seq, args);
    } else if (cmd == "stackTrace") {
        handle_stack_trace(server, seq, args);
    } else if (cmd == "scopes") {
        handle_scopes(server, seq, args);
    } else if (cmd == "variables") {
        handle_variables(server, seq, args);
    } else if (cmd == "evaluate") {
        handle_evaluate(server, seq, args);
    } else if (cmd == "continue") {
        dbg::DebugController::instance().cmd_continue();
        send_response(seq, "continue", true, json::object({ { "allThreadsContinued", true } }));
    } else if (cmd == "next") {
        dbg::DebugController::instance().cmd_next();
        send_response(seq, "next", true);
    } else if (cmd == "stepIn") {
        dbg::DebugController::instance().cmd_step_in();
        send_response(seq, "stepIn", true);
    } else if (cmd == "stepOut") {
        dbg::DebugController::instance().cmd_step_out();
        send_response(seq, "stepOut", true);
    } else if (cmd == "pause") {
        dbg::DebugController::instance().cmd_pause();
        send_response(seq, "pause", true);
    } else if (cmd == "setExceptionBreakpoints") {
        // Accepted but not implemented.
        send_response(seq, "setExceptionBreakpoints", true);
    } else if (cmd == "disconnect" || cmd == "terminate") {
        // Graceful shutdown: unblock the VM and let it wind down.
        auto &dc = dbg::DebugController::instance();
        dc.set_enabled(false);
        dc.cmd_continue(); // in case we're blocked inside publish_stop_and_wait
        send_response(seq, cmd, true);
        // Caller loop will exit after this via the flag.
        server.configuration_done = false;
    } else {
        send_response(seq, cmd, false, json::object(), "unsupported command: " + cmd);
    }
}

} // namespace

int run_dap_server(std::string initial_script, std::vector<std::string> argv) {
    log_init();
    Server server;
    server.script_path = std::move(initial_script);
    server.argv = std::move(argv);

    std::string body;
    while (read_message(body)) {
        log_line("<<", body);
        json msg;
        try {
            msg = json::parse(body);
        } catch (...) {
            continue;
        }
        dispatch(server, msg);

        std::string cmd = msg.value("command", std::string());
        if (cmd == "disconnect" || cmd == "terminate") {
            break;
        }
        if (dbg::DebugController::instance().has_exited() && !server.vm_thread.joinable()) {
            break;
        }
    }

    auto &dc = dbg::DebugController::instance();
    dc.set_enabled(false);
    dc.cmd_continue();

    if (server.vm_thread.joinable()) {
        server.vm_thread.join();
    }
    return dc.has_exited() ? dc.exit_code() : 0;
}

} // namespace dap
} // namespace nari
