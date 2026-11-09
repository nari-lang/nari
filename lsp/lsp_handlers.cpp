/*
  lsp_handlers.cpp
  NariLspServer request handlers split from lsp/lsp_server.cpp.
*/

#include "lsp_server.h"

#include <set>
#include <sstream>
#include <tuple>

// Semantic token type indices must match the legend emitted by handle_initialize.
static constexpr int ST_NAMESPACE = 0;
static constexpr int ST_TYPE = 1;
static constexpr int ST_CLASS = 2;
static constexpr int ST_ENUM = 3;
static constexpr int ST_PARAM = 7;
static constexpr int ST_VARIABLE = 8;
static constexpr int ST_FUNCTION = 12;
// Token modifier bits.
static constexpr int SM_DECLARATION = (1 << 0);


static const char *kNariKeywords[] = { "let",    "global",   "func",   "return",  "if",   "else", "while", "for",   "foreach", "in",
                                       "break",  "continue", "class",  "new",     "this", "type", "enum",  "match", "spawn",   "await",
                                       "import", "export",   "public", "private", "null", "true", "false", nullptr };

static const struct {
    const char *name;
    const char *detail;
} kNariBuiltins[] = { { "print", "func print(...)" },
                      { "panic", "func panic(value) -> never" },
                      { "to_string", "func to_string(value) -> string" },
                      { "to_number", "func to_number(value) -> number" },
                      { "to_bool", "func to_bool(value) -> bool" },
                      { "typeof", "func typeof(value) -> string" },
                      { "is_number", "func is_number(value) -> bool" },
                      { "is_string", "func is_string(value) -> bool" },
                      { "is_bool", "func is_bool(value) -> bool" },
                      { "is_array", "func is_array(value) -> bool" },
                      { "is_object", "func is_object(value) -> bool" },
                      { "is_function", "func is_function(value) -> bool" },
                      { "parse_int", "func parse_int(str, radix?) -> number" },
                      { "parse_float", "func parse_float(str) -> number" },
                      { "random", "func random() -> number" },
                      { "time", "func time() -> number" },
                      { "range", "func range(start, end, step?) -> number[]" },
                      { "set_timeout", "func set_timeout(fn, delay)" },
                      { "set_interval", "func set_interval(fn, interval) -> number" },
                      { "clear_interval", "func clear_interval(id)" },
                      { "read_line", "func read_line() -> string" },
                      { nullptr, nullptr } };

    void NariLspServer::handle_initialize(const json &id) {
        json result;
        result["capabilities"]["textDocumentSync"] = 1;
        result["capabilities"]["completionProvider"]["triggerCharacters"] = json::array({ "." });
        result["capabilities"]["completionProvider"]["resolveProvider"] = false;
        result["capabilities"]["hoverProvider"] = true;
        result["capabilities"]["signatureHelpProvider"]["triggerCharacters"] = json::array({ "(", "," });
        result["capabilities"]["definitionProvider"] = true;
        result["capabilities"]["referencesProvider"] = true;
        result["capabilities"]["typeDefinitionProvider"] = true;
        // Code actions: quickfix (unused vars) + refactor (wrap in try/catch)
        result["capabilities"]["codeActionProvider"]["codeActionKinds"] = json::array({ "quickfix", "refactor" });
        result["capabilities"]["codeActionProvider"]["resolveProvider"] = false;
        // Inlay hints (variable type annotations)
        result["capabilities"]["inlayHintProvider"] = true;
        // Semantic tokens (full document)
        {
            json legend;
            legend["tokenTypes"] =
                json::array({ "namespace", "type",     "class",      "enum",   "interface", "struct",  "typeParameter", "parameter",
                              "variable",  "property", "enumMember", "event",  "function",  "method",  "macro",         "keyword",
                              "modifier",  "comment",  "string",     "number", "regexp",    "operator" });
            legend["tokenModifiers"] = json::array({ "declaration", "definition", "readonly", "static", "deprecated", "abstract", "async",
                                                     "modification", "documentation", "defaultLibrary" });
            result["capabilities"]["semanticTokensProvider"]["legend"] = std::move(legend);
            result["capabilities"]["semanticTokensProvider"]["full"] = true;
        }
        result["serverInfo"]["name"] = "nari-lsp";
        result["serverInfo"]["version"] = "0.1.0";
        send_response(id, std::move(result));
    }

    void NariLspServer::handle_shutdown(const json &id) {
        shutdown = true;
        send_response(id, nullptr);
    }

    void NariLspServer::handle_did_open(const json &params) {
        const auto &_td0 = params.contains("textDocument") ? params["textDocument"] : json{};
        std::string uri = _td0.value("uri", std::string{});
        std::string content = _td0.value("text", std::string{});
        if (uri.empty()) {
            return;
        }

        const std::size_t h = std::hash<std::string>{}(content);
        {
            std::unique_lock<std::shared_timed_mutex> lock(docs_mutex);
            DocState &doc = docs[uri];
            doc.content = content;
            doc.content_hash = h;
            doc.parsed_hash = 0;
            doc.symbols.clear();
            doc.classes.clear();
            doc.object_literals.clear();
        }
        enqueue_parse(uri, content, h);
    }

    void NariLspServer::handle_did_change(const json &params) {
        const auto &_td1 = params.contains("textDocument") ? params["textDocument"] : json{};
        std::string uri = _td1.value("uri", std::string{});
        if (uri.empty()) {
            return;
        }
        if (!params.contains("contentChanges") || !params["contentChanges"].is_array() || params["contentChanges"].empty()) {
            return;
        }
        std::string content = params["contentChanges"].back().value("text", std::string{});
        // Skip the expensive re-parse if content is identical to what we already have.
        std::size_t h = std::hash<std::string>{}(content);
        {
            std::unique_lock<std::shared_timed_mutex> lock(docs_mutex);
            DocState &doc = docs[uri];
            if (doc.content_hash == h) {
                lsp_log("didChange: unchanged " + uri);
                return;
            }
            doc.content = content;
            doc.content_hash = h;
        }
        enqueue_parse(uri, content, h);
    }

    void NariLspServer::handle_did_close(const json &params) {
        const auto &_td2 = params.contains("textDocument") ? params["textDocument"] : json{};
        std::string uri = _td2.value("uri", std::string{});
        if (uri.empty()) {
            return;
        }
        {
            std::unique_lock<std::shared_timed_mutex> lock(docs_mutex);
            docs.erase(uri);
        }
        json p;
        p["uri"] = uri;
        p["diagnostics"] = json::array();
        send_notification("textDocument/publishDiagnostics", std::move(p));
    }

    void NariLspServer::handle_completion(const json &id, const json &params) {
        const auto &_td3 = params.contains("textDocument") ? params["textDocument"] : json{};
        const auto &_pos3 = params.contains("position") ? params["position"] : json{};
        std::string uri = _td3.value("uri", std::string{});
        int line = _pos3.value("line", 0);
        int col = _pos3.value("character", 0);
        // Hold a shared lock while reading docs (parse thread may be committing).
        std::shared_lock<std::shared_timed_mutex> docs_lock(docs_mutex);

        lsp_log("completion request: uri=" + uri + " line=" + std::to_string(line) + " col=" + std::to_string(col) + " doc_known=" +
                (docs.count(uri) ? "yes" : "no") + " symbols=" + (docs.count(uri) ? std::to_string(docs[uri].symbols.size()) : "n/a"));

        // Determine prefix at cursor position
        std::string prefix;
        int word_start_col = col;
        if (docs.count(uri)) {
            auto wp = word_at(docs[uri].content, line, col);
            prefix = wp.first;
            word_start_col = wp.second;
        }

        lsp_log("completion prefix: '" + prefix + "'");

        json items = json::array();

        // dot-completion: detect "receiver.prefix"
        // get the source line so we can inspect the character before the word.
        std::string src_line;
        if (docs.count(uri)) {
            std::istringstream _ss(docs[uri].content);
            std::string _ln;
            int _cur = 0;
            while (std::getline(_ss, _ln)) {
                if (_cur == line) {
                    src_line = _ln;
                    break;
                }
                _cur++;
            }
        }
        std::string receiver;
        {
            int dot_pos = word_start_col - 1;
            if (dot_pos >= 0 && dot_pos < static_cast<int>(src_line.size()) && src_line[dot_pos] == '.') {
                int re = dot_pos;
                int rs = re;
                while (rs > 0 && (std::isalnum(static_cast<unsigned char>(src_line[rs - 1])) || src_line[rs - 1] == '_')) {
                    rs--;
                }
                if (rs < re) {
                    receiver = src_line.substr(rs, re - rs);
                }
            }
        }
        if (!receiver.empty()) {
            if (docs.count(uri)) {
                std::string doc_fs_path = uri;
                if (doc_fs_path.rfind("file://", 0) == 0) {
                    doc_fs_path = doc_fs_path.substr(7);
                }
                const int cursor_line_1 = line + 1;
                if (const ObjectLiteralInfo *info =
                        find_object_literal_info(docs[uri].object_literals, docs[uri].symbols, receiver, doc_fs_path, cursor_line_1)) {
                    for (const auto &mem : info->members) {
                        if (!prefix.empty() && (mem.name.size() < prefix.size() || mem.name.substr(0, prefix.size()) != prefix)) {
                            continue;
                        }
                        json item;
                        item["label"] = mem.name;
                        item["detail"] = mem.detail;
                        item["kind"] = mem.kind;
                        items.push_back(std::move(item));
                    }
                    json result;
                    result["isIncomplete"] = false;
                    result["items"] = std::move(items);
                    send_response(id, std::move(result));
                    return;
                }

                std::string receiver_class =
                    resolve_receiver_class_name(docs[uri].symbols, docs[uri].classes, receiver, doc_fs_path, cursor_line_1);
                if (!receiver_class.empty()) {
                    bool static_only = (receiver == receiver_class);
                    for (const auto &ci : docs[uri].classes) {
                        if (ci.name != receiver_class) {
                            continue;
                        }
                        for (const auto &member : ci.members) {
                            if (static_only && !member.is_static) {
                                continue;
                            }
                            if (!static_only && member.is_static) {
                                continue;
                            }
                            if (!prefix.empty() && (member.name.size() < prefix.size() || member.name.substr(0, prefix.size()) != prefix)) {
                                continue;
                            }
                            json item;
                            item["label"] = member.name;
                            item["detail"] = member.detail;
                            item["kind"] = member.is_method ? static_cast<int>(CK_Method) : static_cast<int>(CK_Field);
                            items.push_back(std::move(item));
                        }
                        json result;
                        result["isIncomplete"] = false;
                        result["items"] = std::move(items);
                        send_response(id, std::move(result));
                        return;
                    }
                }
            }

            if (!builtins_ready_.load(std::memory_order_acquire)) {
                json result;
                result["isIncomplete"] = false;
                result["items"] = std::move(items);
                send_response(id, std::move(result));
                return;
            }

            std::lock_guard<std::mutex> lock(builtins_mutex_);
            auto it = stdlib_member_syms_.find(receiver);
            if (it != stdlib_member_syms_.end()) {
                for (const auto &mem : it->second) {
                    if (!prefix.empty() && (mem.name.size() < prefix.size() || mem.name.substr(0, prefix.size()) != prefix)) {
                        continue;
                    }
                    json item;
                    item["label"] = mem.name;
                    item["detail"] = mem.detail;
                    item["kind"] = mem.kind;
                    items.push_back(std::move(item));
                }
            } else {
                // Try to resolve the receiver to a typed local variable so we
                // can offer type-specific methods instead of every dot-method.
                std::string resolved_type;
                if (docs.count(uri)) {
                    for (const auto &sym : docs[uri].symbols) {
                        if (sym.name == receiver && !sym.inferred_type.empty()) {
                            resolved_type = sym.inferred_type;
                            break;
                        }
                    }
                }

                // Regex literals typed inline (e.g. /foo/.| -> receiver based on
                // the literal text) won't match a symbol; detect the common case
                // where the source token before the dot is 'TK_REGEX'-shaped.
                // (Leave resolved_type empty; we handle it below via a special path.)

                // If we have a known type, serve only that type's methods from
                // the type-specific map.  Fall back to the full method_syms_ list
                // for unknown types so we never show nothing.
                bool served_from_type = false;
                if (!resolved_type.empty()) {
                    auto tit = method_type_syms_.find(normalize_method_receiver_type(resolved_type));
                    if (tit != method_type_syms_.end()) {
                        served_from_type = true;
                        for (const auto &[mname, msym] : tit->second) {
                            if (!prefix.empty() && (mname.size() < prefix.size() || mname.substr(0, prefix.size()) != prefix)) {
                                continue;
                            }
                            json item;
                            item["label"] = mname;
                            item["detail"] = msym.detail;
                            item["kind"] = static_cast<int>(CK_Method);
                            items.push_back(std::move(item));
                        }
                    }
                }
                if (!served_from_type) {
                    for (const auto &[mname, msym] : method_syms_) {
                        if (!prefix.empty() && (mname.size() < prefix.size() || mname.substr(0, prefix.size()) != prefix)) {
                            continue;
                        }
                        json item;
                        item["label"] = mname;
                        item["detail"] = msym.detail;
                        item["kind"] = static_cast<int>(CK_Method);
                        items.push_back(std::move(item));
                    }
                }
            }
            json result;
            result["isIncomplete"] = false;
            result["items"] = std::move(items);
            send_response(id, std::move(result));
            return;
        }

        auto prefix_matches = [&](const std::string &name) {
            if (prefix.empty()) {
                return true;
            }
            return name.size() >= prefix.size() && name.substr(0, prefix.size()) == prefix;
        };

        // Keywords
        for (int i = 0; kNariKeywords[i]; ++i) {
            std::string kw = kNariKeywords[i];
            if (!prefix_matches(kw)) {
                continue;
            }
            json item;
            item["label"] = kw;
            item["kind"] = static_cast<int>(CK_Keyword);
            items.push_back(std::move(item));
        }

        // built-ins: prefer parsed declarations; fall back to static table
        if (builtins_ready_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(builtins_mutex_);
            for (const auto &[name, sym] : builtin_syms_) {
                if (!prefix_matches(name)) {
                    continue;
                }
                json item;
                item["label"] = name;
                item["detail"] = sym.detail;
                item["kind"] = sym.kind;
                items.push_back(std::move(item));
            }
        } else {
            for (int i = 0; kNariBuiltins[i].name; ++i) {
                std::string name = kNariBuiltins[i].name;
                if (!prefix_matches(name)) {
                    continue;
                }
                json item;
                item["label"] = name;
                item["detail"] = kNariBuiltins[i].detail;
                item["kind"] = static_cast<int>(CK_Function);
                items.push_back(std::move(item));
            }
        }

        // Document symbols
        if (docs.count(uri)) {
            for (const auto &sym : docs[uri].symbols) {
                if (!prefix_matches(sym.name)) {
                    continue;
                }
                json item;
                item["label"] = sym.name;
                item["detail"] = sym.detail;
                item["kind"] = sym.kind;
                items.push_back(std::move(item));
            }
            for (const auto &ci : docs[uri].classes) {
                if (!prefix_matches(ci.name)) {
                    continue;
                }
                json item;
                item["label"] = ci.name;
                item["kind"] = int64_t(CK_Class);
                items.push_back(std::move(item));
            }
        }

        json result;
        result["isIncomplete"] = false;
        result["items"] = std::move(items);
        send_response(id, std::move(result));
    }

    void NariLspServer::handle_hover(const json &id, const json &params) {
        const auto &_td3 = params.contains("textDocument") ? params["textDocument"] : json{};
        const auto &_pos3 = params.contains("position") ? params["position"] : json{};
        std::string uri = _td3.value("uri", std::string{});
        int line = _pos3.value("line", 0);
        int col = _pos3.value("character", 0);
        // Hold a shared lock while reading docs (parse thread may be committing).
        std::shared_lock<std::shared_timed_mutex> docs_lock(docs_mutex);

        if (!docs.count(uri)) {
            send_response(id, nullptr);
            return;
        }

        // Check if cursor is on a string literal in an import statement
        {
            int str_start = 0, str_end = 0;
            std::string spec = import_string_at(docs[uri].content, line, col, str_start, str_end);
            if (!spec.empty()) {
                // Derive filesystem basefile from URI
                std::string basefile = uri;
                if (basefile.rfind("file://", 0) == 0) {
                    basefile = basefile.substr(7);
                }
#ifndef DISABLE_PARSER
                std::string resolved = Parser::resolve_import_path(spec, basefile);
#else
                std::string resolved;
#endif
                std::string hover_text = resolved.empty() ? "Cannot resolve \"" + spec + "\"" : resolved;
                if (resolved.rfind("<std>/", 0) == 0) {
                    hover_text = "Bundled standard library module: std/" + resolved.substr(6);
                }

                json sp, ep;
                sp["line"] = int64_t(line);
                sp["character"] = int64_t(str_start);
                ep["line"] = int64_t(line);
                ep["character"] = int64_t(str_end + 1);
                json rng;
                rng["start"] = std::move(sp);
                rng["end"] = std::move(ep);
                json contents;
                contents["kind"] = "markdown";
                contents["value"] = "`" + hover_text + "`";
                json result;
                result["contents"] = std::move(contents);
                result["range"] = std::move(rng);
                send_response(id, std::move(result));
                return;
            }
        }

        auto wp = word_at(docs[uri].content, line, col);
        const std::string &word = wp.first;
        int word_start = wp.second;
        if (word.empty()) {
            send_response(id, nullptr);
            return;
        }

        std::string detail;
        std::string comment;

        // search in document symbols, keep a pointer so we can pull doc_comment
        const SymInfo *found_sym = nullptr;

        // Dot-access receiver: check for "receiver.word" pattern (e.g. math.add)
        {
            std::string receiver = receiver_at(docs[uri].content, line, word_start);
            if (!receiver.empty()) {
                std::string doc_fs_path_h = uri;
                if (doc_fs_path_h.rfind("file://", 0) == 0) {
                    doc_fs_path_h = doc_fs_path_h.substr(7);
                }
                int cursor_line_1h = line + 1;
                std::string module_source;
                for (const auto &sym : docs[uri].symbols) {
                    if (sym.name == receiver && sym.kind == CK_Module) {
                        module_source = sym.source_file;
                        break;
                    }
                }
                if (!module_source.empty()) {
                    for (const auto &sym : docs[uri].symbols) {
                        if (sym.name == word && sym.source_file == module_source && sym.kind != CK_Variable) {
                            found_sym = &sym;
                            break;
                        }
                    }
                }
                if (!found_sym) {
                    if (const ObjectLiteralInfo *object = find_object_literal_info(
                            docs[uri].object_literals, docs[uri].symbols, receiver, doc_fs_path_h, cursor_line_1h)) {
                        for (const auto &member : object->members) {
                            if (member.name == word) {
                                found_sym = &member;
                                break;
                            }
                        }
                    }
                }
                if (!found_sym) {
                    std::string receiver_class =
                        resolve_receiver_class_name(docs[uri].symbols, docs[uri].classes, receiver, doc_fs_path_h, cursor_line_1h);
                    if (!receiver_class.empty()) {
                        bool static_only = (receiver == receiver_class);
                        if (const MemberInfo *member = find_class_member(docs[uri].classes, receiver_class, word, static_only)) {
                            detail = member->detail;
                            comment = member->doc_comment;
                        }
                    }
                }
                // unknown receiver (local var, array, string), check stdlib object members then dot-methods
                if (!found_sym && detail.empty() && builtins_ready_.load(std::memory_order_acquire)) {
                    std::lock_guard<std::mutex> lock(builtins_mutex_);
                    auto sit = stdlib_member_syms_.find(receiver);
                    if (sit != stdlib_member_syms_.end()) {
                        for (const auto &mem : sit->second) {
                            if (mem.name == word) {
                                detail = mem.detail;
                                comment = mem.doc_comment;
                                break;
                            }
                        }
                    }
                    if (detail.empty()) {
                        auto mit = method_syms_.find(word);
                        if (mit != method_syms_.end()) {
                            detail = mit->second.detail;
                            comment = mit->second.doc_comment;
                        }
                    }
                }
            }
        }

        // direct search in document symbols, scope-aware for locals/params
        if (!found_sym && detail.empty()) {
            std::string doc_fs_path_h = uri;
            if (doc_fs_path_h.rfind("file://", 0) == 0) {
                doc_fs_path_h = doc_fs_path_h.substr(7);
            }
            int cursor_line_1h = line + 1;
            found_sym = find_symbol_in_scope(docs[uri].symbols, word, doc_fs_path_h, cursor_line_1h);
        }
        if (found_sym) {
            detail = found_sym->detail;
            comment = found_sym->doc_comment;
            // If no pre-stored comment, scan backward in the document text
            std::string current_file = uri;
            if (current_file.rfind("file://", 0) == 0) {
                current_file = current_file.substr(7);
            }
            if (comment.empty() && found_sym->line > 0 && found_sym->source_file == current_file) {
                comment = extract_doc_comment(docs[uri].content, found_sym->line);
            }
        }

        // Search in class names / members
        if (detail.empty()) {
            for (const auto &ci : docs[uri].classes) {
                if (ci.name == word) {
                    detail = "class " + ci.name;
                    break;
                }
                for (const auto &m : ci.members) {
                    if (m.name == word) {
                        detail = m.detail;
                        break;
                    }
                }
                if (!detail.empty()) {
                    break;
                }
            }
        }

        // Search built-ins (parsed declarations take precedence over the static table)
        if (detail.empty()) {
            if (builtins_ready_.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lock(builtins_mutex_);
                auto it = builtin_syms_.find(word);
                if (it != builtin_syms_.end()) {
                    detail = it->second.detail;
                    comment = it->second.doc_comment;
                }
            }
            if (detail.empty()) {
                for (int i = 0; kNariBuiltins[i].name; ++i) {
                    if (word == kNariBuiltins[i].name) {
                        detail = kNariBuiltins[i].detail;
                        break;
                    }
                }
            }
        }

        if (detail.empty()) {
            send_response(id, nullptr);
            return;
        }

        // Build the range for the hovered word
        json start_pos, end_pos;
        start_pos["line"] = int64_t(line);
        start_pos["character"] = int64_t(word_start);
        end_pos["line"] = int64_t(line);
        end_pos["character"] = int64_t(word_start + static_cast<int>(word.size()));
        json range;
        range["start"] = std::move(start_pos);
        range["end"] = std::move(end_pos);

        // Markdown: doc comment (if any) above the code block
        std::string hover_md;
        if (!comment.empty()) {
            hover_md = comment + "\n\n";
        }
        hover_md += "```nari\n" + detail + "\n```";

        json contents;
        contents["kind"] = "markdown";
        contents["value"] = std::move(hover_md);

        json result;
        result["contents"] = std::move(contents);
        result["range"] = std::move(range);
        send_response(id, std::move(result));
    }

    void NariLspServer::handle_signature_help(const json &id, const json &params) {
        const auto &td = params.contains("textDocument") ? params["textDocument"] : json{};
        const auto &pos = params.contains("position") ? params["position"] : json{};
        std::string uri = td.value("uri", std::string{});
        int line = pos.value("line", 0);
        int col = pos.value("character", 0);
        std::shared_lock<std::shared_timed_mutex> docs_lock(docs_mutex);
        auto doc_it = docs.find(uri);
        if (doc_it == docs.end()) {
            send_response(id, nullptr);
            return;
        }

        const DocState &doc = doc_it->second;
        std::vector<std::string> lines = lsp_split_lines(doc.content);
        if (line < 0 || line >= static_cast<int>(lines.size())) {
            send_response(id, nullptr);
            return;
        }
        std::string before = lines[line].substr(0, std::min<int>(col, lines[line].size()));
        int depth = 0;
        int open = -1;
        for (int i = static_cast<int>(before.size() - 1); i >= 0; --i) {
            if (before[i] == ')') {
                ++depth;
            } else if (before[i] == '(') {
                if (depth == 0) {
                    open = i;
                    break;
                }
                --depth;
            }
        }
        if (open < 0) {
            send_response(id, nullptr);
            return;
        }

        int end = open;
        int start = end;
        while (start > 0 && (std::isalnum(static_cast<unsigned char>(before[start - 1])) || before[start - 1] == '_' || before[start - 1] == '.')) {
            --start;
        }
        std::string callee = before.substr(start, end - start);
        std::string receiver;
        std::string name = callee;
        size_t dot = callee.rfind('.');
        if (dot != std::string::npos) {
            receiver = callee.substr(0, dot);
            name = callee.substr(dot + 1);
        }

        std::string filename = uri.rfind("file://", 0) == 0 ? uri.substr(7) : uri;
        std::string detail;
        std::string documentation;
        if (receiver.empty()) {
            if (const SymInfo *sym = find_symbol_in_scope(doc.symbols, name, filename, line + 1)) {
                detail = sym->detail;
                documentation = sym->doc_comment;
            }
        } else {
            std::string receiver_type = resolve_receiver_class_name(doc.symbols, doc.classes, receiver, filename, line + 1);
            if (const MemberInfo *member = find_class_member(doc.classes, receiver_type, name)) {
                detail = member->detail;
                documentation = member->doc_comment;
            }
        }
        if (detail.empty()) {
            send_response(id, nullptr);
            return;
        }

        int active_parameter = 0;
        depth = 0;
        for (int i = open + 1; i < static_cast<int>(before.size()); ++i) {
            if (before[i] == '(' || before[i] == '[' || before[i] == '{') {
                ++depth;
            } else if (before[i] == ')' || before[i] == ']' || before[i] == '}') {
                --depth;
            } else if (before[i] == ',' && depth == 0) {
                ++active_parameter;
            }
        }

        json signature;
        signature["label"] = detail;
        if (!documentation.empty()) {
            signature["documentation"] = documentation;
        }
        json result;
        result["signatures"] = json::array({ std::move(signature) });
        result["activeSignature"] = 0;
        result["activeParameter"] = active_parameter;
        send_response(id, std::move(result));
    }

    void NariLspServer::handle_definition(const json &id, const json &params) {
        const auto &_td3 = params.contains("textDocument") ? params["textDocument"] : json{};
        const auto &_pos3 = params.contains("position") ? params["position"] : json{};
        std::string uri = _td3.value("uri", std::string{});
        int line = _pos3.value("line", 0);
        int col = _pos3.value("character", 0);
        // Hold a shared lock while reading docs_ (parse thread may be committing).
        std::shared_lock<std::shared_timed_mutex> docs_lock(docs_mutex);

        if (!docs.count(uri)) {
            send_response(id, nullptr);
            return;
        }

        // Ctrl+click on a string literal in an import -> open the file directly
        {
            int str_start = 0, str_end = 0;
            std::string spec = import_string_at(docs[uri].content, line, col, str_start, str_end);
            if (!spec.empty()) {
                std::string basefile = uri;
                if (basefile.rfind("file://", 0) == 0) {
                    basefile = basefile.substr(7);
                }
#ifndef DISABLE_PARSER
                std::string resolved = Parser::resolve_import_path(spec, basefile);
#else
                std::string resolved;
#endif
                if (resolved.empty()) {
                    send_response(id, nullptr);
                    return;
                }

                json sp, ep;
                sp["line"] = int64_t(0);
                sp["character"] = int64_t(0);
                ep["line"] = int64_t(0);
                ep["character"] = int64_t(0);
                json rng;
                rng["start"] = std::move(sp);
                rng["end"] = std::move(ep);
                json location;
                const std::string target_uri = path_to_uri(std::filesystem::path(resolved));
                if (target_uri.empty()) {
                    send_response(id, nullptr);
                    return;
                }
                location["uri"] = target_uri;
                location["range"] = std::move(rng);
                send_response(id, std::move(location));
                return;
            }
        }

        ResolvedSymbol resolved;
        if (!resolve_symbol_at(uri, line, col, resolved)) {
            send_response(id, nullptr);
            return;
        }

        // Convert 1-based parser positions to 0-based LSP positions
        int lsp_line = std::max(0, resolved.sym.line - 1);
        int lsp_col = std::max(0, resolved.sym.col - 1);

        std::string word = word_at(docs[uri].content, line, col).first;
        if (word.empty()) {
            word = resolved.sym.name;
        }

        json start_pos, end_pos;
        start_pos["line"] = int64_t(lsp_line);
        start_pos["character"] = int64_t(lsp_col);
        end_pos["line"] = int64_t(lsp_line);
        end_pos["character"] = int64_t(lsp_col + static_cast<int>(word.size()));
        json def_range;
        def_range["start"] = std::move(start_pos);
        def_range["end"] = std::move(end_pos);

        json location;
        location["uri"] = resolved.def_uri;
        location["range"] = std::move(def_range);
        send_response(id, std::move(location));
    }

    void NariLspServer::handle_references(const json &id, const json &params) {
        const auto &td = params.contains("textDocument") ? params["textDocument"] : json{};
        const auto &pos = params.contains("position") ? params["position"] : json{};
        const auto &ctx = params.contains("context") ? params["context"] : json{};
        std::string uri = td.value("uri", std::string{});
        int line = pos.value("line", 0);
        int col = pos.value("character", 0);
        const bool include_decl = ctx.value("includeDeclaration", true);

        std::shared_lock<std::shared_timed_mutex> docs_lock(docs_mutex);
        if (!docs.count(uri)) {
            send_response(id, json::array());
            return;
        }

        ResolvedSymbol target;
        if (!resolve_symbol_at(uri, line, col, target)) {
            send_response(id, json::array());
            return;
        }

        json locations = json::array();
        std::set<std::tuple<std::string, int, int>> seen;

        for (const auto &entry : docs) {
            const std::string &doc_uri = entry.first;
            const auto &doc = entry.second;
            for_each_identifier_occurrence(doc.content, [&](int occ_line, int occ_col, std::string_view ident) {
                if (ident != target.sym.name) {
                    return;
                }

                ResolvedSymbol candidate;
                if (!resolve_symbol_at(doc_uri, occ_line, occ_col, candidate)) {
                    return;
                }
                if (!same_symbol_identity(candidate.sym, target.sym)) {
                    return;
                }

                if (!include_decl && candidate.def_uri == doc_uri && occ_line == std::max(0, candidate.sym.line - 1) &&
                    occ_col == std::max(0, candidate.sym.col - 1)) {
                    return;
                }

                if (!seen.insert({ doc_uri, occ_line, occ_col }).second) {
                    return;
                }

                json sp, ep, rng, loc;
                sp["line"] = int64_t(occ_line);
                sp["character"] = int64_t(occ_col);
                ep["line"] = int64_t(occ_line);
                ep["character"] = int64_t(occ_col + static_cast<int>(ident.size()));
                rng["start"] = std::move(sp);
                rng["end"] = std::move(ep);
                loc["uri"] = doc_uri;
                loc["range"] = std::move(rng);
                locations.push_back(std::move(loc));
            });
        }

        send_response(id, std::move(locations));
    }

    void NariLspServer::handle_code_action(const json &id, const json &params) {
        const auto &td = params.contains("textDocument") ? params["textDocument"] : json{};
        const auto &range = params.contains("range") ? params["range"] : json{};
        const auto &context = params.contains("context") ? params["context"] : json{};
        std::string uri = td.value("uri", std::string{});

        std::shared_lock<std::shared_timed_mutex> docs_lock(docs_mutex);
        if (!docs.count(uri)) {
            send_response(id, json::array());
            return;
        }
        const std::string content = docs[uri].content; // copy for use below the lock (lock released at scope end)

        json actions = json::array();

        // quick-fix: remove unused variable
        if (context.contains("diagnostics") && context["diagnostics"].is_array()) {
            std::vector<std::string> lines = lsp_split_lines(content);
            for (const auto &diag : context["diagnostics"]) {
                const std::string msg = diag.value("message", std::string{});
                const std::string pfx = "' is declared but never used";
                const size_t quote1 = msg.find('\'');
                const size_t quote2 = msg.find('\'', quote1 + 1);
                if (quote1 == std::string::npos || quote2 == std::string::npos) {
                    continue;
                }
                if (msg.find(pfx) == std::string::npos) {
                    continue;
                }
                const std::string var_name = msg.substr(quote1 + 1, quote2 - quote1 - 1);
                if (var_name.empty()) {
                    continue;
                }

                const auto &d_rng = diag.contains("range") ? diag["range"] : json{};
                const auto &d_start = d_rng.contains("start") ? d_rng["start"] : json{};
                const int diag_line = d_start.value("line", -1);
                const int diag_col = d_start.value("character", 0);
                if (diag_line < 0 || diag_line >= static_cast<int>(lines.size())) {
                    continue;
                }

                const std::string &src_line = lines[diag_line];
                std::string trimmed = src_line;
                const size_t trim_off = trimmed.find_first_not_of(" \t");
                if (trim_off != std::string::npos) {
                    trimmed = trimmed.substr(trim_off);
                } else {
                    trimmed.clear();
                }

                // Is this a standalone let/global declaration (safe to delete the whole line)?
                const bool is_let_line = trimmed.rfind("let ", 0) == 0 || trimmed.rfind("global ", 0) == 0;
                const bool has_no_comma = trimmed.find(',') == std::string::npos;
                const bool is_standalone = is_let_line && has_no_comma;

                if (is_standalone) {
                    // Delete the whole line (range from {line,0} to {line+1,0})
                    const int next_line = (diag_line + 1 < static_cast<int>(lines.size()) ? diag_line + 1 : diag_line);
                    const int end_char = (next_line == diag_line) ? static_cast<int>(src_line.size()) : 0;
                    json del_s, del_e, del_rng, del_edit, changes;
                    del_s["line"] = int64_t(diag_line);
                    del_s["character"] = int64_t(0);
                    del_e["line"] = int64_t(next_line);
                    del_e["character"] = int64_t(end_char);
                    del_rng["start"] = std::move(del_s);
                    del_rng["end"] = std::move(del_e);
                    del_edit["range"] = std::move(del_rng);
                    del_edit["newText"] = "";
                    changes[uri] = json::array({ std::move(del_edit) });
                    json action;
                    action["title"] = "Remove unused variable '" + var_name + "'";
                    action["kind"] = "quickfix";
                    action["diagnostics"] = json::array({ diag });
                    action["edit"]["changes"] = std::move(changes);
                    action["isPreferred"] = true;
                    actions.push_back(std::move(action));
                }

                // always offer: rename to _name (suppress warning without removing code)
                {
                    json es, ee, rename_rng, rename_edit, changes;
                    es["line"] = int64_t(diag_line);
                    es["character"] = int64_t(diag_col);
                    ee["line"] = int64_t(diag_line);
                    ee["character"] = int64_t(diag_col + static_cast<int>(var_name.size()));
                    rename_rng["start"] = std::move(es);
                    rename_rng["end"] = std::move(ee);
                    rename_edit["range"] = std::move(rename_rng);
                    rename_edit["newText"] = "_" + var_name;
                    changes[uri] = json::array({ std::move(rename_edit) });
                    json action;
                    action["title"] = "Suppress: rename '" + var_name + "' to '_" + var_name + "'";
                    action["kind"] = "quickfix";
                    action["diagnostics"] = json::array({ diag });
                    action["edit"]["changes"] = std::move(changes);
                    actions.push_back(std::move(action));
                }
            }
        }

        send_response(id, std::move(actions));
    }

    void NariLspServer::handle_inlay_hints(const json &id, const json &params) {
        const auto &td = params.contains("textDocument") ? params["textDocument"] : json{};
        const auto &rng = params.contains("range") ? params["range"] : json{};
        std::string uri = td.value("uri", std::string{});

        std::shared_lock<std::shared_timed_mutex> docs_lock(docs_mutex);
        if (!docs.count(uri)) {
            send_response(id, json::array());
            return;
        }
        const DocState &doc = docs[uri];

        const int range_start = rng.contains("start") ? rng["start"].value("line", 0) : 0;
        const int range_end = rng.contains("end") ? rng["end"].value("line", 0x7fffffff) : 0x7fffffff;

        const std::vector<std::string> lines = lsp_split_lines(doc.content);
        json hints = json::array();

        for (const auto &sym : doc.symbols) {
            // Only annotate variable declarations that have a known inferred type
            if (sym.kind != CK_Variable) {
                continue;
            }
            if (sym.inferred_type.empty()) {
                continue;
            }
            // skip parameters, they already display types in hover/signature
            if (!sym.detail.empty() && sym.detail.rfind("(parameter)", 0) == 0) {
                continue;
            }

            const int lsp_line = sym.line - 1;
            if (lsp_line < range_start || lsp_line > range_end) {
                continue;
            }
            if (lsp_line < 0 || lsp_line >= static_cast<int>(lines.size())) {
                continue;
            }

            // Find the variable identifier position on this line (sym.col points to `let`)
            const size_t search_from = (sym.col > 0) ? static_cast<size_t>(sym.col - 1) : 0u;
            const int found_col1 = lsp_find_word_col(lines[lsp_line], sym.name, search_from);
            if (found_col1 < 0) {
                continue;
            }

            // Hint position: right after the variable name
            const int hint_col = (found_col1 - 1) + static_cast<int>(sym.name.size()); // 0-based
            json hint;
            hint["position"]["line"] = int64_t(lsp_line);
            hint["position"]["character"] = int64_t(hint_col);
            hint["label"] = ": " + sym.inferred_type;
            hint["kind"] = int64_t(1); // InlayHintKind.Type = 1
            hint["paddingLeft"] = true;
            hints.push_back(std::move(hint));
        }

        send_response(id, std::move(hints));
    }

    void NariLspServer::handle_semantic_tokens_full(const json &id, const json &params) {
        const auto &td = params.contains("textDocument") ? params["textDocument"] : json{};
        std::string uri = td.value("uri", std::string{});

        std::shared_lock<std::shared_timed_mutex> docs_lock(docs_mutex);
        if (!docs.count(uri)) {
            json r;
            r["data"] = json::array();
            send_response(id, std::move(r));
            return;
        }
        const DocState &doc = docs[uri];
        const std::vector<std::string> lines = lsp_split_lines(doc.content);

        // build name -> entry map
        struct NameEntry {
            int token_type;
            int priority;
            bool emit_usages;
            std::set<std::pair<int, int>> decl_pos;
        };
        std::unordered_map<std::string, NameEntry> name_map;
        name_map.reserve(doc.symbols.size() * 2);

        auto upsert = [&](const std::string &name, int tok_type, int prio, bool emit_usages, int sym_line, int sym_col) {
            auto ins = name_map.emplace(name, NameEntry{ tok_type, prio, emit_usages, {} });
            auto it = ins.first;
            bool inserted = ins.second;
            if (!inserted && prio > it->second.priority) {
                it->second.token_type = tok_type;
                it->second.priority = prio;
                it->second.emit_usages = emit_usages;
            }
            // Resolve the actual identifier column (sym_col points to `let`/`func` keyword)
            const int lsp_ln = sym_line - 1;
            if (lsp_ln >= 0 && lsp_ln < static_cast<int>(lines.size())) {
                const size_t from = (sym_col > 0) ? static_cast<size_t>(sym_col - 1) : 0u;
                const int fc = lsp_find_word_col(lines[lsp_ln], name, from);
                if (fc > 0) {
                    it->second.decl_pos.insert({ lsp_ln, fc - 1 });
                }
            }
        };

        for (const auto &sym : doc.symbols) {
            switch (sym.kind) {
                case CK_Function:
                    upsert(sym.name, ST_FUNCTION, 6, true, sym.line, sym.col);
                    break;
                case CK_Class:
                    upsert(sym.name, ST_CLASS, 5, true, sym.line, sym.col);
                    break;
                case CK_Enum:
                    upsert(sym.name, ST_ENUM, 4, true, sym.line, sym.col);
                    break;
                case CK_Struct:
                    upsert(sym.name, ST_TYPE, 3, true, sym.line, sym.col);
                    break;
                case CK_Module:
                    upsert(sym.name, ST_NAMESPACE, 2, true, sym.line, sym.col);
                    break;
                case CK_Variable:
                    if (!sym.detail.empty() && sym.detail.rfind("(parameter)", 0) == 0) {
                        upsert(sym.name, ST_PARAM, 1, false, sym.line, sym.col);
                    } else {
                        upsert(sym.name, ST_VARIABLE, 0, false, sym.line, sym.col);
                    }
                    break;
                default:
                    break;
            }
        }

        // Also include known builtin function names (not already in name_map)
        if (builtins_ready_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(builtins_mutex_);
            for (const auto &[bname, bsym] : builtin_syms_) {
                if (bsym.kind == CK_Function) {
                    name_map.emplace(bname, NameEntry{ ST_FUNCTION, 6, true, {} });
                }
            }
        }

        // scan source lines and collect tokens
        struct Token {
            int line, col, len, type, mods;
        };
        std::vector<Token> tokens;
        tokens.reserve(512);

        const auto is_id_c = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };

        for (int ln = 0; ln < static_cast<int>(lines.size()); ++ln) {
            const std::string &line_str = lines[ln];

            // Quick check: skip pure comment lines
            {
                size_t p = 0;
                while (p < line_str.size() && (line_str[p] == ' ' || line_str[p] == '\t')) {
                    ++p;
                }
                if (p + 1 < line_str.size() && line_str[p] == '/' && line_str[p + 1] == '/') {
                    continue;
                }
            }

            bool in_str = false; // basic single-line string-skip
            for (size_t i = 0; i < line_str.size();) {
                if (!in_str && line_str[i] == '"') {
                    in_str = true;
                    ++i;
                    continue;
                }
                if (in_str) {
                    if (line_str[i] == '\\') {
                        i += 2;
                        continue;
                    }
                    if (line_str[i] == '"') {
                        in_str = false;
                    }
                    ++i;
                    continue;
                }
                // Skip line comment mid-line
                if (i + 1 < line_str.size() && line_str[i] == '/' && line_str[i + 1] == '/') {
                    break;
                }

                if (!std::isalpha(static_cast<unsigned char>(line_str[i])) && line_str[i] != '_') {
                    ++i;
                    continue;
                }

                const size_t id_start = i;
                while (i < line_str.size() && is_id_c(line_str[i])) {
                    ++i;
                }
                const size_t id_end = i;

                const std::string ident(line_str.data() + id_start, id_end - id_start);
                auto it = name_map.find(ident);
                if (it == name_map.end()) {
                    continue;
                }

                const NameEntry &entry = it->second;
                const std::pair<int, int> pos = { ln, static_cast<int>(id_start) };
                const bool is_decl = entry.decl_pos.count(pos) > 0;

                if (!entry.emit_usages && !is_decl) {
                    continue; // only decl positions for variables/params
                }

                tokens.push_back({ ln, static_cast<int>(id_start), static_cast<int>(id_end - id_start), entry.token_type, is_decl ? SM_DECLARATION : 0 });
            }
        }

        // Sort and deduplicate (keep first occurrence at each position)
        std::stable_sort(tokens.begin(), tokens.end(),
                         [](const Token &a, const Token &b) { return a.line != b.line ? a.line < b.line : a.col < b.col; });
        tokens.erase(
            std::unique(tokens.begin(), tokens.end(), [](const Token &a, const Token &b) { return a.line == b.line && a.col == b.col; }),
            tokens.end());

        // Encode as LSP delta-encoded semantic tokens array
        json data = json::array();
        auto &arr = data.array_items();
        arr.reserve(tokens.size() * 5);
        int prev_ln = 0, prev_col = 0;
        for (const auto &tok : tokens) {
            const int dl = tok.line - prev_ln;
            const int dc = (dl == 0) ? (tok.col - prev_col) : tok.col;
            arr.push_back(int64_t(dl));
            arr.push_back(int64_t(dc));
            arr.push_back(int64_t(tok.len));
            arr.push_back(int64_t(tok.type));
            arr.push_back(int64_t(tok.mods));
            prev_ln = tok.line;
            prev_col = tok.col;
        }

        json result;
        result["data"] = std::move(data);
        send_response(id, std::move(result));
    }

    void NariLspServer::handle_type_definition(const json &id, const json &params) {
        const auto &td = params.contains("textDocument") ? params["textDocument"] : json{};
        const auto &pos = params.contains("position") ? params["position"] : json{};
        std::string uri = td.value("uri", std::string{});
        const int line = pos.value("line", 0);
        const int col = pos.value("character", 0);

        std::shared_lock<std::shared_timed_mutex> docs_lock(docs_mutex);
        if (!docs.count(uri)) {
            send_response(id, nullptr);
            return;
        }
        const DocState &doc = docs[uri];

        auto wp = word_at(doc.content, line, col);
        const std::string &word = wp.first;
        if (word.empty()) {
            send_response(id, nullptr);
            return;
        }

        // primitive types are not user-defined, so no type declaration to jump to
        static const std::unordered_set<std::string> primitives = { "string", "number", "bool",     "null",
                                                                    "array",  "object", "function", "regex" };

        // if the word is a variable, look at its inferred_type for a class name
        std::string class_name;
        for (const auto &sym : doc.symbols) {
            if (sym.name != word || sym.kind != CK_Variable) {
                continue;
            }
            if (!sym.inferred_type.empty() && !primitives.count(sym.inferred_type)) {
                class_name = sym.inferred_type;
                break;
            }
        }

        // if the word is itself a class or type name, jump to its declaration
        if (class_name.empty()) {
            for (const auto &ci : doc.classes) {
                if (ci.name == word) {
                    class_name = word;
                    break;
                }
            }
        }
        if (class_name.empty()) {
            for (const auto &sym : doc.symbols) {
                if (sym.name == word && (sym.kind == CK_Class || sym.kind == CK_Struct || sym.kind == CK_Enum)) {
                    class_name = word;
                    break;
                }
            }
        }

        if (class_name.empty()) {
            send_response(id, nullptr);
            return;
        }

        // locate the class definition

        // first try doc.classes (parsed class declarations)
        for (const auto &ci : doc.classes) {
            if (ci.name != class_name) {
                continue;
            }
            // find source file via symbols
            std::string src_file;
            for (const auto &sym : doc.symbols) {
                if (sym.name == class_name && sym.kind == CK_Class) {
                    src_file = sym.source_file;
                    break;
                }
            }

            const int lsp_ln = std::max(0, ci.line - 1);
            const int lsp_ch = std::max(0, ci.col - 1);
            json sp, ep, rng, loc;
            sp["line"] = int64_t(lsp_ln);
            sp["character"] = int64_t(lsp_ch);
            ep["line"] = int64_t(lsp_ln);
            ep["character"] = int64_t(lsp_ch + static_cast<int>(class_name.size()));
            rng["start"] = std::move(sp);
            rng["end"] = std::move(ep);
            loc["range"] = std::move(rng);
            loc["uri"] = src_file.empty() ? uri : path_to_uri(std::filesystem::path(src_file));
            send_response(id, std::move(loc));
            return;
        }

        // fall back to the symbol table (enum / type aliases defined in stdlib or builtins)
        for (const auto &sym : doc.symbols) {
            if (sym.name != class_name) {
                continue;
            }
            if (sym.kind != CK_Enum && sym.kind != CK_Struct && sym.kind != CK_Class) {
                continue;
            }
            const int lsp_ln = std::max(0, sym.line - 1);
            const int lsp_ch = std::max(0, sym.col - 1);
            json sp, ep, rng, loc;
            sp["line"] = int64_t(lsp_ln);
            sp["character"] = int64_t(lsp_ch);
            ep["line"] = int64_t(lsp_ln);
            ep["character"] = int64_t(lsp_ch + static_cast<int>(class_name.size()));
            rng["start"] = std::move(sp);
            rng["end"] = std::move(ep);
            loc["range"] = std::move(rng);
            loc["uri"] = sym.source_file.empty() ? uri : path_to_uri(std::filesystem::path(sym.source_file));
            send_response(id, std::move(loc));
            return;
        }

        // check builtins
        if (builtins_ready_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(builtins_mutex_);
            auto bit = builtin_syms_.find(class_name);
            if (bit != builtin_syms_.end() && !builtin_decls_uri_.empty()) {
                const int lsp_ln = std::max(0, bit->second.line - 1);
                const int lsp_ch = std::max(0, bit->second.col - 1);
                json sp, ep, rng, loc;
                sp["line"] = int64_t(lsp_ln);
                sp["character"] = int64_t(lsp_ch);
                ep["line"] = int64_t(lsp_ln);
                ep["character"] = int64_t(lsp_ch + static_cast<int>(class_name.size()));
                rng["start"] = std::move(sp);
                rng["end"] = std::move(ep);
                loc["range"] = std::move(rng);
                loc["uri"] = builtin_decls_uri_;
                send_response(id, std::move(loc));
                return;
            }
        }

        send_response(id, nullptr);
    }
