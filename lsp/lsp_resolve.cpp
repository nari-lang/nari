/*
  lsp_resolve.cpp
  NariLspServer symbol resolution split from lsp/lsp_server.cpp.
*/

#include "lsp_server.h"

    bool NariLspServer::same_symbol_identity(const SymInfo &a, const SymInfo &b) {
        if (a.kind != b.kind || a.name != b.name) {
            return false;
        }

        if (a.kind == CK_Field || a.kind == CK_Method) {
            return !a.owner_class.empty() && a.owner_class == b.owner_class;
        }

        if (a.kind == CK_Variable) {
            const bool a_is_param = !a.detail.empty() && a.detail.rfind("(parameter)", 0) == 0;
            const bool b_is_param = !b.detail.empty() && b.detail.rfind("(parameter)", 0) == 0;
            if (a_is_param != b_is_param) {
                return false;
            }

            if (a.scope_fn_line != 0 || b.scope_fn_line != 0) {
                return a.source_file == b.source_file && a.scope_fn_line == b.scope_fn_line;
            }

            return a.source_file == b.source_file && a.line == b.line;
        }

        return a.source_file == b.source_file && a.line == b.line;
    }

    bool NariLspServer::resolve_symbol_at(const std::string &uri, int line, int col, ResolvedSymbol &out) {
        if (!docs.count(uri)) {
            return false;
        }
        const DocState &doc = docs[uri];

        int str_start = 0, str_end = 0;
        std::string spec = import_string_at(doc.content, line, col, str_start, str_end);
        if (!spec.empty()) {
            return false;
        }

        auto wp = word_at(doc.content, line, col);
        const std::string &word = wp.first;
        int word_start = wp.second;
        if (word.empty()) {
            return false;
        }

        std::string receiver = receiver_at(doc.content, line, word_start);

        const SymInfo *found = nullptr;
        SymInfo resolved_sym;
        std::string doc_fs_path = uri;
        if (doc_fs_path.rfind("file://", 0) == 0) {
            doc_fs_path = doc_fs_path.substr(7);
        }
        int cursor_line_1 = line + 1;

        if (word == "this") {
            std::string this_class = resolve_receiver_class_name(doc.symbols, doc.classes, "this", doc_fs_path, cursor_line_1);
            if (!this_class.empty()) {
                for (const auto &ci : doc.classes) {
                    if (ci.name != this_class) {
                        continue;
                    }
                    resolved_sym = { ci.name, "class " + ci.name, CK_Class, ci.line, ci.col, ci.source_file };
                    found = &resolved_sym;
                    break;
                }
            }
        }

        if (!receiver.empty()) {
            std::string module_source;
            for (const auto &sym : doc.symbols) {
                if (sym.name == receiver && sym.kind == CK_Module) {
                    module_source = sym.source_file;
                    break;
                }
            }
            if (!module_source.empty()) {
                for (const auto &sym : doc.symbols) {
                    if (sym.name == word && sym.source_file == module_source && sym.kind != CK_Variable) {
                        found = &sym;
                        break;
                    }
                }
            }
            if (!found) {
                if (const ObjectLiteralInfo *object =
                        find_object_literal_info(doc.object_literals, doc.symbols, receiver, doc_fs_path, cursor_line_1)) {
                    for (const auto &member : object->members) {
                        if (member.name == word) {
                            found = &member;
                            break;
                        }
                    }
                }
            }
            if (!found) {
                std::string receiver_class = resolve_receiver_class_name(doc.symbols, doc.classes, receiver, doc_fs_path, cursor_line_1);
                if (!receiver_class.empty()) {
                    bool static_only = (receiver == receiver_class);
                    if (const MemberInfo *member = find_class_member(doc.classes, receiver_class, word, static_only)) {
                        resolved_sym = { member->name, member->detail, member->is_method ? CK_Method : CK_Field,
                                         member->line, member->col,    member->source_file };
                        resolved_sym.owner_class = receiver_class;
                        found = &resolved_sym;
                    }
                }
            }
        }

        if (!found) {
            found = find_symbol_in_scope(doc.symbols, word, doc_fs_path, cursor_line_1);
        }

        SymInfo stdlib_mem_sym;
        if (!found && !receiver.empty() && builtins_ready_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(builtins_mutex_);
            auto it = stdlib_member_syms_.find(receiver);
            if (it != stdlib_member_syms_.end()) {
                for (const auto &mem : it->second) {
                    if (mem.name == word) {
                        stdlib_mem_sym = mem;
                        found = &stdlib_mem_sym;
                        break;
                    }
                }
            }
            if (!found) {
                auto mit = method_syms_.find(word);
                if (mit != method_syms_.end()) {
                    stdlib_mem_sym = mit->second;
                    stdlib_mem_sym.source_file = "";
                    found = &stdlib_mem_sym;
                }
            }
        }

        SymInfo class_sym;
        if (!found) {
            for (const auto &ci : doc.classes) {
                if (ci.name == word) {
                    class_sym = { ci.name, "class " + ci.name, CK_Class, ci.line, ci.col, ci.source_file };
                    found = &class_sym;
                    break;
                }
            }
        }

        SymInfo builtin_sym;
        if (!found && builtins_ready_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(builtins_mutex_);
            if (!builtin_decls_uri_.empty()) {
                auto it = builtin_syms_.find(word);
                if (it != builtin_syms_.end()) {
                    builtin_sym = it->second;
                    found = &builtin_sym;
                }
            }
        }

        if (!found) {
            return false;
        }

        out.sym = *found;
        if (&builtin_sym == found) {
            out.def_uri = builtin_decls_uri_;
        } else if (found == &stdlib_mem_sym && found->source_file.empty()) {
            out.def_uri = builtin_decls_uri_;
        } else if (!found->source_file.empty()) {
            out.def_uri = path_to_uri(std::filesystem::path(found->source_file));
            if (out.def_uri.empty()) {
                return false;
            }
        } else {
            out.def_uri = uri;
        }
        return true;
    }
