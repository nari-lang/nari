/*
  parser.cpp
  Expression-precedence parser (precedence-climbing) that builds the AST for
  expressions. read:
  https://en.wikipedia.org/wiki/Operator-precedence_parser#Precedence_climbing_method
*/

#include "ast.h"

#include <cmath>
#include <cstdio>
#include <ctype.h>
#include <errno.h>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include "win_funcs.h"
#endif

using nari::BlockPtr;
using nari::ExprPtr;
using nari::FunctionPtr;
using nari::StmtPtr;
using nari::VarDeclCtrl;

namespace Parser {

// Token kinds for parser input.
enum class TokenKind {
  // end of input
  TK_EOF,
  // identifiers and literals
  TK_IDENT,
  // numbers
  TK_NUMBER,
  // strings
  TK_STRING,
  // `interpolated strings like this {some_var}!`
  TK_INTERP_STRING,
  // (
  TK_LPAREN,
  // )
  TK_RPAREN,
  // {
  TK_LBRACE,
  // }
  TK_RBRACE,
  // [
  TK_LBRACKET,
  // ]
  TK_RBRACKET,
  // ,
  TK_COMMA,
  // =
  TK_EQUAL,
  // ;
  TK_SEMICOLON,
  // :
  TK_COLON,
  // .
  TK_DOT,
  // ...
  TK_ELLIPSIS,
  // ?
  TK_QUESTION,
  // @
  TK_AT,
  // +
  TK_PLUS,
  // -
  TK_MINUS,
  // *
  TK_STAR,
  // /
  TK_SLASH,
  // !
  TK_BANG,
  // <
  TK_LT,
  // >
  TK_GT,
  // <=
  TK_LE,
  // >=
  TK_GE,
  // ==
  TK_EQEQ,
  // !=
  TK_NEQ,
  // &&
  TK_ANDAND,
  // ||
  TK_OROR,
  // %
  TK_PERCENT,
  // ++
  TK_PLUSPLUS,
  // --
  TK_MINUSMINUS,
  // +=
  TK_PLUSEQ,
  // -=
  TK_MINUSEQ,
  // *=
  TK_STAREQ,
  // /=
  TK_SLASHEQ,
  // %=
  TK_PERCENTEQ,
  // **
  TK_EXPONENT,
  // ??
  TK_NULLISHCOALESCE,
  // ->
  TK_ARROW,
  // =>
  TK_FATARROW,
  // &
  TK_AMPERSAND,
  // |
  TK_PIPE,
  // ^
  TK_CARET,
  // ~
  TK_TILDE,
  // <<
  TK_LSHIFT,
  // >>
  TK_RSHIFT,
  // &=
  TK_AMPEQ,
  // |=
  TK_PIPEEQ,
  // ^=
  TK_CARETEQ,
  // <<=
  TK_LSHIFTEQ,
  // >>=
  TK_RSHIFTEQ,
  // unknown token
  TK_UNKNOWN
};

struct Token {
  TokenKind kind = TokenKind::TK_UNKNOWN;
  std::string text; // identifier name, string contents (unquoted), number text,
                    // or symbol text
  int line = 0;
  int col = 0;
  std::string filename; // for error reporting

  // For TK_INTERP_STRING: stores alternating string parts and expression source
  std::vector<std::string> interp_parts; // String literals
  std::vector<std::string> interp_exprs; // Expression source code to parse

  std::string stringify() const {
    if (text.empty())
      return "<empty>";
    return text;
  }
};

// human-readable token description
static std::string token_desc(const Token &tok) {
  if (tok.text.empty())
    return "<empty>";
  return tok.text;
}

// global filename and error context, for error reporting mostly.
static std::string g_current_filename;
static std::vector<std::string> g_error_contexts;

static std::vector<nari::TypeDeclPtr> g_type_declarations;
static std::map<std::string, const nari::TypeDecl *> g_type_registry;

static std::vector<nari::EnumDeclPtr> g_enum_declarations;
static std::map<std::string, const nari::EnumDecl *> g_enum_registry;

static std::vector<nari::ClassDeclPtr> g_class_declarations;
static std::map<std::string, const nari::ClassDecl *> g_class_registry;

void register_type(nari::TypeDeclPtr type_decl) {
  const std::string &name = type_decl->name;
  const nari::TypeDecl *ptr = type_decl.get();
  g_type_declarations.push_back(std::move(type_decl));
  g_type_registry[name] = ptr;
}

void register_enum(nari::EnumDeclPtr enum_decl) {
  const std::string &name = enum_decl->name;
  const nari::EnumDecl *ptr = enum_decl.get();
  g_enum_declarations.push_back(std::move(enum_decl));
  g_enum_registry[name] = ptr;
}

void register_class(nari::ClassDeclPtr class_decl) {
  const std::string &name = class_decl->name;
  const nari::ClassDecl *ptr = class_decl.get();
  g_class_declarations.push_back(std::move(class_decl));
  g_class_registry[name] = ptr;
}

bool is_registered_type(const std::string &name) {
  return g_type_registry.find(name) != g_type_registry.end();
}

bool is_registered_enum(const std::string &name) {
  return g_enum_registry.find(name) != g_enum_registry.end();
}

bool is_registered_class(const std::string &name) {
  return g_class_registry.find(name) != g_class_registry.end();
}

const nari::TypeDecl *get_registered_type(const std::string &name) {
  auto it = g_type_registry.find(name);
  if (it != g_type_registry.end()) {
    return it->second;
  }
  return nullptr;
}

const nari::ClassDecl *get_registered_class(const std::string &name) {
  auto it = g_class_registry.find(name);
  if (it != g_class_registry.end()) {
    return it->second;
  }
  return nullptr;
}

void clear_type_registry() {
  g_type_registry.clear();
  g_type_declarations.clear();
  g_enum_registry.clear();
  g_enum_declarations.clear();
  g_class_registry.clear();
  g_class_declarations.clear();
}

void set_source_filename(const std::string &fn) { g_current_filename = fn; }

static std::unordered_set<std::string> g_visited_files;

static std::string resolve_include_path(const std::string &inc,
                                        const std::string &basefile) {
  namespace fs = std::filesystem;
  if (inc.empty())
    return inc;

  fs::path incp = inc;
  if (incp.is_absolute())
    return incp.lexically_normal().string();

  // get base path to resolve relative includes against
  fs::path basepath;
  if (basefile.empty()) {
    basepath = fs::current_path();
  } else {
    fs::path bf(basefile);
    if (bf.has_parent_path())
      basepath = bf.parent_path();
    else
      basepath = fs::current_path();
  }

  fs::path combined = basepath / incp;
  return combined.lexically_normal().string();
}

/*
    NOTE: these functions do not imply that an error *has* occured
    it just adds the context to the error stack for if one does happen.
*/
void push_error_context(const std::string &ctx) {
  g_error_contexts.push_back(ctx);
}
void pop_error_context() {
  if (!g_error_contexts.empty())
    g_error_contexts.pop_back();
}

// prints filename:line:col header + message, and then exits.
static void fatal_error(const std::string &msg, const Token *tok = nullptr) {
  std::string fname =
      g_current_filename.empty() ? std::string("<input>") : g_current_filename;
  int l = 0, c = 0;
  if (tok) {
    if (!tok->filename.empty())
      fname = tok->filename;
    l = tok->line;
    c = tok->col;
  }

  fprintf(stderr, "Exception at %s:%d:%d\n", fname.c_str(), l, c);
  fprintf(stderr, "    %s\n", msg.c_str());

  auto print_source_context = [&](const std::string &file, int line, int col) {
    if (file.empty() || file[0] == '<' || line <= 0)
      return;
    FILE *fp = fopen(file.c_str(), "r");
    if (!fp)
      return;

    char *line_buffer = nullptr;
    size_t buffer_size = 0;
    std::string line_text;

    for (int i = 1; i <= line; ++i) {
      ssize_t line_length = getline(&line_buffer, &buffer_size, fp);
      if (line_length == -1) {
        free(line_buffer);
        fclose(fp);
        return;
      }
      if (i == line) {
        line_text = line_buffer;
        if (!line_text.empty() && line_text.back() == '\n') {
          line_text.pop_back();
        }
      }
    }
    free(line_buffer);
    fclose(fp);

    fprintf(stderr, "    %s\n", line_text.c_str());
    int caret_pos = col > 0 ? col - 1 : 0;
    std::string caret_line(caret_pos, ' ');
    caret_line.push_back('^');
    fprintf(stderr, "    %s\n", caret_line.c_str());
  };

  print_source_context(fname, l, c);

  // prints out a trace of included files up to this point
  if (!g_error_contexts.empty()) {
    for (int i = static_cast<int>(g_error_contexts.size()) - 1; i >= 0; --i) {
      fprintf(stderr, "    included from %s\n", g_error_contexts[i].c_str());
    }
  }

  exit(1);
}

std::vector<FunctionPtr>
parse_program_from_source(const std::string &src,
                          bool create_aggregator = true);

class Parser {
public:
  Parser(const std::vector<Token> &tokens) : toks(tokens), idx(0) {}

  std::vector<FunctionPtr> parse_program(bool create_aggregator = true) {
    std::vector<FunctionPtr> functions;

    // collect top-level statements into synthetic __top_level__ function
    auto top_block = std::make_unique<nari::BlockStmt>();

    while (!is_eof()) {
      const Token &tok = peek();

      // type declaration: type Name<T> { field: type; ... }
      if (tok.kind == TokenKind::TK_IDENT && tok.text == "type" &&
          looks_like_type_decl()) {
        auto type_decl = parse_type_decl();
        register_type(std::move(type_decl));
        continue;
      }

      // enum declaration: enum Name<T> { Variant1, Variant2(T), ... }
      if (tok.kind == TokenKind::TK_IDENT && tok.text == "enum" &&
          looks_like_enum_decl()) {
        auto enum_decl = parse_enum_decl();
        register_enum(std::move(enum_decl));
        continue;
      }

      // class declaration: class Name<T> { fields... methods... }
      if (tok.kind == TokenKind::TK_IDENT && tok.text == "class" &&
          looks_like_class_decl()) {
        auto class_decl = parse_class_decl();
        register_class(std::move(class_decl));
        continue;
      }

      // function declaration: func foo(args...) { ... }
      if (tok.kind == TokenKind::TK_IDENT && tok.text == "func" &&
          looks_like_func_keyword_decl()) {
        auto fn = parse_function();
        functions.push_back(std::move(fn));
        continue;
      }

      // import "file.nari" OR import name from "library.so"
      if (tok.kind == TokenKind::TK_IDENT && tok.text == "import") {
        next();

        // check if this is "import name from"  syntax
        if (peek().kind == TokenKind::TK_IDENT) {
          std::string var_name = next().text;

          if (peek().kind != TokenKind::TK_IDENT || peek().text != "from") {
            error_and_exit("Expected 'from' after import variable name");
          }
          next(); // consume "from"

          if (peek().kind != TokenKind::TK_STRING) {
            error_and_exit("Expected library path string after 'from'");
          }
          std::string lib_path = next().text;

          // transform into `let var_name = __ffi_load_library("path");`
          auto load_call = std::make_unique<nari::CallExpr>(
              std::make_unique<nari::IdentExpr>("__ffi_load_library"));
          load_call->args.push_back(
              std::make_unique<nari::StringExpr>(lib_path));

          auto var_decl = std::make_unique<nari::VarDeclStmt>(
              var_name, std::move(load_call), VarDeclCtrl::LOCAL);

          top_block->stmts.push_back(std::move(var_decl));

          if (peek().kind == TokenKind::TK_SEMICOLON)
            next();
          continue;
        }

        const Token &tok = peek();
        if (tok.kind != TokenKind::TK_STRING) {
          error_and_exit("import requires a string filename or 'name from "
                         "\"path\"' syntax");
        }
        std::string inc_name = next().text;
        std::string base_for_resolve =
            !tok.filename.empty() ? tok.filename : g_current_filename;
        std::string inc_path = resolve_include_path(inc_name, base_for_resolve);

        // avoid import cycle
        if (g_visited_files.find(inc_path) == g_visited_files.end()) {
          g_visited_files.insert(inc_path);

          std::string context =
              (g_current_filename.empty() ? std::string("<input>")
                                          : g_current_filename) +
              ":" + std::to_string(tok.line) + ":" + std::to_string(tok.col);
          push_error_context(context);

          FILE *fp = fopen(inc_path.c_str(), "r");
          if (!fp) {
            std::string err = "Failed to open import file " + inc_path;
            error_and_exit(err);
          }

          fseek(fp, 0, SEEK_END);
          long file_size = ftell(fp);
          fseek(fp, 0, SEEK_SET);

          std::string included_src;
          if (file_size > 0) {
            included_src.resize(file_size);
            size_t bytes_read = fread(&included_src[0], 1, file_size, fp);
            included_src.resize(bytes_read);
          }
          fclose(fp);

          // remember previous filename and tell parser about the included
          // filename
          std::string prev_file = g_current_filename;
          set_source_filename(inc_path);

          // parse the included module and merge its functions into current
          // vector
          auto more_funcs = parse_program_from_source(included_src, false);
          for (auto &f : more_funcs)
            functions.push_back(std::move(f));

          // restore filename, pop ctx
          set_source_filename(prev_file);
          pop_error_context();
        }

        // semi-colons are optional in this language.
        if (peek().kind == TokenKind::TK_SEMICOLON)
          next();
        continue;
      }

      if (tok.kind == TokenKind::TK_EOF)
        break;

      // for anything else, parse statement and store it in the synthetic
      // top-level block
      auto stmt = parse_stmt();
      if (stmt) {
        top_block->stmts.push_back(std::move(stmt));
        continue;
      }
      // if we have an unrecoverable statement / eof, break out.
      break;
    }

    // if we collected any top-level statements, synthesize a __top_level__
    // function
    if (!top_block->stmts.empty()) {
      prune_dead_code(top_block.get());
      optimize_block(top_block.get());
      std::string modname = g_current_filename.empty() ? std::string("<input>")
                                                       : g_current_filename;
      std::string per_module_name = std::string("__top_level__@") + modname;

      auto top_fn = std::make_unique<nari::Function>();
      top_fn->name = per_module_name;

      // attach location info if possible
      if (!top_block->stmts.empty()) {
        const auto &first = top_block->stmts.front();
        if (first) {
          top_fn->line = first->line;
          top_fn->col = first->col;
          top_fn->filename =
              first->filename.empty() ? modname : first->filename;
        }
      }
      top_fn->body = std::move(top_block);
      functions.push_back(std::move(top_fn));

      // build an aggregator __top_level__ function only if requested
      if (create_aggregator) {
        // build an aggregator __top_level__ function that will call all
        // per-module synthetic top functions discovered in this module's
        // function list.
        auto agg_fn = std::make_unique<nari::Function>();
        agg_fn->name = std::string("__top_level__");
        auto aggregate_block = std::make_unique<nari::BlockStmt>();

        for (const auto &func_ptr : functions) {
          if (!func_ptr)
            continue;
          const std::string &name = func_ptr->name;
          const std::string prefix = "__top_level__@";
          if (name.size() >= prefix.size() &&
              name.compare(0, prefix.size(), prefix) == 0) {
            auto ident = std::make_unique<nari::IdentExpr>(name);
            ident->line = func_ptr->line;
            ident->col = func_ptr->col;
            ident->filename = func_ptr->filename;

            auto call_expr = std::make_unique<nari::CallExpr>(std::move(ident));
            // attach position info
            call_expr->line = func_ptr->line;
            call_expr->col = func_ptr->col;
            call_expr->filename = func_ptr->filename;
            // snapshot metadata before moving the call
            // so that we don't dereference a moved-from unique_ptr.
            int c_line = call_expr->line;
            int c_col = call_expr->col;
            std::string c_filename = call_expr->filename;
            auto call_stmt =
                std::make_unique<nari::ExprStmt>(std::move(call_expr));
            call_stmt->line = c_line;
            call_stmt->col = c_col;
            call_stmt->filename = c_filename;
            aggregate_block->stmts.push_back(std::move(call_stmt));
          }
        }

        // attach aggregator metadata and insert it at the front so callers that
        // inspect function lists see the aggregator early.
        agg_fn->body = std::move(aggregate_block);
        agg_fn->line = 0;
        agg_fn->col = 0;
        agg_fn->filename = modname;
        functions.insert(functions.begin(), std::move(agg_fn));
      }
    }

    for (auto &fn : functions) {
      if (fn && fn->body) {
        prune_dead_code(fn->body.get());
        optimize_block(fn->body.get());
      }
    }
    return functions;
  }

private:
  const std::vector<Token> &toks;
  size_t idx;

  bool is_eof() const {
    return idx >= toks.size() || toks[idx].kind == TokenKind::TK_EOF;
  }

  const Token &peek(size_t n = 0) const {
    size_t i = idx + n;
    if (i >= toks.size())
      return toks.back();
    return toks[i];
  }

  const Token &next() {
    if (idx < toks.size())
      return toks[idx++];
    return toks.back();
  }

  bool match(TokenKind k) {
    if (!is_eof() && peek().kind == k) {
      next();
      return true;
    }
    return false;
  }

  // TODO: This needs finer grained control over exit condition
  // i.e whether we should return control back to the runtime, which can then do
  // whatever, or actually exit.
  void error_and_exit(const std::string &msg) {
    const Token *tok = nullptr;
    if (!is_eof())
      tok = &peek();

    std::string use_fname = g_current_filename.empty() ? std::string("<input>")
                                                       : g_current_filename;
    int line = 0;
    int col = 0;

    if (tok) {
      // If the lexer attached a filename to the token, prefer it.
      if (!tok->filename.empty())
        use_fname = tok->filename;
      line = tok->line;
      col = tok->col;
    }

    fprintf(stderr, "Exception at %s:%d:%d\n", use_fname.c_str(), line, col);
    fprintf(stderr, "    %s %s:%d:%d\n", msg.c_str(), use_fname.c_str(), line,
            col);

    auto print_source_context = [&](const std::string &file, int lno, int cno) {
      if (file.empty() || file[0] == '<' || lno <= 0)
        return;
      FILE *fp = fopen(file.c_str(), "r");
      if (!fp)
        return;

      char *line_buffer = nullptr;
      size_t buffer_size = 0;
      std::string line_text;

      for (int i = 1; i <= lno; ++i) {
        ssize_t line_length = getline(&line_buffer, &buffer_size, fp);
        if (line_length == -1) {
          free(line_buffer);
          fclose(fp);
          return;
        }
        if (i == lno) {
          line_text = line_buffer;
          // remove trailing newline if present
          if (!line_text.empty() && line_text.back() == '\n') {
            line_text.pop_back();
          }
        }
      }
      free(line_buffer);
      fclose(fp);

      fprintf(stderr, "    %s\n", line_text.c_str());
      int caret_pos = cno > 0 ? cno - 1 : 0;
      std::string caret_line(caret_pos, ' ');
      caret_line.push_back('^');
      fprintf(stderr, "    %s\n", caret_line.c_str());
    };

    print_source_context(use_fname, line, col);

    // print chain if we have more than one error context
    if (!g_error_contexts.empty()) {
      for (int i = g_error_contexts.size() - 1; i >= 0; --i) {
        fprintf(stderr, "    included from %s:%d:%d\n",
                g_error_contexts[i].c_str(), line, col);
      }
    }

    std::exit(1);
  }

  void expect(TokenKind k, const char *msg = nullptr) {
    if (!is_eof() && peek().kind == k) {
      next();
      return;
    }
    std::string error_msg = "Parse error";
    if (!is_eof())
      error_msg += " token='" + token_desc(peek()) + "'";
    if (msg)
      error_msg += " (" + std::string(msg) + ")";
    error_and_exit(error_msg);
  }

  bool looks_like_func_keyword_decl() {
    // func IDENT '(' ... ')' [-> type] '{'
    if (peek().kind != TokenKind::TK_IDENT || peek().text != "func")
      return false;
    if (peek(1).kind != TokenKind::TK_IDENT)
      return false;
    if (peek(2).kind != TokenKind::TK_LPAREN)
      return false;
    // find matching RPAREN
    size_t i = idx + 3;
    int depth = 1;
    while (i < toks.size()) {
      if (toks[i].kind == TokenKind::TK_LPAREN)
        ++depth;
      else if (toks[i].kind == TokenKind::TK_RPAREN) {
        --depth;
        if (depth == 0)
          break;
      } else if (toks[i].kind == TokenKind::TK_EOF)
        break;
      ++i;
    }
    // after ')' we might have '-> type' or '-> type[]' before '{'
    size_t next_idx = i + 1;
    if (next_idx < toks.size() && toks[next_idx].kind == TokenKind::TK_ARROW) {
      // skip '-> type'
      next_idx++; // skip '->'
      if (next_idx < toks.size() &&
          toks[next_idx].kind == TokenKind::TK_IDENT) {
        next_idx++; // skip type name
        // check for array syntax: type[]
        if (next_idx < toks.size() &&
            toks[next_idx].kind == TokenKind::TK_LBRACKET &&
            next_idx + 1 < toks.size() &&
            toks[next_idx + 1].kind == TokenKind::TK_RBRACKET) {
          next_idx += 2; // skip '[]'
        }
      }
    }
    if (next_idx < toks.size() && toks[next_idx].kind == TokenKind::TK_LBRACE)
      return true;
    return false;
  }

  bool looks_like_foreach_loop() {
    // for ( IDENT in ... )
    if (peek().kind != TokenKind::TK_IDENT || peek().text != "for")
      return false;
    if (peek(1).kind != TokenKind::TK_LPAREN)
      return false;
    if (peek(2).kind != TokenKind::TK_IDENT)
      return false;
    if (peek(3).kind != TokenKind::TK_IDENT || peek(3).text != "in")
      return false;
    return true;
  }

  bool looks_like_type_decl() {
    // type IDENT { or type IDENT< (generics) or type IDENT IDENT (alias)
    if (peek().kind != TokenKind::TK_IDENT || peek().text != "type")
      return false;
    if (peek(1).kind != TokenKind::TK_IDENT)
      return false;
    // Could be type Name { or type Name<T> { or type Alias BaseType
    if (peek(2).kind == TokenKind::TK_LBRACE ||
        peek(2).kind == TokenKind::TK_LT || peek(2).kind == TokenKind::TK_IDENT)
      return true;
    return false;
  }

  bool looks_like_enum_decl() {
    // enum IDENT { or enum IDENT< (generics)
    if (peek().kind != TokenKind::TK_IDENT || peek().text != "enum")
      return false;
    if (peek(1).kind != TokenKind::TK_IDENT)
      return false;
    if (peek(2).kind == TokenKind::TK_LBRACE ||
        peek(2).kind == TokenKind::TK_LT)
      return true;
    return false;
  }

  bool looks_like_class_decl() {
    // class IDENT { or class IDENT< (generics) or class IDENT extends
    if (peek().kind != TokenKind::TK_IDENT || peek().text != "class")
      return false;
    if (peek(1).kind != TokenKind::TK_IDENT)
      return false;
    if (peek(2).kind == TokenKind::TK_LBRACE ||
        peek(2).kind == TokenKind::TK_LT ||
        (peek(2).kind == TokenKind::TK_IDENT && peek(2).text == "extends"))
      return true;
    return false;
  }

  template <typename T> void parse_generic_arguments(T *type) {
    if (peek().kind == TokenKind::TK_LT) {
      next(); // consume '<'
      while (!is_eof() && peek().kind != TokenKind::TK_GT) {
        if (peek().kind != TokenKind::TK_IDENT) {
          error_and_exit("Expected generic parameter name");
        }
        type->generic_params.push_back(next().text);

        if (peek().kind == TokenKind::TK_COMMA) {
          next(); // consume ','
        } else if (peek().kind != TokenKind::TK_GT) {
          error_and_exit("Expected ',' or '>' in generic parameters");
        }
      }
      expect(TokenKind::TK_GT, "'>' after generic parameters");
    }
  }

  nari::TypeDeclPtr parse_type_decl() {
    next(); // consume 'type'
    const Token &nameTok = next();
    if (nameTok.kind != TokenKind::TK_IDENT) {
      error_and_exit("Expected type name after 'type'");
    }
    auto type_decl = std::make_unique<nari::TypeDecl>(nameTok.text);
    type_decl->line = nameTok.line;
    type_decl->col = nameTok.col;
    type_decl->filename =
        nameTok.filename.empty() ? g_current_filename : nameTok.filename;

    parse_generic_arguments(type_decl.get());

    // Check if this is a type alias or a struct definition
    if (peek().kind == TokenKind::TK_LBRACE) {
      // Struct definition
      expect(TokenKind::TK_LBRACE, "type body start '{'");

      while (!is_eof() && peek().kind != TokenKind::TK_RBRACE) {
        // Parse field: name: type or name: type[]
        if (peek().kind != TokenKind::TK_IDENT) {
          error_and_exit("Expected field name in type declaration");
        }
        std::string field_name = next().text;

        expect(TokenKind::TK_COLON, "':' after field name");

        auto field_type = parse_type_annotation();
        type_decl->fields.emplace_back(field_name, std::move(field_type));

        // Optional semicolon after field
        if (peek().kind == TokenKind::TK_SEMICOLON) {
          next();
        }
      }

      expect(TokenKind::TK_RBRACE, "type body end '}'");
    } else if (peek().kind == TokenKind::TK_IDENT) {
      // Type alias: type NewName ExistingType
      type_decl->alias_target = parse_type_annotation();

      // Optional semicolon after alias
      if (peek().kind == TokenKind::TK_SEMICOLON) {
        next();
      }
    } else {
      error_and_exit("Expected '{' for type definition or type name for alias");
    }

    return type_decl;
  }

  // type annotation with optional generics
  std::unique_ptr<nari::TypeAnnotation> parse_type_annotation() {
    if (peek().kind != TokenKind::TK_IDENT) {
      error_and_exit("Expected type name");
    }
    std::string type_name = next().text;
    auto type_ann = std::make_unique<nari::TypeAnnotation>(type_name);

    parse_generic_arguments(type_ann.get());

    // is it type[]?
    if (peek().kind == TokenKind::TK_LBRACKET &&
        peek(1).kind == TokenKind::TK_RBRACKET) {
      next(); // consume '['
      next(); // consume ']'
      type_ann->is_array = true;
    }

    return type_ann;
  }

  nari::EnumDeclPtr parse_enum_decl() {
    next(); // consume 'enum'
    const Token &nameTok = next();
    if (nameTok.kind != TokenKind::TK_IDENT) {
      error_and_exit("Expected enum name after 'enum'");
    }
    auto enum_decl = std::make_unique<nari::EnumDecl>(nameTok.text);
    enum_decl->line = nameTok.line;
    enum_decl->col = nameTok.col;
    enum_decl->filename =
        nameTok.filename.empty() ? g_current_filename : nameTok.filename;

    // parse generic parameters
    parse_generic_arguments(enum_decl.get());

    expect(TokenKind::TK_LBRACE, "enum body start '{'");

    while (!is_eof() && peek().kind != TokenKind::TK_RBRACE) {
      // Parse variant: Name or Name(T) or Name { field: T }
      if (peek().kind != TokenKind::TK_IDENT) {
        error_and_exit("Expected variant name in enum declaration");
      }
      std::string variant_name = next().text;
      nari::EnumVariant variant(variant_name);

      // Check for tuple variant: Some(T)
      if (peek().kind == TokenKind::TK_LPAREN) {
        next(); // consume '('
        while (!is_eof() && peek().kind != TokenKind::TK_RPAREN) {
          auto field_type = parse_type_annotation();
          variant.tuple_fields.push_back(std::move(field_type));

          if (peek().kind == TokenKind::TK_COMMA) {
            next(); // consume ','
          } else if (peek().kind != TokenKind::TK_RPAREN) {
            error_and_exit("Expected ',' or ')' in variant fields");
          }
        }
        expect(TokenKind::TK_RPAREN, "')' after variant fields");
      }

      else if (peek().kind == TokenKind::TK_LBRACE) {
        next(); // consume '{'
        while (!is_eof() && peek().kind != TokenKind::TK_RBRACE) {
          if (peek().kind != TokenKind::TK_IDENT) {
            error_and_exit("Expected field name in variant");
          }
          std::string field_name = next().text;
          expect(TokenKind::TK_COLON, "':' after field name");

          auto field_type = parse_type_annotation();
          variant.named_fields.emplace_back(field_name, std::move(field_type));

          if (peek().kind == TokenKind::TK_COMMA) {
            next(); // consume ','
          } else if (peek().kind != TokenKind::TK_RBRACE) {
            error_and_exit("Expected ',' or '}' in variant fields");
          }
        }
        expect(TokenKind::TK_RBRACE, "'}' after variant fields");
      }

      enum_decl->variants.push_back(std::move(variant));

      // optional comma
      if (peek().kind == TokenKind::TK_COMMA) {
        next();
      }
    }

    expect(TokenKind::TK_RBRACE, "enum body end '}'");
    return enum_decl;
  }

  nari::ClassDeclPtr parse_class_decl() {
    next(); // consume 'class'
    const Token &nameTok = next();
    if (nameTok.kind != TokenKind::TK_IDENT) {
      error_and_exit("Expected class name after 'class'");
    }
    auto class_decl = std::make_unique<nari::ClassDecl>(nameTok.text);
    class_decl->line = nameTok.line;
    class_decl->col = nameTok.col;
    class_decl->filename =
        nameTok.filename.empty() ? g_current_filename : nameTok.filename;

    // parse generic parameters
    parse_generic_arguments(class_decl.get());

    // Parse optional extends clause
    if (peek().kind == TokenKind::TK_IDENT && peek().text == "extends") {
      next(); // consume 'extends'
      const Token &parentTok = peek();
      if (parentTok.kind != TokenKind::TK_IDENT) {
        error_and_exit("Expected parent class name after 'extends'");
      }
      class_decl->parent_name = next().text;
    }

    expect(TokenKind::TK_LBRACE, "class body start '{'");

    // Parse class body: fields and methods with visibility modifiers
    while (!is_eof() && peek().kind != TokenKind::TK_RBRACE) {
      // Parse visibility: public or private (default to public)
      nari::Visibility visibility = nari::Visibility::Public;

      if (peek().kind == TokenKind::TK_IDENT) {
        if (peek().text == "public") {
          next(); // consume 'public'
          visibility = nari::Visibility::Public;
        } else if (peek().text == "private") {
          next(); // consume 'private'
          visibility = nari::Visibility::Private;
        }
      }

      // Now parse either field or method
      // Check if it's a constructor
      if (peek().kind == TokenKind::TK_IDENT && peek().text == "init" &&
          peek(1).kind == TokenKind::TK_LPAREN) {
        // Constructor
        std::string method_name = next().text;
        nari::ClassMethod method(method_name, visibility);
        method.is_constructor = true;

        // Parse parameters
        expect(TokenKind::TK_LPAREN, "'(' for constructor parameters");
        if (peek().kind != TokenKind::TK_RPAREN) {
          while (true) {
            if (peek().kind != TokenKind::TK_IDENT) {
              error_and_exit("Expected parameter name");
            }
            std::string param_name = next().text;

            std::unique_ptr<nari::TypeAnnotation> param_type = nullptr;
            if (peek().kind == TokenKind::TK_COLON) {
              next(); // consume ':'
              param_type = parse_type_annotation();
            }

            method.params.emplace_back(param_name, nullptr, false,
                                       std::move(param_type));

            if (peek().kind == TokenKind::TK_COMMA) {
              next();
            } else if (peek().kind == TokenKind::TK_RPAREN) {
              break;
            } else {
              error_and_exit("Expected ',' or ')' in constructor parameters");
            }
          }
        }
        expect(TokenKind::TK_RPAREN, "')' after constructor parameters");

        // Parse body
        method.body = parse_block();

        class_decl->methods.push_back(std::move(method));
        continue;
      }

      // Check if it's a method: identifier followed by '('
      if (peek().kind == TokenKind::TK_IDENT &&
          peek(1).kind == TokenKind::TK_LPAREN) {
        // Method
        std::string method_name = next().text;
        nari::ClassMethod method(method_name, visibility);

        // Parse parameters
        expect(TokenKind::TK_LPAREN, "'(' for method parameters");
        if (peek().kind != TokenKind::TK_RPAREN) {
          while (true) {
            if (peek().kind != TokenKind::TK_IDENT) {
              error_and_exit("Expected parameter name");
            }
            std::string param_name = next().text;

            std::unique_ptr<nari::TypeAnnotation> param_type = nullptr;
            if (peek().kind == TokenKind::TK_COLON) {
              next(); // consume ':'
              param_type = parse_type_annotation();
            }

            method.params.emplace_back(param_name, nullptr, false,
                                       std::move(param_type));

            if (peek().kind == TokenKind::TK_COMMA) {
              next();
            } else if (peek().kind == TokenKind::TK_RPAREN) {
              break;
            } else {
              error_and_exit("Expected ',' or ')' in method parameters");
            }
          }
        }
        expect(TokenKind::TK_RPAREN, "')' after method parameters");

        // Parse optional return type
        if (peek().kind == TokenKind::TK_ARROW) {
          next(); // consume '->'
          method.return_type = parse_type_annotation();
        }

        // Parse body
        method.body = parse_block();

        class_decl->methods.push_back(std::move(method));
        continue;
      }

      // Otherwise it's a field: name: type or name: type = default
      if (peek().kind != TokenKind::TK_IDENT) {
        error_and_exit("Expected field or method name in class body");
      }
      std::string field_name = next().text;

      expect(TokenKind::TK_COLON, "':' after field name");

      auto field_type = parse_type_annotation();

      // Parse optional default value
      ExprPtr default_value = nullptr;
      if (peek().kind == TokenKind::TK_EQUAL) {
        next(); // consume '='
        default_value = parse_expression();
      }

      class_decl->fields.emplace_back(field_name, visibility,
                                      std::move(field_type),
                                      std::move(default_value));

      // Optional semicolon after field
      if (peek().kind == TokenKind::TK_SEMICOLON) {
        next();
      }
    }

    expect(TokenKind::TK_RBRACE, "class body end '}'");
    return class_decl;
  }

  FunctionPtr parse_function() {
    // consume 'func'
    next();
    const Token &nameTok = next();
    if (nameTok.kind != TokenKind::TK_IDENT) {
      error_and_exit("Expected function name after 'func'");
    }
    auto fn = std::make_unique<nari::Function>();
    fn->name = nameTok.text;
    expect(TokenKind::TK_LPAREN, "function params start '('");
    bool seen_rest = false;
    if (peek().kind != TokenKind::TK_RPAREN) {
      while (true) {
        bool is_rest = false;
        if (peek().kind == TokenKind::TK_ELLIPSIS) {
          if (seen_rest)
            error_and_exit("Only one rest parameter is allowed");
          seen_rest = true;
          is_rest = true;
          next(); // consume '...'
        }

        const Token &p = peek();
        if (p.kind != TokenKind::TK_IDENT) {
          error_and_exit("Expected parameter name");
        }
        std::string pname = next().text;

        // parse optional type annotation
        // name: type or name: type[]
        nari::TypeAnnotationPtr param_type = nullptr;
        if (peek().kind == TokenKind::TK_COLON) {
          next(); // consume ':'
          if (peek().kind != TokenKind::TK_IDENT) {
            error_and_exit("Expected type name after ':'");
          }
          std::string type_name = next().text;
          bool is_array = false;
          // Check for array syntax: type[]
          if (peek().kind == TokenKind::TK_LBRACKET &&
              peek(1).kind == TokenKind::TK_RBRACKET) {
            next(); // consume '['
            next(); // consume ']'
            is_array = true;
          }
          param_type =
              std::make_unique<nari::TypeAnnotation>(type_name, is_array);
        }

        ExprPtr default_value = nullptr;
        if (!is_rest && peek().kind == TokenKind::TK_EQUAL) {
          next(); // consume '='
          default_value = parse_expression();
        } else if (is_rest && peek().kind == TokenKind::TK_EQUAL) {
          error_and_exit("Rest parameter cannot have a default value");
        }

        fn->params.emplace_back(pname, std::move(default_value), is_rest,
                                std::move(param_type));

        if (is_rest) {
          if (peek().kind == TokenKind::TK_COMMA) {
            error_and_exit("Rest parameter must be last");
          }
          break;
        }

        if (peek().kind == TokenKind::TK_COMMA)
          next();
        else
          break;
      }
    }
    expect(TokenKind::TK_RPAREN, "function params end ')'");

    // parse optional return type: -> type or -> type[]
    if (peek().kind == TokenKind::TK_ARROW) {
      next(); // consume '->'
      if (peek().kind != TokenKind::TK_IDENT) {
        error_and_exit("Expected return type after '->'");
      }
      std::string return_type_name = next().text;
      bool is_array = false;
      // Check for array syntax: type[]
      if (peek().kind == TokenKind::TK_LBRACKET &&
          peek(1).kind == TokenKind::TK_RBRACKET) {
        next(); // consume '['
        next(); // consume ']'
        is_array = true;
      }
      fn->return_type =
          std::make_unique<nari::TypeAnnotation>(return_type_name, is_array);
    }

    fn->body = parse_block();
    return fn;
  }

  BlockPtr parse_block() {
    expect(TokenKind::TK_LBRACE, "block start '{'");
    auto blk = std::make_unique<nari::BlockStmt>();
    while (!is_eof() && peek().kind != TokenKind::TK_RBRACE) {
      auto stmt = parse_stmt();
      if (stmt)
        blk->stmts.push_back(std::move(stmt));
      else
        break;
    }
    expect(TokenKind::TK_RBRACE, "block end '}'");
    prune_dead_code(blk.get());
    optimize_block(blk.get());
    return blk;
  }

  void prune_dead_code(nari::BlockStmt *blk) {
    if (!blk)
      return;
    bool seen_return = false;
    for (size_t i = 0; i < blk->stmts.size(); ++i) {
      if (seen_return) {
        blk->stmts.erase(blk->stmts.begin() + i, blk->stmts.end());
        break;
      }
      if (dynamic_cast<nari::ReturnStmt *>(blk->stmts[i].get())) {
        seen_return = true;
      }
    }
  }

  void optimize_block(nari::BlockStmt *blk) {
    if (!blk)
      return;
    for (size_t i = 0; i < blk->stmts.size(); ++i) {
      auto *fs = dynamic_cast<nari::ForStmt *>(blk->stmts[i].get());
      if (!fs)
        continue;
      nari::Stmt *prev = (i > 0) ? blk->stmts[i - 1].get() : nullptr;
      auto replacement = try_optimize_for(fs, prev);
      if (replacement) {
        blk->stmts[i] = std::move(replacement);
      }
    }
  }

  bool get_loop_init(nari::Stmt *init, std::string &var, int64_t &start) {
    if (auto *vd = dynamic_cast<nari::VarDeclStmt *>(init)) {
      if (!vd->initializerExpr)
        return false;
      int64_t v = 0;
      if (!literal_int(vd->initializerExpr.get(), v))
        return false;
      var = vd->name;
      start = v;
      return true;
    }
    if (auto *as = dynamic_cast<nari::AssignStmt *>(init)) {
      if (!as->value)
        return false;
      int64_t v = 0;
      if (!literal_int(as->value.get(), v))
        return false;
      var = as->target;
      start = v;
      return true;
    }
    return false;
  }

  bool get_loop_cond(nari::Expr *cond, const std::string &var, int64_t &end,
                     bool &inclusive) {
    auto *be = dynamic_cast<nari::BinaryExpr *>(cond);
    if (!be || !be->left || !be->right)
      return false;
    if (be->op != "<" && be->op != "<=")
      return false;
    auto *id = dynamic_cast<nari::IdentExpr *>(be->left.get());
    if (!id || id->name != var)
      return false;
    int64_t v = 0;
    if (!literal_int(be->right.get(), v))
      return false;
    inclusive = (be->op == "<=");
    end = v;
    return true;
  }

  bool get_loop_step(nari::Stmt *post, const std::string &var, int64_t &step) {
    if (!post)
      return false;
    if (auto *as = dynamic_cast<nari::AssignStmt *>(post)) {
      if (as->target != var || !as->value)
        return false;
      auto *be = dynamic_cast<nari::BinaryExpr *>(as->value.get());
      if (!be || !be->left || !be->right)
        return false;
      auto *id = dynamic_cast<nari::IdentExpr *>(be->left.get());
      if (!id || id->name != var)
        return false;
      int64_t v = 0;
      if (!literal_int(be->right.get(), v))
        return false;
      if (be->op == "+") {
        step = v;
        return true;
      }
      if (be->op == "-") {
        step = -v;
        return true;
      }
      return false;
    }
    if (auto *es = dynamic_cast<nari::ExprStmt *>(post)) {
      auto *ue = dynamic_cast<nari::UnaryExpr *>(es->expr.get());
      if (!ue)
        return false;
      auto *id = dynamic_cast<nari::IdentExpr *>(ue->operand.get());
      if (!id || id->name != var)
        return false;
      if (ue->op == "post++") {
        step = 1;
        return true;
      }
      if (ue->op == "post--") {
        step = -1;
        return true;
      }
    }
    return false;
  }

  bool get_body_increment(nari::Stmt *body, std::string &target, int64_t &inc) {
    auto *blk = dynamic_cast<nari::BlockStmt *>(body);
    if (!blk || blk->stmts.size() != 1)
      return false;

    if (auto *as = dynamic_cast<nari::AssignStmt *>(blk->stmts[0].get())) {
      auto *be = dynamic_cast<nari::BinaryExpr *>(as->value.get());
      if (!be || !be->left || !be->right)
        return false;
      auto *id = dynamic_cast<nari::IdentExpr *>(be->left.get());
      if (!id || id->name != as->target)
        return false;
      int64_t v = 0;
      if (!literal_int(be->right.get(), v))
        return false;
      if (be->op == "+") {
        target = as->target;
        inc = v;
        return true;
      }
      if (be->op == "-") {
        target = as->target;
        inc = -v;
        return true;
      }
      return false;
    }

    if (auto *es = dynamic_cast<nari::ExprStmt *>(blk->stmts[0].get())) {
      auto *ue = dynamic_cast<nari::UnaryExpr *>(es->expr.get());
      if (!ue)
        return false;
      auto *id = dynamic_cast<nari::IdentExpr *>(ue->operand.get());
      if (!id)
        return false;
      if (ue->op == "post++") {
        target = id->name;
        inc = 1;
        return true;
      }
      if (ue->op == "post--") {
        target = id->name;
        inc = -1;
        return true;
      }
    }

    return false;
  }

  std::unique_ptr<nari::Stmt> try_optimize_for(nari::ForStmt *fs,
                                               nari::Stmt *prev) {
    if (!fs || !fs->init || !fs->cond || !fs->post || !fs->body)
      return nullptr;

    std::string loop_var;
    int64_t start = 0;
    if (!get_loop_init(fs->init.get(), loop_var, start))
      return nullptr;

    int64_t end = 0;
    bool inclusive = false;
    if (!get_loop_cond(fs->cond.get(), loop_var, end, inclusive))
      return nullptr;

    int64_t step = 0;
    if (!get_loop_step(fs->post.get(), loop_var, step))
      return nullptr;

    if (step <= 0)
      return nullptr;
    if (step != 1)
      return nullptr;

    // check if body is empty - optimize to just setting the loop variable to
    // the end value
    auto *blk = dynamic_cast<nari::BlockStmt *>(fs->body.get());
    if (blk && blk->stmts.empty()) {
      int64_t final_value = inclusive ? end + 1 : end;
      auto val_expr = std::make_unique<nari::NumberExpr>(final_value);
      val_expr->line = fs->line;
      val_expr->col = fs->col;
      val_expr->filename = fs->filename;
      auto assign =
          std::make_unique<nari::AssignStmt>(loop_var, std::move(val_expr));
      assign->line = fs->line;
      assign->col = fs->col;
      assign->filename = fs->filename;
      return assign;
    }

    std::string target;
    int64_t inc = 0;
    if (!get_body_increment(fs->body.get(), target, inc))
      return nullptr;
    if (target == loop_var)
      return nullptr;

    if (!prev)
      return nullptr;
    bool prev_numeric = false;
    if (auto *vd = dynamic_cast<nari::VarDeclStmt *>(prev)) {
      int64_t v = 0;
      if (vd->name == target && vd->initializerExpr &&
          literal_int(vd->initializerExpr.get(), v)) {
        prev_numeric = true;
      }
    } else if (auto *as = dynamic_cast<nari::AssignStmt *>(prev)) {
      int64_t v = 0;
      if (as->target == target && as->value &&
          literal_int(as->value.get(), v)) {
        prev_numeric = true;
      }
    }
    if (!prev_numeric)
      return nullptr;

    __int128 diff = static_cast<__int128>(end) - static_cast<__int128>(start);
    if (inclusive)
      diff += 1;
    if (diff <= 0)
      diff = 0;
    __int128 delta128 = diff * static_cast<__int128>(inc);
    if (delta128 < std::numeric_limits<int64_t>::min() ||
        delta128 > std::numeric_limits<int64_t>::max()) {
      return nullptr;
    }
    int64_t delta = static_cast<int64_t>(delta128);
    auto left = std::make_unique<nari::IdentExpr>(target);
    auto right = std::make_unique<nari::NumberExpr>(delta);
    auto be = std::make_unique<nari::BinaryExpr>("+", std::move(left),
                                                 std::move(right));
    be->line = fs->line;
    be->col = fs->col;
    be->filename = fs->filename;
    auto assign = std::make_unique<nari::AssignStmt>(target, std::move(be));
    assign->line = fs->line;
    assign->col = fs->col;
    assign->filename = fs->filename;
    return assign;
  }

  static bool literal_int(const nari::Expr *e, int64_t &out) {
    if (const auto *ne = dynamic_cast<const nari::NumberExpr *>(e)) {
      if (!ne->is_float) {
        out = ne->i;
        return true;
      }
    }
    return false;
  }

  static bool literal_float(const nari::Expr *e, double &out) {
    if (const auto *ne = dynamic_cast<const nari::NumberExpr *>(e)) {
      if (ne->is_float) {
        out = ne->f;
        return true;
      }
    }
    return false;
  }

  static double number_to_double(const nari::NumberExpr *ne) {
    if (!ne)
      return 0.0;
    if (ne->is_float)
      return ne->f;
    return static_cast<double>(ne->i);
  }

  static bool literal_bool(const nari::Expr *e, bool &out) {
    if (const auto *be = dynamic_cast<const nari::BoolExpr *>(e)) {
      out = be->value;
      return true;
    }
    return false;
  }

  static bool literal_string(const nari::Expr *e, std::string &out) {
    if (const auto *se = dynamic_cast<const nari::StringExpr *>(e)) {
      out = se->value;
      return true;
    }
    return false;
  }

  static bool literal_any(const nari::Expr *e) {
    return dynamic_cast<const nari::NumberExpr *>(e) ||
           dynamic_cast<const nari::StringExpr *>(e) ||
           dynamic_cast<const nari::BoolExpr *>(e);
  }

  static std::string literal_to_string(const nari::Expr *e) {
    if (const auto *se = dynamic_cast<const nari::StringExpr *>(e))
      return se->value;
    if (const auto *be = dynamic_cast<const nari::BoolExpr *>(e))
      return be->value ? "true" : "false";
    if (const auto *ne = dynamic_cast<const nari::NumberExpr *>(e)) {
      if (ne->is_float)
        return std::to_string(ne->f);
      else
        return std::to_string(ne->i);
    }
    return "";
  }

  static void copy_loc(nari::Expr *dst, const nari::Expr *src) {
    if (!dst || !src)
      return;
    dst->line = src->line;
    dst->col = src->col;
    dst->filename = src->filename;
  }

  ExprPtr fold_expr(ExprPtr expr) {
    if (!expr)
      return expr;

    if (auto *ue = dynamic_cast<nari::UnaryExpr *>(expr.get())) {
      ue->operand = fold_expr(std::move(ue->operand));
      if (!ue->operand)
        return expr;
      int64_t ni = 0;
      double nf = 0.0;
      bool b = false;
      if (ue->op == "neg" && literal_int(ue->operand.get(), ni)) {
        auto result = std::make_unique<nari::NumberExpr>(-ni);
        copy_loc(result.get(), ue);
        return result;
      }
      if (ue->op == "+" && literal_int(ue->operand.get(), ni)) {
        auto result = std::make_unique<nari::NumberExpr>(ni);
        copy_loc(result.get(), ue);
        return result;
      }
      if (ue->op == "+" && literal_float(ue->operand.get(), nf)) {
        auto result = std::make_unique<nari::NumberExpr>(nf);
        copy_loc(result.get(), ue);
        return result;
      }
      if (ue->op == "!" && literal_bool(ue->operand.get(), b)) {
        auto result = std::make_unique<nari::BoolExpr>(!b);
        copy_loc(result.get(), ue);
        return result;
      }
      return expr;
    }

    if (auto *binaryExpr = dynamic_cast<nari::BinaryExpr *>(expr.get())) {
      binaryExpr->left = fold_expr(std::move(binaryExpr->left));
      binaryExpr->right = fold_expr(std::move(binaryExpr->right));
      if (!binaryExpr->left || !binaryExpr->right)
        return expr;

      int64_t li = 0, ri = 0;
      double lf = 0.0, rf = 0.0;
      bool lb = false, rb = false;
      std::string ls, rs;

      bool lint = literal_int(binaryExpr->left.get(), li);
      bool rint = literal_int(binaryExpr->right.get(), ri);
      bool lfloat = literal_float(binaryExpr->left.get(), lf);
      bool rfloat = literal_float(binaryExpr->right.get(), rf);
      bool lbool = literal_bool(binaryExpr->left.get(), lb);
      bool rbool = literal_bool(binaryExpr->right.get(), rb);
      bool lstr = literal_string(binaryExpr->left.get(), ls);
      bool rstr = literal_string(binaryExpr->right.get(), rs);

      const std::string &op = binaryExpr->op;
      if (lint && rint) {
        if (op == "+") {
          __int128 v = (__int128)li + (__int128)ri;
          if (v < std::numeric_limits<int64_t>::min() ||
              v > std::numeric_limits<int64_t>::max())
            return expr;
          auto ne = std::make_unique<nari::NumberExpr>(static_cast<int64_t>(v));
          copy_loc(ne.get(), binaryExpr);
          return ne;
        }
        if (op == "-") {
          __int128 v = (__int128)li - (__int128)ri;
          if (v < std::numeric_limits<int64_t>::min() ||
              v > std::numeric_limits<int64_t>::max())
            return expr;
          auto ne = std::make_unique<nari::NumberExpr>(static_cast<int64_t>(v));
          copy_loc(ne.get(), binaryExpr);
          return ne;
        }
        if (op == "*") {
          __int128 v = (__int128)li * (__int128)ri;
          if (v < std::numeric_limits<int64_t>::min() ||
              v > std::numeric_limits<int64_t>::max())
            return expr;
          auto ne = std::make_unique<nari::NumberExpr>(static_cast<int64_t>(v));
          copy_loc(ne.get(), binaryExpr);
          return ne;
        }
        if (op == "/") {
          double val =
              (ri == 0) ? 0.0
                        : (static_cast<double>(li) / static_cast<double>(ri));
          auto numExpr = std::make_unique<nari::NumberExpr>(val);
          copy_loc(numExpr.get(), binaryExpr);
          return numExpr;
        }
        if (op == "%") {
          if (ri == 0) {
            auto result =
                std::make_unique<nari::NumberExpr>(static_cast<int64_t>(0));
            copy_loc(result.get(), binaryExpr);
            return result;
          }
          auto result = std::make_unique<nari::NumberExpr>(li % ri);
          copy_loc(result.get(), binaryExpr);
          return result;
        }
        if (op == "**") {
          if (ri < 0) {
            auto result = std::make_unique<nari::NumberExpr>(
                std::pow(static_cast<double>(li), static_cast<double>(ri)));
            copy_loc(result.get(), binaryExpr);
            return result;
          }
          if (ri > std::numeric_limits<uint64_t>::max())
            return expr;
          uint64_t exp = (uint64_t)ri;
          int64_t base = li;
          int64_t total = 1;
          while (exp > 0) {
            if (exp & 1ULL) {
              __int128 v = (__int128)total * (__int128)base;
              if (v < std::numeric_limits<int64_t>::min() ||
                  v > std::numeric_limits<int64_t>::max())
                return expr;
              total = static_cast<int64_t>(v);
            }
            if (exp > 1) {
              __int128 v = (__int128)base * (__int128)base;
              if (v < std::numeric_limits<int64_t>::min() ||
                  v > std::numeric_limits<int64_t>::max())
                return expr;
              base = static_cast<int64_t>(v);
            }
            exp >>= 1ULL;
          }
          auto result =
              std::make_unique<nari::NumberExpr>(static_cast<int64_t>(total));
          copy_loc(result.get(), binaryExpr);
          return result;
        }
      }

      if ((lfloat || rfloat) && (lint || lfloat) && (rint || rfloat)) {
        double l = lfloat ? lf
                          : (lint ? (double)li
                                  : number_to_double(
                                        dynamic_cast<const nari::NumberExpr *>(
                                            binaryExpr->left.get())));
        double r = rfloat ? rf
                          : (rint ? (double)ri
                                  : number_to_double(
                                        dynamic_cast<const nari::NumberExpr *>(
                                            binaryExpr->right.get())));
        if (op == "+") {
          auto result = std::make_unique<nari::NumberExpr>(l + r);
          copy_loc(result.get(), binaryExpr);
          return result;
        }
        if (op == "-") {
          auto result = std::make_unique<nari::NumberExpr>(l - r);
          copy_loc(result.get(), binaryExpr);
          return result;
        }
        if (op == "/") {
          double v = (r == 0.0) ? 0.0 : (l / r);
          auto result = std::make_unique<nari::NumberExpr>(v);
          copy_loc(result.get(), binaryExpr);
          return result;
        }
        if (op == "%") {
          double v = (r == 0.0) ? 0.0 : std::fmod(l, r);
          auto result = std::make_unique<nari::NumberExpr>(v);
          copy_loc(result.get(), binaryExpr);
          return result;
        }
        if (op == "**") {
          auto result = std::make_unique<nari::NumberExpr>(std::pow(l, r));
          copy_loc(result.get(), binaryExpr);
          return result;
        }
      }

      if (op == "@" && literal_any(binaryExpr->left.get()) &&
          literal_any(binaryExpr->right.get()) && (lstr || rstr)) {
        std::string out = literal_to_string(binaryExpr->left.get()) +
                          literal_to_string(binaryExpr->right.get());
        auto result = std::make_unique<nari::StringExpr>(out);
        copy_loc(result.get(), binaryExpr);
        return result;
      }

      if (op == "==" || op == "!=") {
        bool eq = false;
        if (lint && rint) {
          eq = (li == ri);
        } else if ((lfloat || rfloat) && (lint || lfloat) && (rint || rfloat)) {
          double l = lfloat
                         ? lf
                         : (lint ? static_cast<double>(li)
                                 : number_to_double(
                                       dynamic_cast<const nari::NumberExpr *>(
                                           binaryExpr->left.get())));
          double r = rfloat
                         ? rf
                         : (rint ? static_cast<double>(ri)
                                 : number_to_double(
                                       dynamic_cast<const nari::NumberExpr *>(
                                           binaryExpr->right.get())));
          eq = std::fabs(l - r) < 1e-12;
        } else if (lbool && rbool) {
          eq = (lb == rb);
        } else if ((lstr || lbool || lint || lfloat) &&
                   (rstr || rbool || rint || rfloat)) {
          eq = literal_to_string(binaryExpr->left.get()) ==
               literal_to_string(binaryExpr->right.get());
        } else {
          return expr;
        }
        auto result = std::make_unique<nari::BoolExpr>(op == "==" ? eq : !eq);
        copy_loc(result.get(), binaryExpr);
        return result;
      }

      if (lint && rint) {
        if (op == "<") {
          auto result = std::make_unique<nari::BoolExpr>(li < ri);
          copy_loc(result.get(), binaryExpr);
          return result;
        }
        if (op == ">") {
          auto result = std::make_unique<nari::BoolExpr>(li > ri);
          copy_loc(result.get(), binaryExpr);
          return result;
        }
        if (op == "<=") {
          auto result = std::make_unique<nari::BoolExpr>(li <= ri);
          copy_loc(result.get(), binaryExpr);
          return result;
        }
        if (op == ">=") {
          auto result = std::make_unique<nari::BoolExpr>(li >= ri);
          copy_loc(result.get(), binaryExpr);
          return result;
        }
      }

      if ((lfloat || rfloat) && (lint || lfloat) && (rint || rfloat)) {
        double l = lfloat ? lf
                          : (lint ? static_cast<double>(li)
                                  : number_to_double(
                                        dynamic_cast<const nari::NumberExpr *>(
                                            binaryExpr->left.get())));
        double r = rfloat ? rf
                          : (rint ? static_cast<double>(ri)
                                  : number_to_double(
                                        dynamic_cast<const nari::NumberExpr *>(
                                            binaryExpr->right.get())));
        if (op == "<") {
          auto result = std::make_unique<nari::BoolExpr>(l < r);
          copy_loc(result.get(), binaryExpr);
          return result;
        }
        if (op == ">") {
          auto result = std::make_unique<nari::BoolExpr>(l > r);
          copy_loc(result.get(), binaryExpr);
          return result;
        }
        if (op == "<=") {
          auto result = std::make_unique<nari::BoolExpr>(l <= r);
          copy_loc(result.get(), binaryExpr);
          return result;
        }
        if (op == ">=") {
          auto result = std::make_unique<nari::BoolExpr>(l >= r);
          copy_loc(result.get(), binaryExpr);
          return result;
        }
      }

      if (lbool && rbool) {
        if (op == "&&") {
          auto result = std::make_unique<nari::BoolExpr>(lb && rb);
          copy_loc(result.get(), binaryExpr);
          return result;
        }
        if (op == "||") {
          auto result = std::make_unique<nari::BoolExpr>(lb || rb);
          copy_loc(result.get(), binaryExpr);
          return result;
        }
      }

      return expr;
    }

    if (auto *te = dynamic_cast<nari::TernaryExpr *>(expr.get())) {
      te->condition = fold_expr(std::move(te->condition));
      te->true_expr = fold_expr(std::move(te->true_expr));
      te->false_expr = fold_expr(std::move(te->false_expr));
      bool cond = false;
      if (literal_bool(te->condition.get(), cond)) {
        return cond ? std::move(te->true_expr) : std::move(te->false_expr);
      }
      return expr;
    }

    if (auto *ce = dynamic_cast<nari::CallExpr *>(expr.get())) {
      ce->callee = fold_expr(std::move(ce->callee));
      for (auto &arg : ce->args) {
        arg = fold_expr(std::move(arg));
      }
      return expr;
    }

    if (auto *ae = dynamic_cast<nari::ArrayLiteralExpr *>(expr.get())) {
      for (auto &el : ae->elements) {
        el = fold_expr(std::move(el));
      }
      return expr;
    }

    if (auto *oe = dynamic_cast<nari::ObjectLiteralExpr *>(expr.get())) {
      for (auto &entry : oe->entries) {
        entry.second = fold_expr(std::move(entry.second));
      }
      return expr;
    }

    if (auto *ie = dynamic_cast<nari::IndexExpr *>(expr.get())) {
      ie->object = fold_expr(std::move(ie->object));
      ie->index = fold_expr(std::move(ie->index));
      return expr;
    }

    if (auto *me = dynamic_cast<nari::MemberExpr *>(expr.get())) {
      me->object = fold_expr(std::move(me->object));
      return expr;
    }

    return expr;
  }

  bool looks_like_arrow_function() {
    if (peek().kind != TokenKind::TK_LPAREN)
      return false;

    // lookahead to find matching ) and check for =>
    size_t i = idx + 1;
    int depth = 1;
    while (i < toks.size() && depth > 0) {
      if (toks[i].kind == TokenKind::TK_LPAREN)
        ++depth;
      else if (toks[i].kind == TokenKind::TK_RPAREN)
        --depth;
      else if (toks[i].kind == TokenKind::TK_EOF)
        return false;
      ++i;
    }

    // check if next token after matching ) is =>
    if (i < toks.size() && toks[i].kind == TokenKind::TK_FATARROW)
      return true;
      
    return false;
  }

  // parse function expression: func(params) { ... }
  ExprPtr parse_function_expression() {
    const Token &funcTok = peek();
    expect(TokenKind::TK_IDENT, "func keyword");

    auto func_expr = std::make_unique<nari::FunctionExpr>();
    func_expr->line = funcTok.line;
    func_expr->col = funcTok.col;
    func_expr->filename =
        funcTok.filename.empty() ? g_current_filename : funcTok.filename;

    // parse parameters
    expect(TokenKind::TK_LPAREN, "function params start '('");
    bool seen_rest = false;
    if (peek().kind != TokenKind::TK_RPAREN) {
      while (true) {
        bool is_rest = false;
        if (peek().kind == TokenKind::TK_ELLIPSIS) {
          if (seen_rest)
            error_and_exit("Only one rest parameter is allowed");
          seen_rest = true;
          is_rest = true;
          next(); // consume '...'
        }

        const Token &p = peek();
        if (p.kind != TokenKind::TK_IDENT) {
          error_and_exit("Expected parameter name");
        }
        std::string pname = next().text;

        // parse optional type annotation: name: type or name: type[]
        nari::TypeAnnotationPtr param_type = nullptr;
        if (peek().kind == TokenKind::TK_COLON) {
          next(); // consume ':'
          if (peek().kind != TokenKind::TK_IDENT) {
            error_and_exit("Expected type name after ':'");
          }
          std::string type_name = next().text;
          bool is_array = false;
          // Check for array syntax: type[]
          if (peek().kind == TokenKind::TK_LBRACKET &&
              peek(1).kind == TokenKind::TK_RBRACKET) {
            next(); // consume '['
            next(); // consume ']'
            is_array = true;
          }
          param_type =
              std::make_unique<nari::TypeAnnotation>(type_name, is_array);
        }

        ExprPtr default_value = nullptr;
        if (!is_rest && peek().kind == TokenKind::TK_EQUAL) {
          next(); // consume '='
          default_value = parse_expression();
        } else if (is_rest && peek().kind == TokenKind::TK_EQUAL) {
          error_and_exit("Rest parameter cannot have a default value");
        }

        func_expr->params.emplace_back(pname, std::move(default_value), is_rest,
                                       std::move(param_type));

        if (is_rest) {
          if (peek().kind == TokenKind::TK_COMMA) {
            error_and_exit("Rest parameter must be last");
          }
          break;
        }

        if (peek().kind == TokenKind::TK_COMMA)
          next();
        else
          break;
      }
    }
    expect(TokenKind::TK_RPAREN, "function params end ')'");

    // parse optional return type: -> type or -> type[]
    if (peek().kind == TokenKind::TK_ARROW) {
      next(); // consume '->'
      if (peek().kind != TokenKind::TK_IDENT) {
        error_and_exit("Expected return type after '->'");
      }
      std::string return_type_name = next().text;
      bool is_array = false;
      // Check for array syntax: type[]
      if (peek().kind == TokenKind::TK_LBRACKET &&
          peek(1).kind == TokenKind::TK_RBRACKET) {
        next(); // consume '['
        next(); // consume ']'
        is_array = true;
      }
      func_expr->return_type =
          std::make_unique<nari::TypeAnnotation>(return_type_name, is_array);
    }

    func_expr->body = parse_block();
    return std::move(func_expr);
  }

  // parse spawn expression: spawn { ... }
  ExprPtr parse_spawn_expression() {
    const Token &spawnTok = peek();
    expect(TokenKind::TK_IDENT, "spawn keyword");

    auto spawn_expr = std::make_unique<nari::SpawnExpr>(nullptr);
    spawn_expr->line = spawnTok.line;
    spawn_expr->col = spawnTok.col;
    spawn_expr->filename =
        spawnTok.filename.empty() ? g_current_filename : spawnTok.filename;

    // parse the block
    spawn_expr->body = parse_block();
    return spawn_expr;
  }

  // parse arrow function expression: (params) => { ... } or (params) => expr
  ExprPtr parse_arrow_function_expression() {
    const Token &parenTok = peek();
    auto func_expr = std::make_unique<nari::FunctionExpr>();
    func_expr->line = parenTok.line;
    func_expr->col = parenTok.col;
    func_expr->filename =
        parenTok.filename.empty() ? g_current_filename : parenTok.filename;

    // Parse parameters
    expect(TokenKind::TK_LPAREN, "arrow function params start '('");
    bool seen_rest = false;
    if (peek().kind != TokenKind::TK_RPAREN) {
      while (true) {
        bool is_rest = false;
        if (peek().kind == TokenKind::TK_ELLIPSIS) {
          if (seen_rest)
            error_and_exit("Only one rest parameter is allowed");
          seen_rest = true;
          is_rest = true;
          next(); // consume '...'
        }

        const Token &p = peek();
        if (p.kind != TokenKind::TK_IDENT) {
          error_and_exit("Expected parameter name");
        }
        std::string pname = next().text;

        // Parse optional type annotation: name: type or name: type[]
        nari::TypeAnnotationPtr param_type = nullptr;
        if (peek().kind == TokenKind::TK_COLON) {
          next(); // consume ':'
          if (peek().kind != TokenKind::TK_IDENT) {
            error_and_exit("Expected type name after ':'");
          }
          std::string type_name = next().text;
          bool is_array = false;
          // Check for array syntax: type[]
          if (peek().kind == TokenKind::TK_LBRACKET &&
              peek(1).kind == TokenKind::TK_RBRACKET) {
            next(); // consume '['
            next(); // consume ']'
            is_array = true;
          }
          param_type =
              std::make_unique<nari::TypeAnnotation>(type_name, is_array);
        }

        ExprPtr default_value = nullptr;
        if (!is_rest && peek().kind == TokenKind::TK_EQUAL) {
          next(); // consume '='
          default_value = parse_expression();
        } else if (is_rest && peek().kind == TokenKind::TK_EQUAL) {
          error_and_exit("Rest parameter cannot have a default value");
        }

        func_expr->params.emplace_back(pname, std::move(default_value), is_rest,
                                       std::move(param_type));

        if (is_rest) {
          if (peek().kind == TokenKind::TK_COMMA) {
            error_and_exit("Rest parameter must be last");
          }
          break;
        }

        if (peek().kind == TokenKind::TK_COMMA)
          next();
        else
          break;
      }
    }
    expect(TokenKind::TK_RPAREN, "arrow function params end ')'");
    
    expect(TokenKind::TK_FATARROW, "'=>' in arrow function");

    // Parse body: either a block or a single expression
    if (peek().kind == TokenKind::TK_LBRACE) {
      // Block body: (x) => { return x + 1; }
      func_expr->body = parse_block();
    } else {
      // Expression body: (x) => x + 1
      // Convert to implicit return
      ExprPtr body_expr = parse_expression();
      auto ret_stmt = std::make_unique<nari::ReturnStmt>(std::move(body_expr));
      ret_stmt->line = func_expr->line;
      ret_stmt->col = func_expr->col;
      ret_stmt->filename = func_expr->filename;
      
      auto block = std::make_unique<nari::BlockStmt>();
      block->stmts.push_back(std::move(ret_stmt));
      
      func_expr->body = std::move(block);
    }
    
    return std::move(func_expr);
  }

  // parse assignment, expression-statement, and block statements.
  StmtPtr parse_stmt() {
    const Token &tok = peek();
    if (tok.kind == TokenKind::TK_IDENT) {
      // control flow keywords
      if (tok.text == "if") {
        next(); // consume 'if'
        expect(TokenKind::TK_LPAREN, "if condition (");
        ExprPtr cond = parse_expression();
        expect(TokenKind::TK_RPAREN, "if condition )");
        StmtPtr then_branch = parse_stmt();
        if (!then_branch) {
          error_and_exit("Expected statement or block after if condition");
        }
        StmtPtr else_branch = nullptr;
        if (peek().kind == TokenKind::TK_IDENT && peek().text == "else") {
          next(); // consume 'else'
          else_branch = parse_stmt();
          if (!else_branch) {
            error_and_exit("Expected statement or block after else");
          }
        }
        auto ifs = std::make_unique<nari::IfStmt>(
            std::move(cond), std::move(then_branch), std::move(else_branch));
        ifs->line = tok.line;
        ifs->col = tok.col;
        ifs->filename =
            tok.filename.empty() ? g_current_filename : tok.filename;
        return ifs;
      }

      if (tok.text == "while") {
        next(); // consume tok

        expect(TokenKind::TK_LPAREN, "while condition (");
        ExprPtr cond = parse_expression();
        expect(TokenKind::TK_RPAREN, "while condition )");
        StmtPtr body = parse_block();
        auto ws =
            std::make_unique<nari::WhileStmt>(std::move(cond), std::move(body));
        ws->line = tok.line;
        ws->col = tok.col;
        ws->filename = tok.filename.empty() ? g_current_filename : tok.filename;
        return ws;
      }

      if (tok.text == "for") {
        if (looks_like_foreach_loop()) {
          Token kw = tok;
          next(); // consume 'for'
          expect(TokenKind::TK_LPAREN, "for (");
          Token varTok = peek();
          expect(TokenKind::TK_IDENT, "foreach variable");
          if (peek().kind != TokenKind::TK_IDENT || peek().text != "in") {
            error_and_exit("Expected 'in' in for-each loop");
          }
          next(); // consume 'in'
          ExprPtr iterable = parse_expression();
          expect(TokenKind::TK_RPAREN, "for )");
          StmtPtr body = parse_block();
          auto fs = std::make_unique<nari::ForEachStmt>(
              varTok.text, std::move(iterable), std::move(body));
          fs->line = kw.line;
          fs->col = kw.col;
          fs->filename = kw.filename.empty() ? g_current_filename : kw.filename;
          return fs;
        }

        Token kw = tok;
        next(); // consume 'for'
        expect(TokenKind::TK_LPAREN, "for (");
        // either a declaration or an expression statement (or empty)
        StmtPtr init = nullptr;
        if (peek().kind != TokenKind::TK_SEMICOLON) {
          if (peek().kind == TokenKind::TK_IDENT &&
              (peek().text == "let" || peek().text == "global")) {
            init = parse_stmt();
          } else {
            // Handle assignment (i = 0) or expression
            if (peek().kind == TokenKind::TK_IDENT &&
                peek(1).kind == TokenKind::TK_EQUAL) {
              const Token &nameTok = peek();
              std::string pname = next().text; // consume identifier
              next();                          // consume '='

              ExprPtr prhs = parse_expression();
              auto assign =
                  std::make_unique<nari::AssignStmt>(pname, std::move(prhs));
              assign->line = nameTok.line;
              assign->col = nameTok.col;
              assign->filename = nameTok.filename.empty() ? g_current_filename
                                                          : nameTok.filename;
              init = std::move(assign);
            } else {
              init = std::make_unique<nari::ExprStmt>(parse_expression());
            }
            if (peek().kind == TokenKind::TK_SEMICOLON)
              next();
          }
        } else {
          next();
        }

        ExprPtr cond = nullptr;
        if (peek().kind != TokenKind::TK_SEMICOLON) {
          cond = parse_expression();
        }
        expect(TokenKind::TK_SEMICOLON, "for ;");
        StmtPtr post = nullptr;
        if (peek().kind != TokenKind::TK_RPAREN) {
          // allow either an expression (e.g. i = i + 1) or an assignment
          // statement as the post clause. detect an assignment (=) and
          // construct an AssignStmt for it. otherwise, parse an expression and
          // wrap it in an ExprStmt as before.
          if (peek().kind == TokenKind::TK_IDENT &&
              peek(1).kind == TokenKind::TK_EQUAL) {
            const Token &nameTok = peek();
            std::string pname = next().text; // consume identifier
            next();                          // consume '='

            ExprPtr prhs = parse_expression();
            auto assign =
                std::make_unique<nari::AssignStmt>(pname, std::move(prhs));
            assign->line = nameTok.line;
            assign->col = nameTok.col;
            assign->filename = nameTok.filename.empty() ? g_current_filename
                                                        : nameTok.filename;
            post = std::move(assign);
          } else {
            post = std::make_unique<nari::ExprStmt>(parse_expression());
          }
        }
        expect(TokenKind::TK_RPAREN, "for )");
        StmtPtr body = parse_block();
        auto fs = std::make_unique<nari::ForStmt>(
            std::move(init), std::move(cond), std::move(post), std::move(body));
        fs->line = kw.line;
        fs->col = kw.col;
        fs->filename = kw.filename.empty() ? g_current_filename : kw.filename;
        return fs;
      }

      if (tok.text == "switch") {
        return parse_switch_stmt();
      }

      if (tok.text == "break") {
        next();
        if (peek().kind == TokenKind::TK_SEMICOLON)
          next();
        auto bs = std::make_unique<nari::BreakStmt>();
        bs->line = tok.line;
        bs->col = tok.col;
        bs->filename = tok.filename.empty() ? g_current_filename : tok.filename;
        return bs;
      }

      if (tok.text == "continue") {
        next();
        if (peek().kind == TokenKind::TK_SEMICOLON)
          next();
        auto cs = std::make_unique<nari::ContinueStmt>();
        cs->line = tok.line;
        cs->col = tok.col;
        cs->filename = tok.filename.empty() ? g_current_filename : tok.filename;
        return cs;
      }

      if (tok.text == "return") {
        next();
        ExprPtr val = nullptr;
        // optional return value
        if (peek().kind != TokenKind::TK_SEMICOLON &&
            peek().kind != TokenKind::TK_RBRACE &&
            peek().kind != TokenKind::TK_EOF) {
          val = parse_expression();
        }
        if (peek().kind == TokenKind::TK_SEMICOLON)
          next();
        auto rs = std::make_unique<nari::ReturnStmt>(std::move(val));
        rs->line = tok.line;
        rs->col = tok.col;
        rs->filename = tok.filename.empty() ? g_current_filename : tok.filename;
        return rs;
      }

      if (tok.text == "throw") {
        next();
        ExprPtr val = nullptr;
        if (peek().kind == TokenKind::TK_SEMICOLON ||
            peek().kind == TokenKind::TK_RBRACE ||
            peek().kind == TokenKind::TK_EOF) {
          error_and_exit("throw requires a value");
        }
        val = parse_expression();
        if (peek().kind == TokenKind::TK_SEMICOLON)
          next();
        auto ts = std::make_unique<nari::ThrowStmt>(std::move(val));
        ts->line = tok.line;
        ts->col = tok.col;
        ts->filename = tok.filename.empty() ? g_current_filename : tok.filename;
        return ts;
      }

      if (tok.text == "try") {
        return parse_try_stmt();
      }

      // import statement: `import name from "library.so"`
      if (tok.text == "import") {
        Token importTok = tok;
        next(); // consume 'import'

        // check if this is "import name from" syntax (ffi)
        if (peek().kind == TokenKind::TK_IDENT) {
          std::string var_name = next().text;

          if (peek().kind != TokenKind::TK_IDENT || peek().text != "from") {
            error_and_exit("Expected 'from' after import variable name");
          }
          next(); // consume "from"

          if (peek().kind != TokenKind::TK_STRING) {
            error_and_exit("Expected library path string after 'from'");
          }
          std::string lib_path = next().text;

          // create `let var_name = __ffi_load_library("path");`
          auto load_call = std::make_unique<nari::CallExpr>(
              std::make_unique<nari::IdentExpr>("__ffi_load_library"));
          load_call->args.push_back(
              std::make_unique<nari::StringExpr>(lib_path));

          if (peek().kind == TokenKind::TK_SEMICOLON)
            next();

          auto var_decl = std::make_unique<nari::VarDeclStmt>(
              var_name, std::move(load_call), VarDeclCtrl::LOCAL);
          var_decl->line = importTok.line;
          var_decl->col = importTok.col;
          var_decl->filename = importTok.filename.empty() ? g_current_filename
                                                          : importTok.filename;

          return var_decl;
        } else {
          error_and_exit("import inside functions only supports 'import name "
                         "from \"library.so\"' syntax");
        }
      }

      // variable declaration: `let IDENT = expr` or `global IDENT = expr`
      // or destructuring: `let [a, b] = expr` or `let {x, y} = expr`
      if (tok.text == "let" || tok.text == "global") {
        VarDeclCtrl is_global = (VarDeclCtrl)(tok.text == "global");
        Token keyword = tok;
        next(); // consume 'let' or 'global'

        // Check for array destructuring: let [a, b, c] = ...
        if (peek().kind == TokenKind::TK_LBRACKET) {
          next(); // consume '['
          
          std::vector<std::string> names;
          while (peek().kind != TokenKind::TK_RBRACKET && !is_eof()) {
            if (peek().kind != TokenKind::TK_IDENT) {
              error_and_exit("Expected identifier in array destructuring");
            }
            names.push_back(next().text);
            
            if (peek().kind == TokenKind::TK_COMMA) {
              next(); // consume ','
            } else if (peek().kind != TokenKind::TK_RBRACKET) {
              error_and_exit("Expected ',' or ']' in array destructuring");
            }
          }
          expect(TokenKind::TK_RBRACKET, "']' to close array destructuring");
          
          ExprPtr init = nullptr;
          if (peek().kind == TokenKind::TK_EQUAL) {
            next(); // consume '='
            init = parse_expression();
          } else {
            error_and_exit("Array destructuring requires initialization");
          }
          
          if (peek().kind == TokenKind::TK_SEMICOLON)
            next();
            
          auto decl = std::make_unique<nari::VarDeclStmt>("", std::move(init), is_global);
          decl->destructure_kind = nari::DestructureKind::Array;
          decl->array_names = std::move(names);
          decl->line = keyword.line;
          decl->col = keyword.col;
          decl->filename = keyword.filename.empty() ? g_current_filename : keyword.filename;
          return decl;
        }
        
        // check for object destructuring: let {x, y} = ... or let {a: x, b: y} = ...
        if (peek().kind == TokenKind::TK_LBRACE) {
          next(); // consume '{'
          
          std::vector<std::pair<std::string, std::string>> bindings;
          while (peek().kind != TokenKind::TK_RBRACE && !is_eof()) {
            if (peek().kind != TokenKind::TK_IDENT) {
              error_and_exit("Expected identifier in object destructuring");
            }
            std::string key = next().text;
            std::string name = key;  // default: use same name
            
            // check for key: name syntax
            if (peek().kind == TokenKind::TK_COLON) {
              next(); // consume ':'
              if (peek().kind != TokenKind::TK_IDENT) {
                error_and_exit("Expected identifier after ':' in object destructuring");
              }
              name = next().text;
            }
            
            bindings.emplace_back(key, name);
            
            if (peek().kind == TokenKind::TK_COMMA) {
              next(); // consume ','
            } else if (peek().kind != TokenKind::TK_RBRACE) {
              error_and_exit("Expected ',' or '}' in object destructuring");
            }
          }
          expect(TokenKind::TK_RBRACE, "'}' to close object destructuring");
          
          ExprPtr init = nullptr;
          if (peek().kind == TokenKind::TK_EQUAL) {
            next(); // consume '='
            init = parse_expression();
          } else {
            error_and_exit("Object destructuring requires initialization");
          }
          
          if (peek().kind == TokenKind::TK_SEMICOLON)
            next();
            
          auto decl = std::make_unique<nari::VarDeclStmt>("", std::move(init), is_global);
          decl->destructure_kind = nari::DestructureKind::Object;
          decl->object_bindings = std::move(bindings);
          decl->line = keyword.line;
          decl->col = keyword.col;
          decl->filename = keyword.filename.empty() ? g_current_filename : keyword.filename;
          return decl;
        }

        // simple variable declaration
        const Token &nameTok = peek();
        if (nameTok.kind != TokenKind::TK_IDENT) {
          error_and_exit(std::string(is_global
                                         ? "Expected identifier after 'global'"
                                         : "Expected identifier after 'let'"));
        }
        std::string name = next().text;
        ExprPtr init = nullptr;
        if (peek().kind == TokenKind::TK_EQUAL) {
          next(); // consume '='
          init = parse_expression();
        }
        if (peek().kind == TokenKind::TK_SEMICOLON)
          next();
        auto decl = std::make_unique<nari::VarDeclStmt>(name, std::move(init),
                                                        is_global);
        decl->line = keyword.line;
        decl->col = keyword.col;
        decl->filename =
            keyword.filename.empty() ? g_current_filename : keyword.filename;
        return decl;
      }

      // '=' expr (simple assignment or indexed assignment)
      if (peek(1).kind == TokenKind::TK_EQUAL) {
        std::string name = next().text;
        next(); // consume '='

        ExprPtr rhs = parse_expression();
        if (peek().kind == TokenKind::TK_SEMICOLON)
          next();
        auto as = std::make_unique<nari::AssignStmt>(name, std::move(rhs));
        as->line = tok.line;
        as->col = tok.col;
        as->filename = tok.filename.empty() ? g_current_filename : tok.filename;
        return as;
      }

      // Compound assignment: +=, -=, *=, /=, %=, &=, |=, ^=, <<=, >>=
      TokenKind next_kind = peek(1).kind;
      if (next_kind == TokenKind::TK_PLUSEQ ||
          next_kind == TokenKind::TK_MINUSEQ ||
          next_kind == TokenKind::TK_STAREQ ||
          next_kind == TokenKind::TK_SLASHEQ ||
          next_kind == TokenKind::TK_PERCENTEQ ||
          next_kind == TokenKind::TK_AMPEQ ||
          next_kind == TokenKind::TK_PIPEEQ ||
          next_kind == TokenKind::TK_CARETEQ ||
          next_kind == TokenKind::TK_LSHIFTEQ ||
          next_kind == TokenKind::TK_RSHIFTEQ) {
        std::string name = next().text;
        Token opTok = next(); // consume compound operator

        // Determine the binary operator
        std::string binop;
        if (opTok.kind == TokenKind::TK_PLUSEQ)
          binop = "+";
        else if (opTok.kind == TokenKind::TK_MINUSEQ)
          binop = "-";
        else if (opTok.kind == TokenKind::TK_STAREQ)
          binop = "*";
        else if (opTok.kind == TokenKind::TK_SLASHEQ)
          binop = "/";
        else if (opTok.kind == TokenKind::TK_PERCENTEQ)
          binop = "%";
        else if (opTok.kind == TokenKind::TK_AMPEQ)
          binop = "&";
        else if (opTok.kind == TokenKind::TK_PIPEEQ)
          binop = "|";
        else if (opTok.kind == TokenKind::TK_CARETEQ)
          binop = "^";
        else if (opTok.kind == TokenKind::TK_LSHIFTEQ)
          binop = "<<";
        else if (opTok.kind == TokenKind::TK_RSHIFTEQ)
          binop = ">>";

        ExprPtr rhs = parse_expression();

        // name = name <operation> rhs (i.e name = name + rhs)
        auto lhs = std::make_unique<nari::IdentExpr>(name);
        lhs->line = tok.line;
        lhs->col = tok.col;
        lhs->filename =
            tok.filename.empty() ? g_current_filename : tok.filename;

        auto binexpr = std::make_unique<nari::BinaryExpr>(binop, std::move(lhs),
                                                          std::move(rhs));
        binexpr->line = opTok.line;
        binexpr->col = opTok.col;
        binexpr->filename =
            opTok.filename.empty() ? g_current_filename : opTok.filename;

        if (peek().kind == TokenKind::TK_SEMICOLON)
          next();
        auto as = std::make_unique<nari::AssignStmt>(name, std::move(binexpr));
        as->line = tok.line;
        as->col = tok.col;
        as->filename = tok.filename.empty() ? g_current_filename : tok.filename;
        return as;
      }

      // check for indexed/member assignment: expr["member"] = val, or
      // expr.member = val this part is kinda a mess :)
      bool is_indexed_assign = false;
      size_t lookahead_idx = 1;
      while (lookahead_idx < toks.size()) {
        TokenKind k = peek(lookahead_idx).kind;
        if (k == TokenKind::TK_LBRACKET || k == TokenKind::TK_DOT) {
          lookahead_idx++;

          if (k == TokenKind::TK_LBRACKET) {
            // skip to matching ]
            int depth = 1;
            while (lookahead_idx < toks.size() && depth > 0) {
              if (peek(lookahead_idx).kind == TokenKind::TK_LBRACKET)
                depth++;
              else if (peek(lookahead_idx).kind == TokenKind::TK_RBRACKET)
                depth--;
              lookahead_idx++;
            }
          } else if (k == TokenKind::TK_DOT) {
            // skip identifier
            if (lookahead_idx < toks.size() &&
                peek(lookahead_idx).kind == TokenKind::TK_IDENT) {
              lookahead_idx++;
            }
          }
        } else if (k == TokenKind::TK_EQUAL) {
          is_indexed_assign = true;
          break;
        } else {
          break;
        }
      }

      if (is_indexed_assign) {
        // Parse the target as an expression (which will be IndexExpr or
        // MemberExpr)
        ExprPtr target = parse_expression();
        expect(TokenKind::TK_EQUAL, "indexed assignment =");
        ExprPtr value = parse_expression();
        if (peek().kind == TokenKind::TK_SEMICOLON)
          next();
        auto idxAssignStmt = std::make_unique<nari::IndexAssignStmt>(
            std::move(target), std::move(value));
        idxAssignStmt->line = tok.line;
        idxAssignStmt->col = tok.col;
        idxAssignStmt->filename =
            tok.filename.empty() ? g_current_filename : tok.filename;
        return idxAssignStmt;
      }

      // call or expr statement
      if (peek(1).kind == TokenKind::TK_LPAREN) {
        ExprPtr e = parse_expression();
        if (peek().kind == TokenKind::TK_SEMICOLON)
          next();
        auto exprStmt = std::make_unique<nari::ExprStmt>(std::move(e));
        exprStmt->line = tok.line;
        exprStmt->col = tok.col;
        exprStmt->filename =
            tok.filename.empty() ? g_current_filename : tok.filename;
        return exprStmt;
      }

      ExprPtr expr = parse_expression();
      if (peek().kind == TokenKind::TK_SEMICOLON)
        next();
      auto exprStmt = std::make_unique<nari::ExprStmt>(std::move(expr));
      exprStmt->line = tok.line;
      exprStmt->col = tok.col;
      exprStmt->filename =
          tok.filename.empty() ? g_current_filename : tok.filename;
      return exprStmt;
    } else if (tok.kind == TokenKind::TK_LBRACE) {
      auto blk = parse_block();
      return blk;
    } else if (tok.kind == TokenKind::TK_PLUSPLUS ||
               tok.kind == TokenKind::TK_MINUSMINUS) {
      // handle prefix increment/decrement as statements
      ExprPtr e = parse_expression();
      if (peek().kind == TokenKind::TK_SEMICOLON)
        next();
      auto es = std::make_unique<nari::ExprStmt>(std::move(e));
      es->line = tok.line;
      es->col = tok.col;
      es->filename = tok.filename.empty() ? g_current_filename : tok.filename;
      return es;
    } else if (tok.kind == TokenKind::TK_STRING) {
      ExprPtr s = parse_expression();
      if (peek().kind == TokenKind::TK_SEMICOLON)
        next();
      auto es = std::make_unique<nari::ExprStmt>(std::move(s));
      es->line = tok.line;
      es->col = tok.col;
      es->filename = tok.filename.empty() ? g_current_filename : tok.filename;
      return es;
    } else if (tok.kind == TokenKind::TK_EOF) {
      return nullptr;
    } else {
      error_and_exit("Unexpected token in statement: '" + token_desc(tok) +
                     "'");
      __builtin_unreachable();
    }
  }

  StmtPtr parse_try_stmt() {
    const Token &kw = peek();
    expect(TokenKind::TK_IDENT, "try");
    auto try_block = parse_block();

    std::string catch_var;
    BlockPtr catch_block = nullptr;
    BlockPtr finally_block = nullptr;

    if (peek().kind == TokenKind::TK_IDENT && peek().text == "catch") {
      next(); // consume 'catch'
      expect(TokenKind::TK_LPAREN, "Missing start `(`");
      const Token &varTok = peek();
      expect(TokenKind::TK_IDENT, "Catch requires a variable");
      catch_var = varTok.text;
      expect(TokenKind::TK_RPAREN, "Missing end `)`");
      catch_block = parse_block();
    }

    if (peek().kind == TokenKind::TK_IDENT && peek().text == "finally") {
      next(); // consume 'finally'
      finally_block = parse_block();
    }

    if (!catch_block && !finally_block) {
      error_and_exit("Try must have catch block.");
    }

    auto ts = std::make_unique<nari::TryStmt>(std::move(try_block), catch_var,
                                              std::move(catch_block),
                                              std::move(finally_block));
    ts->line = kw.line;
    ts->col = kw.col;
    ts->filename = kw.filename.empty() ? g_current_filename : kw.filename;
    return ts;
  }

  StmtPtr parse_switch_stmt() {
    const Token &kw = peek();
    expect(TokenKind::TK_IDENT, "switch");
    expect(TokenKind::TK_LPAREN, "switch (");
    ExprPtr value = parse_expression();
    expect(TokenKind::TK_RPAREN, "switch )");
    expect(TokenKind::TK_LBRACE, "switch { ");

    auto sw = std::make_unique<nari::SwitchStmt>(std::move(value));
    sw->line = kw.line;
    sw->col = kw.col;
    sw->filename = kw.filename.empty() ? g_current_filename : kw.filename;

    bool seen_default = false;
    while (!is_eof() && peek().kind != TokenKind::TK_RBRACE) {
      if (peek().kind != TokenKind::TK_IDENT) {
        error_and_exit("Expected 'case' or 'default' in switch");
      }

      if (peek().text == "case") {
        next(); // consume 'case'
        ExprPtr match = parse_expression();
        expect(TokenKind::TK_COLON, "case :");
        auto body = std::make_unique<nari::BlockStmt>();
        while (!is_eof() && peek().kind != TokenKind::TK_RBRACE) {
          if (peek().kind == TokenKind::TK_IDENT &&
              (peek().text == "case" || peek().text == "default")) {
            break;
          }
          StmtPtr st = parse_stmt();
          if (st)
            body->stmts.push_back(std::move(st));
          else
            break;
        }
        sw->cases.emplace_back(std::move(match), std::move(body));
        continue;
      }

      if (peek().text == "default") {
        if (seen_default)
          error_and_exit("Only one default case is allowed");
        seen_default = true;
        next(); // consume 'default'
        expect(TokenKind::TK_COLON, "default :");
        auto body = std::make_unique<nari::BlockStmt>();
        while (!is_eof() && peek().kind != TokenKind::TK_RBRACE) {
          if (peek().kind == TokenKind::TK_IDENT &&
              (peek().text == "case" || peek().text == "default")) {
            break;
          }
          StmtPtr st = parse_stmt();
          if (st)
            body->stmts.push_back(std::move(st));
          else
            break;
        }
        sw->default_body = std::move(body);
        continue;
      }

      error_and_exit("Expected 'case' or 'default' in switch");
    }

    expect(TokenKind::TK_RBRACE, "switch }");
    return sw;
  }

  // higher value = tighter binding
  enum Precedence {
    PREC_NONE = 0,
    PREC_TERNARY = 1,       // ? :
    PREC_NULL_COALESCE = 2, // ??
    PREC_LOGICAL_OR = 3,    // ||
    PREC_LOGICAL_AND = 4,   // &&
    PREC_BITWISE_OR = 5,    // |
    PREC_BITWISE_XOR = 6,   // ^
    PREC_BITWISE_AND = 7,   // &
    PREC_EQUALITY = 8,      // == !=
    PREC_COMPARISON = 9,    // < > <= >=
    PREC_SHIFT = 10,        // << >>
    PREC_TERM = 11,         // + - @
    PREC_FACTOR = 12,       // * / %
    PREC_EXPONENT = 13,     // **
    PREC_UNARY = 14,        // unary + - ! ~
    PREC_PRIMARY = 15
  };

  static Precedence token_precedence(const Token &t) {
    switch (t.kind) {
    case TokenKind::TK_QUESTION:
      return PREC_TERNARY;
    case TokenKind::TK_NULLISHCOALESCE:
      return PREC_NULL_COALESCE;
    case TokenKind::TK_OROR:
      return PREC_LOGICAL_OR;
    case TokenKind::TK_ANDAND:
      return PREC_LOGICAL_AND;
    case TokenKind::TK_PIPE:
      return PREC_BITWISE_OR;
    case TokenKind::TK_CARET:
      return PREC_BITWISE_XOR;
    case TokenKind::TK_AMPERSAND:
      return PREC_BITWISE_AND;
    case TokenKind::TK_EQEQ:
    case TokenKind::TK_NEQ:
      return PREC_EQUALITY;
    case TokenKind::TK_LT:
    case TokenKind::TK_GT:
    case TokenKind::TK_LE:
    case TokenKind::TK_GE:
      return PREC_COMPARISON;
    case TokenKind::TK_LSHIFT:
    case TokenKind::TK_RSHIFT:
      return PREC_SHIFT;
    case TokenKind::TK_PLUS:
    case TokenKind::TK_MINUS:
    case TokenKind::TK_AT:
      return PREC_TERM;
    case TokenKind::TK_STAR:
    case TokenKind::TK_SLASH:
    case TokenKind::TK_PERCENT:
      return PREC_FACTOR;
    case TokenKind::TK_EXPONENT:
      return PREC_EXPONENT;
    default:
      return PREC_NONE;
    }
  }

  ExprPtr parse_expression() {
    return fold_expr(parse_precedence(PREC_TERNARY));
  }

  ExprPtr parse_precedence(Precedence min_prec) {
    ExprPtr lhs = parse_unary();
    if (!lhs)
      return nullptr;

    while (true) {
      const Token &t = peek();
      Precedence prec = token_precedence(t);
      if (prec < min_prec || prec == PREC_NONE)
        break;

      // Special handling for ternary operator
      if (t.kind == TokenKind::TK_QUESTION) {
        Token qmark = t;
        next(); // consume '?'
        ExprPtr true_expr = parse_expression();
        if (!true_expr) {
          error_and_exit("Expected expression after '?'");
        }
        expect(TokenKind::TK_COLON, "':' in ternary operator");
        ExprPtr false_expr =
            parse_precedence(static_cast<Precedence>(prec + 1));
        if (!false_expr) {
          error_and_exit("Expected expression after ':'");
        }
        auto te = std::make_unique<nari::TernaryExpr>(
            std::move(lhs), std::move(true_expr), std::move(false_expr));
        te->line = qmark.line;
        te->col = qmark.col;
        te->filename =
            qmark.filename.empty() ? g_current_filename : qmark.filename;
        lhs = std::move(te);
        continue;
      }

      Token op = t;
      next(); // consume operator

      // For left-assoc operators, the next minimum precedence is prec + 1
      Precedence next_min = static_cast<Precedence>(prec + 1);
      ExprPtr rhs = parse_precedence(next_min);
      if (!rhs) {
        error_and_exit("Expected RHS expression for binary operator");
      }

      std::string opname;
      switch (op.kind) {
      case TokenKind::TK_PLUS:
        opname = "+";
        break;
      case TokenKind::TK_MINUS:
        opname = "-";
        break;
      case TokenKind::TK_STAR:
        opname = "*";
        break;
      case TokenKind::TK_AT:
        opname = "@";
        break;
      case TokenKind::TK_SLASH:
        opname = "/";
        break;
      case TokenKind::TK_PERCENT:
        opname = "%";
        break;
      case TokenKind::TK_EXPONENT:
        opname = "**";
        break;
      case TokenKind::TK_EQEQ:
        opname = "==";
        break;
      case TokenKind::TK_NEQ:
        opname = "!=";
        break;
      case TokenKind::TK_LT:
        opname = "<";
        break;
      case TokenKind::TK_GT:
        opname = ">";
        break;
      case TokenKind::TK_LE:
        opname = "<=";
        break;
      case TokenKind::TK_GE:
        opname = ">=";
        break;
      case TokenKind::TK_ANDAND:
        opname = "&&";
        break;
      case TokenKind::TK_OROR:
        opname = "||";
        break;
      case TokenKind::TK_NULLISHCOALESCE:
        opname = "??";
        break;
      case TokenKind::TK_AMPERSAND:
        opname = "&";
        break;
      case TokenKind::TK_PIPE:
        opname = "|";
        break;
      case TokenKind::TK_CARET:
        opname = "^";
        break;
      case TokenKind::TK_LSHIFT:
        opname = "<<";
        break;
      case TokenKind::TK_RSHIFT:
        opname = ">>";
        break;
      default:
        opname = token_desc(op);
        break;
      }

      auto be = std::make_unique<nari::BinaryExpr>(opname, std::move(lhs),
                                                   std::move(rhs));
      // attach source position from operator token for better diagnostics
      be->line = op.line;
      be->col = op.col;
      // Attach filename (prefer token filename if present, otherwise current
      // parser filename)
      be->filename = op.filename.empty() ? g_current_filename : op.filename;
      lhs = std::move(be);
    }

    return lhs;
  }

  // parse unary operators and primaries
  ExprPtr parse_unary() {
    const Token &t = peek();
    if (t.kind == TokenKind::TK_BANG || t.kind == TokenKind::TK_MINUS ||
        t.kind == TokenKind::TK_PLUS || t.kind == TokenKind::TK_TILDE) {
      Token op = t;
      next();
      ExprPtr operand = parse_unary();
      if (!operand)
        error_and_exit("Expected operand for unary operator");
      std::string opname;
      if (op.kind == TokenKind::TK_BANG)
        opname = "!";
      else if (op.kind == TokenKind::TK_MINUS)
        opname = "neg";
      else if (op.kind == TokenKind::TK_TILDE)
        opname = "~";
      else
        opname = "+";
      auto ue = std::make_unique<nari::UnaryExpr>(opname, std::move(operand));
      // attach source position from operator token
      ue->line = op.line;
      ue->col = op.col;
      ue->filename = op.filename.empty() ? g_current_filename : op.filename;
      return ue;
    }
    // prefix ++ and --
    if (t.kind == TokenKind::TK_PLUSPLUS ||
        t.kind == TokenKind::TK_MINUSMINUS) {
      Token op = t;
      next();
      ExprPtr operand = parse_unary();
      if (!operand)
        error_and_exit("Expected operand for ++ or --");
      std::string opname = (op.kind == TokenKind::TK_PLUSPLUS) ? "++" : "--";
      auto ue = std::make_unique<nari::UnaryExpr>(opname, std::move(operand));
      ue->line = op.line;
      ue->col = op.col;
      ue->filename = op.filename.empty() ? g_current_filename : op.filename;
      return ue;
    }
    return parse_postfix();
  }

  // parse postfix operators (like ++ and -- after an expression, and [] and .
  // access)
  ExprPtr parse_postfix() {
    ExprPtr expr = parse_primary();
    while (true) {
      const Token &tok = peek();
      if (tok.kind == TokenKind::TK_PLUSPLUS ||
          tok.kind == TokenKind::TK_MINUSMINUS) {
        // only treat as postfix if on the same line as the expression
        // this prevents `x\n++y` from being parsed as `(x++)\ny`
        if (expr && expr->line != 0 && tok.line != expr->line) {
          break;
        }
        Token op = tok;
        next();
        std::string opname =
            (op.kind == TokenKind::TK_PLUSPLUS) ? "post++" : "post--";
        auto ue = std::make_unique<nari::UnaryExpr>(opname, std::move(expr));
        ue->line = op.line;
        ue->col = op.col;
        ue->filename = op.filename.empty() ? g_current_filename : op.filename;
        expr = std::move(ue);
      } else if (tok.kind == TokenKind::TK_LPAREN) {
        // Function call: expr(args...)
        Token op = tok;
        next(); // consume '('
        auto call = std::make_unique<nari::CallExpr>(std::move(expr));
        call->line = op.line;
        call->col = op.col;
        call->filename = op.filename.empty() ? g_current_filename : op.filename;

        if (peek().kind != TokenKind::TK_RPAREN) {
          while (true) {
            call->args.push_back(parse_expression());
            if (peek().kind == TokenKind::TK_COMMA) {
              next();
              continue;
            }
            break;
          }
        }
        expect(TokenKind::TK_RPAREN, "call end )");
        expr = std::move(call);
      } else if (tok.kind == TokenKind::TK_LBRACKET) {
        // Index access: expr[index]
        Token op = tok;
        next(); // consume '['
        ExprPtr index = parse_expression();
        expect(TokenKind::TK_RBRACKET, "index access end ]");
        auto ie = std::make_unique<nari::IndexExpr>(std::move(expr),
                                                    std::move(index));
        ie->line = op.line;
        ie->col = op.col;
        ie->filename = op.filename.empty() ? g_current_filename : op.filename;
        expr = std::move(ie);
      } else if (tok.kind == TokenKind::TK_DOT) {
        // Member access: expr.member
        Token op = tok;
        next(); // consume '.'
        Token memberTok = peek();
        expect(TokenKind::TK_IDENT, "member name after .");
        auto me =
            std::make_unique<nari::MemberExpr>(std::move(expr), memberTok.text);
        me->line = op.line;
        me->col = op.col;
        me->filename = op.filename.empty() ? g_current_filename : op.filename;
        expr = std::move(me);
      } else {
        break;
      }
    }
    return expr;
  }

  // parse a match expression pattern
  nari::PatternPtr parse_pattern() {
    const Token &tok = peek();

    // wildcard pattern: _
    if (tok.kind == TokenKind::TK_IDENT && tok.text == "_") {
      next();
      auto pattern = std::make_unique<nari::WildcardPattern>();
      pattern->line = tok.line;
      pattern->col = tok.col;
      pattern->filename =
          tok.filename.empty() ? g_current_filename : tok.filename;
      return pattern;
    }

    // literal patterns: numbers, strings, booleans, null
    if (tok.kind == TokenKind::TK_NUMBER || tok.kind == TokenKind::TK_STRING) {
      auto expr = parse_primary(); // reuse literal expression parsing
      auto pattern = std::make_unique<nari::LiteralPattern>(std::move(expr));
      pattern->line = tok.line;
      pattern->col = tok.col;
      pattern->filename =
          tok.filename.empty() ? g_current_filename : tok.filename;
      return pattern;
    }

    if (tok.kind == TokenKind::TK_IDENT) {
      // true, false, null literals
      if (tok.text == "true" || tok.text == "false" || tok.text == "null") {
        auto expr = parse_primary();
        auto pattern = std::make_unique<nari::LiteralPattern>(std::move(expr));
        pattern->line = tok.line;
        pattern->col = tok.col;
        pattern->filename =
            tok.filename.empty() ? g_current_filename : tok.filename;
        return pattern;
      }

      // variant pattern or binding
      std::string name = next().text;

      // check for something like Ok(value)
      if (peek().kind == TokenKind::TK_LPAREN) {
        next(); // consume '('
        auto pattern = std::make_unique<nari::VariantPattern>(name);
        pattern->line = tok.line;
        pattern->col = tok.col;
        pattern->filename =
            tok.filename.empty() ? g_current_filename : tok.filename;

        // Parse nested patterns
        while (!is_eof() && peek().kind != TokenKind::TK_RPAREN) {
          pattern->fields.push_back(parse_pattern());

          if (peek().kind == TokenKind::TK_COMMA) {
            next(); // consume ','
          } else if (peek().kind != TokenKind::TK_RPAREN) {
            error_and_exit("Expected ',' or ')' in variant pattern");
          }
        }
        expect(TokenKind::TK_RPAREN, "')' after variant fields");
        return pattern;
      }

      // simple binding pattern: x, value, etc.
      auto pattern = std::make_unique<nari::BindingPattern>(name);
      pattern->line = tok.line;
      pattern->col = tok.col;
      pattern->filename =
          tok.filename.empty() ? g_current_filename : tok.filename;
      return pattern;
    }

    error_and_exit("Expected pattern");
    return nullptr;
  }

  // parse match expression itself: match value { pattern => expr, ... }
  ExprPtr parse_match_expression() {
    const Token &matchTok = peek();
    next(); // consume 'match'

    // parse the value being matched
    auto scrutinee = parse_expression();

    auto match_expr = std::make_unique<nari::MatchExpr>(std::move(scrutinee));
    match_expr->line = matchTok.line;
    match_expr->col = matchTok.col;
    match_expr->filename =
        matchTok.filename.empty() ? g_current_filename : matchTok.filename;

    expect(TokenKind::TK_LBRACE, "'{' after match scrutinee");

    while (!is_eof() && peek().kind != TokenKind::TK_RBRACE) {
      auto pattern = parse_pattern();

      // expect '=>' (either as FATARROW or as '=' followed by '>')
      if (peek().kind == TokenKind::TK_FATARROW) {
        next(); // consume '=>'
      } else if (peek().kind == TokenKind::TK_EQUAL &&
          peek(1).kind == TokenKind::TK_GT) {
        next(); // consume '='
        next(); // consume '>'
      } else {
        error_and_exit("Expected '=>' after pattern in match arm");
      }

      auto body_expr = parse_expression();

      match_expr->arms.emplace_back(std::move(pattern), std::move(body_expr));

      // optional comma after arm
      if (peek().kind == TokenKind::TK_COMMA) {
        next();
      }
    }

    expect(TokenKind::TK_RBRACE, "'}' after match arms");

    if (match_expr->arms.empty()) {
      error_and_exit("Match expression must have at least one arm");
    }

    return match_expr;
  }

  ExprPtr parse_primary() {
    const Token &tok = peek();
    if (tok.kind == TokenKind::TK_IDENT) {
      if (tok.text == "func") {
        return parse_function_expression();
      }
      if (tok.text == "spawn") {
        return parse_spawn_expression();
      }
      if (tok.text == "match") {
        return parse_match_expression();
      }
      if (tok.text == "this") {
        auto t = std::make_unique<nari::ThisExpr>();
        t->line = tok.line;
        t->col = tok.col;
        t->filename = tok.filename.empty() ? g_current_filename : tok.filename;
        next();
        return t;
      }
      if (tok.text == "new") {
        next(); // consume 'new'
        if (peek().kind != TokenKind::TK_IDENT) {
          error_and_exit("Expected class name after 'new'");
        }
        std::string class_name = next().text;
        auto new_expr = std::make_unique<nari::NewExpr>(class_name);
        new_expr->line = tok.line;
        new_expr->col = tok.col;
        new_expr->filename =
            tok.filename.empty() ? g_current_filename : tok.filename;

        // parse constructor arguments
        expect(TokenKind::TK_LPAREN, "'(' for constructor arguments");
        if (peek().kind != TokenKind::TK_RPAREN) {
          while (true) {
            new_expr->args.push_back(parse_expression());
            if (peek().kind == TokenKind::TK_COMMA) {
              next();
            } else if (peek().kind == TokenKind::TK_RPAREN) {
              break;
            } else {
              error_and_exit("Expected ',' or ')' in constructor arguments");
            }
          }
        }
        expect(TokenKind::TK_RPAREN, "')' after constructor arguments");
        return new_expr;
      }
      if (tok.text == "true") {
        auto b = std::make_unique<nari::BoolExpr>(true);
        b->line = tok.line;
        b->col = tok.col;
        next();
        return b;
      }
      if (tok.text == "false") {
        auto b = std::make_unique<nari::BoolExpr>(false);
        b->line = tok.line;
        b->col = tok.col;
        next();
        return b;
      }
      if (tok.text == "null") {
        auto n = std::make_unique<nari::NullExpr>();
        n->line = tok.line;
        n->col = tok.col;
        next();
        return n;
      }

      std::string ident = next().text;
      auto ie = std::make_unique<nari::IdentExpr>(ident);
      ie->line = tok.line;
      ie->col = tok.col;
      ie->filename = tok.filename.empty() ? g_current_filename : tok.filename;
      return ie;
    } else if (tok.kind == TokenKind::TK_STRING) {
      std::string str = tok.text;
      auto str_expr = std::make_unique<nari::StringExpr>(str);
      str_expr->line = tok.line;
      str_expr->col = tok.col;
      str_expr->filename =
          tok.filename.empty() ? g_current_filename : tok.filename;
      next();
      return str_expr;
    } else if (tok.kind == TokenKind::TK_INTERP_STRING) {
      // interpolated string: `Hello {name}!`
      auto interp = std::make_unique<nari::StringInterpolationExpr>();
      interp->line = tok.line;
      interp->col = tok.col;
      interp->filename =
          tok.filename.empty() ? g_current_filename : tok.filename;

      // copy directly
      interp->parts = tok.interp_parts;
      interp->expr_sources = tok.interp_exprs;

      next();
      return interp;
    } else if (tok.kind == TokenKind::TK_NUMBER) {
      std::string s = tok.text;
      bool is_float = (s.find('.') != std::string::npos) ||
                      (s.find('e') != std::string::npos) ||
                      (s.find('E') != std::string::npos);
      std::unique_ptr<nari::NumberExpr> num_expr;
      if (is_float) {
        double v = 0.0;
        char *endptr = nullptr;
        v = std::strtod(s.c_str(), &endptr);
        if (endptr == s.c_str())
          v = 0.0;
        num_expr = std::make_unique<nari::NumberExpr>(v);
      } else {
        errno = 0;
        char *endptr = nullptr;

        bool is_hex =
            s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
        int base = is_hex ? 16 : 10;

        int64_t v = std::strtoll(s.c_str(), &endptr, base);
        if (endptr == s.c_str() || errno == ERANGE) {
          if (is_hex) {
            errno = 0;
            endptr = nullptr;
            unsigned long long uv = std::strtoull(s.c_str(), &endptr, 16);
            if (endptr == s.c_str() || errno == ERANGE) {
              num_expr = std::make_unique<nari::NumberExpr>(0.0);
            } else if (uv <= static_cast<unsigned long long>(
                                 std::numeric_limits<int64_t>::max())) {
              num_expr =
                  std::make_unique<nari::NumberExpr>(static_cast<int64_t>(uv));
            } else {
              num_expr =
                  std::make_unique<nari::NumberExpr>(static_cast<double>(uv));
            }
          } else {
            double dv = std::strtod(s.c_str(), &endptr);
            if (endptr == s.c_str())
              dv = 0.0;
            num_expr = std::make_unique<nari::NumberExpr>(dv);
          }
        } else {
          num_expr = std::make_unique<nari::NumberExpr>(v);
        }
      }
      num_expr->line = tok.line;
      num_expr->col = tok.col;
      num_expr->filename =
          tok.filename.empty() ? g_current_filename : tok.filename;
      next();
      return num_expr;
    } else if (tok.kind == TokenKind::TK_LPAREN) {
      // Check if this is an arrow function: (params) => ...
      if (looks_like_arrow_function()) {
        return parse_arrow_function_expression();
      }
      // Otherwise, it's a parenthesized expression
      next(); // consume '('
      ExprPtr e = parse_expression();
      expect(TokenKind::TK_RPAREN, "closing )");
      return e;
    } else if (tok.kind == TokenKind::TK_LBRACKET) {
      // array literal: [elem1, elem2, ...]
      Token start = tok;
      next(); // consume '['
      auto arr = std::make_unique<nari::ArrayLiteralExpr>();
      arr->line = start.line;
      arr->col = start.col;
      arr->filename =
          start.filename.empty() ? g_current_filename : start.filename;

      if (peek().kind != TokenKind::TK_RBRACKET) {
        while (true) {
          arr->elements.push_back(parse_expression());
          if (peek().kind == TokenKind::TK_COMMA) {
            next();

            if (peek().kind == TokenKind::TK_RBRACKET)
              break;
            continue;
          }
          break;
        }
      }
      expect(TokenKind::TK_RBRACKET, "array literal end ]");
      return arr;
    } else if (tok.kind == TokenKind::TK_LBRACE) {
      // object literal: {key1: val1, key2: val2, ...}
      Token start = tok;
      next(); // consume '{'
      auto obj = std::make_unique<nari::ObjectLiteralExpr>();
      obj->line = start.line;
      obj->col = start.col;
      obj->filename =
          start.filename.empty() ? g_current_filename : start.filename;

      if (peek().kind != TokenKind::TK_RBRACE) {
        while (true) {
          // k:v
          Token keyTok = peek();
          std::string key;
          if (keyTok.kind == TokenKind::TK_IDENT) {
            key = keyTok.text;
            next();
          } else if (keyTok.kind == TokenKind::TK_STRING) {
            key = keyTok.text;
            next();
          } else {
            error_and_exit("Expected identifier or string as object key");
          }

          expect(TokenKind::TK_COLON, "object literal :");

          ExprPtr value;
          if (peek().kind == TokenKind::TK_IDENT && peek().text == "func") {
            // func(params) { ... }
            value = parse_function_expression();
          } else if (peek().kind == TokenKind::TK_LPAREN &&
                     looks_like_arrow_function()) {
            // (params) => { ... } arrow function expression
            value = parse_arrow_function_expression();
          } else {
            // Regular expression
            value = parse_expression();
          }
          obj->entries.push_back({key, std::move(value)});

          if (peek().kind == TokenKind::TK_COMMA) {
            next();

            if (peek().kind == TokenKind::TK_RBRACE)
              break;
            continue;
          }
          break;
        }
      }
      expect(TokenKind::TK_RBRACE, "object literal end }");
      return obj;
    } else {
      error_and_exit("Unexpected primary token '" + token_desc(tok) + "'");
      return nullptr;
    }
  }
};

std::vector<FunctionPtr> parse_program_from_source(const std::string &src,
                                                   bool create_aggregator) {
  std::vector<Token> tokens_out;
  auto push_tok = [&](TokenKind k, std::string txt, int line, int col) {
    Token tk;
    tk.kind = k;
    tk.text = std::move(txt);
    tk.line = line;
    tk.col = col;
    tk.filename = g_current_filename;
    tokens_out.push_back(std::move(tk));
  };

  size_t pos = 0;
  int line = 1, col = 1;
  auto cur = [&]() -> char { return pos < src.size() ? src[pos] : '\0'; };
  auto peek = [&](size_t n = 1) -> char {
    size_t p = pos + n;
    return p < src.size() ? src[p] : '\0';
  };
  auto advance = [&](size_t n = 1) {
    for (size_t i = 0; i < n; ++i) {
      if (pos >= src.size())
        return;
      if (src[pos] == '\n') {
        ++line;
        col = 1;
        ++pos;
      } else {
        ++pos;
        ++col;
      }
    }
  };

  while (cur()) {
    char c = cur();
    // whitespace and comments
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      advance();
      continue;
    }
    if (c == '/' && peek() == '/') {
      // line comment
      advance(2);
      while (cur() && cur() != '\n')
        advance();
      continue;
    }
    if (c == '/' && peek() == '*') {
      advance(2);
      while (cur()) {
        if (cur() == '*' && peek() == '/') {
          advance(2);
          break;
        }
        advance();
      }
      continue;
    }

    // multi-char operators
    if (c == '&' && peek() == '&') {
      push_tok(TokenKind::TK_ANDAND, "&&", line, col);
      advance(2);
      continue;
    }
    if (c == '&' && peek() == '=') {
      push_tok(TokenKind::TK_AMPEQ, "&=", line, col);
      advance(2);
      continue;
    }
    if (c == '|' && peek() == '|') {
      push_tok(TokenKind::TK_OROR, "||", line, col);
      advance(2);
      continue;
    }
    if (c == '|' && peek() == '=') {
      push_tok(TokenKind::TK_PIPEEQ, "|=", line, col);
      advance(2);
      continue;
    }
    if (c == '^' && peek() == '=') {
      push_tok(TokenKind::TK_CARETEQ, "^=", line, col);
      advance(2);
      continue;
    }
    if (c == '=' && peek() == '=') {
      push_tok(TokenKind::TK_EQEQ, "==", line, col);
      advance(2);
      continue;
    }
    if (c == '=' && peek() == '>') {
      push_tok(TokenKind::TK_FATARROW, "=>", line, col);
      advance(2);
      continue;
    }
    if (c == '!' && peek() == '=') {
      push_tok(TokenKind::TK_NEQ, "!=", line, col);
      advance(2);
      continue;
    }
    if (c == '<' && peek() == '<' && peek(2) == '=') {
      push_tok(TokenKind::TK_LSHIFTEQ, "<<=", line, col);
      advance(3);
      continue;
    }
    if (c == '<' && peek() == '<') {
      push_tok(TokenKind::TK_LSHIFT, "<<", line, col);
      advance(2);
      continue;
    }
    if (c == '<' && peek() == '=') {
      push_tok(TokenKind::TK_LE, "<=", line, col);
      advance(2);
      continue;
    }
    if (c == '>' && peek() == '>' && peek(2) == '=') {
      push_tok(TokenKind::TK_RSHIFTEQ, ">>=", line, col);
      advance(3);
      continue;
    }
    if (c == '>' && peek() == '>') {
      push_tok(TokenKind::TK_RSHIFT, ">>", line, col);
      advance(2);
      continue;
    }
    if (c == '>' && peek() == '=') {
      push_tok(TokenKind::TK_GE, ">=", line, col);
      advance(2);
      continue;
    }
    if (c == '+' && peek() == '+') {
      push_tok(TokenKind::TK_PLUSPLUS, "++", line, col);
      advance(2);
      continue;
    }
    if (c == '-' && peek() == '-') {
      push_tok(TokenKind::TK_MINUSMINUS, "--", line, col);
      advance(2);
      continue;
    }
    if (c == '-' && peek() == '>') {
      push_tok(TokenKind::TK_ARROW, "->", line, col);
      advance(2);
      continue;
    }
    if (c == '+' && peek() == '=') {
      push_tok(TokenKind::TK_PLUSEQ, "+=", line, col);
      advance(2);
      continue;
    }
    if (c == '-' && peek() == '=') {
      push_tok(TokenKind::TK_MINUSEQ, "-=", line, col);
      advance(2);
      continue;
    }
    if (c == '*' && peek() == '*') {
      push_tok(TokenKind::TK_EXPONENT, "**", line, col);
      advance(2);
      continue;
    }
    if (c == '*' && peek() == '=') {
      push_tok(TokenKind::TK_STAREQ, "*=", line, col);
      advance(2);
      continue;
    }
    if (c == '/' && peek() == '=') {
      push_tok(TokenKind::TK_SLASHEQ, "/=", line, col);
      advance(2);
      continue;
    }
    if (c == '%' && peek() == '=') {
      push_tok(TokenKind::TK_PERCENTEQ, "%=", line, col);
      advance(2);
      continue;
    }
    if (c == '?' && peek() == '?') {
      push_tok(TokenKind::TK_NULLISHCOALESCE, "??", line, col);
      advance(2);
      continue;
    }
    if (c == '.' && peek() == '.' && peek(2) == '.') {
      push_tok(TokenKind::TK_ELLIPSIS, "...", line, col);
      advance(3);
      continue;
    }

    // single-char tokens
    if (c == '(') {
      push_tok(TokenKind::TK_LPAREN, "(", line, col);
      advance();
      continue;
    }
    if (c == ')') {
      push_tok(TokenKind::TK_RPAREN, ")", line, col);
      advance();
      continue;
    }
    if (c == '{') {
      push_tok(TokenKind::TK_LBRACE, "{", line, col);
      advance();
      continue;
    }
    if (c == '}') {
      push_tok(TokenKind::TK_RBRACE, "}", line, col);
      advance();
      continue;
    }
    if (c == '[') {
      push_tok(TokenKind::TK_LBRACKET, "[", line, col);
      advance();
      continue;
    }
    if (c == ']') {
      push_tok(TokenKind::TK_RBRACKET, "]", line, col);
      advance();
      continue;
    }
    if (c == ',') {
      push_tok(TokenKind::TK_COMMA, ",", line, col);
      advance();
      continue;
    }
    if (c == ';') {
      push_tok(TokenKind::TK_SEMICOLON, ";", line, col);
      advance();
      continue;
    }
    if (c == ':') {
      push_tok(TokenKind::TK_COLON, ":", line, col);
      advance();
      continue;
    }
    if (c == '.') {
      push_tok(TokenKind::TK_DOT, ".", line, col);
      advance();
      continue;
    }
    if (c == '?') {
      push_tok(TokenKind::TK_QUESTION, "?", line, col);
      advance();
      continue;
    }
    if (c == '@') {
      push_tok(TokenKind::TK_AT, "@", line, col);
      advance();
      continue;
    }
    if (c == '=') {
      push_tok(TokenKind::TK_EQUAL, "=", line, col);
      advance();
      continue;
    }
    if (c == '+') {
      push_tok(TokenKind::TK_PLUS, "+", line, col);
      advance();
      continue;
    }
    if (c == '-') {
      push_tok(TokenKind::TK_MINUS, "-", line, col);
      advance();
      continue;
    }
    if (c == '*') {
      push_tok(TokenKind::TK_STAR, "*", line, col);
      advance();
      continue;
    }
    if (c == '/') {
      push_tok(TokenKind::TK_SLASH, "/", line, col);
      advance();
      continue;
    }
    if (c == '!') {
      push_tok(TokenKind::TK_BANG, "!", line, col);
      advance();
      continue;
    }
    if (c == '<') {
      push_tok(TokenKind::TK_LT, "<", line, col);
      advance();
      continue;
    }
    if (c == '>') {
      push_tok(TokenKind::TK_GT, ">", line, col);
      advance();
      continue;
    }
    if (c == '%') {
      push_tok(TokenKind::TK_PERCENT, "%", line, col);
      advance();
      continue;
    }
    if (c == '&') {
      push_tok(TokenKind::TK_AMPERSAND, "&", line, col);
      advance();
      continue;
    }
    if (c == '|') {
      push_tok(TokenKind::TK_PIPE, "|", line, col);
      advance();
      continue;
    }
    if (c == '^') {
      push_tok(TokenKind::TK_CARET, "^", line, col);
      advance();
      continue;
    }
    if (c == '~') {
      push_tok(TokenKind::TK_TILDE, "~", line, col);
      advance();
      continue;
    }

    // string literal (supports escapes: \n, \t, \", \\, others map to the
    // escaped char)
    if (c == '\"') {
      int start_line = line;
      int start_col = col;
      advance();
      std::string out;
      while (cur() && cur() != '\"') {
        if (cur() == '\\') {
          advance();
          char e = cur();
          if (!e)
            break;
          if (e == 'n')
            out.push_back('\n');
          else if (e == 'r')
            out.push_back('\r');
          else if (e == 't')
            out.push_back('\t');
          else if (e == '\\')
            out.push_back('\\');
          else if (e == '\"')
            out.push_back('\"');
          else
            out.push_back(e);
          advance();
        } else {
          out.push_back(cur());
          advance();
        }
      }
      if (cur() == '\"') {
        advance();
      } else {
        // uh oh.
        Token tok;
        tok.filename = g_current_filename;
        tok.line = start_line;
        tok.col = start_col;
        fatal_error(std::string("Unterminated string literal"), &tok);
      }
      push_tok(TokenKind::TK_STRING, out, start_line, start_col);
      continue;
    }

    // interpolated string literal with backticks: `Hello {name}!`
    if (c == '`') {
      int start_line = line;
      int start_col = col;
      advance();

      Token tok;
      tok.kind = TokenKind::TK_INTERP_STRING;
      tok.line = start_line;
      tok.col = start_col;
      tok.filename = g_current_filename;

      std::string current_part;

      while (cur() && cur() != '`') {
        if (cur() == '\\') {
          // Handle escape sequences
          advance();
          char e = cur();
          if (!e)
            break;
          if (e == 'n')
            current_part.push_back('\n');
          else if (e == 'r')
            current_part.push_back('\r');
          else if (e == 't')
            current_part.push_back('\t');
          else if (e == '\\')
            current_part.push_back('\\');
          else if (e == '`')
            current_part.push_back('`');
          else if (e == '{')
            current_part.push_back('{');
          else
            current_part.push_back(e);
          advance();
        } else if (cur() == '{') {
          tok.interp_parts.push_back(current_part);
          current_part.clear();
          advance(); // '{'

          // extract until '}'
          std::string expr;
          int brace_depth = 1;
          while (cur() && brace_depth > 0) {
            if (cur() == '{')
              brace_depth++;
            else if (cur() == '}')
              brace_depth--;

            if (brace_depth > 0) {
              expr.push_back(cur());
            }
            advance();
          }

          tok.interp_exprs.push_back(expr);
        } else {
          current_part.push_back(cur());
          advance();
        }
      }

      // add the final part (after last expression or whole string if no
      // expressions)
      tok.interp_parts.push_back(current_part);

      if (cur() == '`') {
        advance();
      } else {
        // uh oh again
        fatal_error(std::string("Unterminated interpolated string literal"),
                    &tok);
      }

      tokens_out.push_back(tok);
      continue;
    }

    // numbers: integer, hex integer, or floating
    if ((c >= '0' && c <= '9') ||
        (c == '.' && (peek() >= '0' && peek() <= '9'))) {
      int start_line = line;
      int start_col = col;
      size_t st = pos;

      if (c == '0' && (peek() == 'x' || peek() == 'X')) {
        advance(); // 0
        advance(); // x/X

        size_t hex_start = pos;
        while (cur() && std::isxdigit(static_cast<unsigned char>(cur()))) {
          advance();
        }

        if (pos == hex_start) {
          Token tok;
          tok.filename = g_current_filename;
          tok.line = start_line;
          tok.col = start_col;
          fatal_error(std::string("Invalid hex literal: expected at least one "
                                  "hex digit after 0x"),
                      &tok);
        }

        std::string num = src.substr(st, pos - st);
        push_tok(TokenKind::TK_NUMBER, num, start_line, start_col);
        continue;
      }

      while (cur() && (cur() >= '0' && cur() <= '9'))
        advance();
      if (cur() == '.') {
        advance();
        while (cur() && (cur() >= '0' && cur() <= '9'))
          advance();
      }
      std::string num = src.substr(st, pos - st);
      push_tok(TokenKind::TK_NUMBER, num, start_line, start_col);
      continue;
    }

    // letters, digits, underscore
    if (isalpha(c) || c == '_') {
      int start_line = line;
      int start_col = col;
      size_t st = pos;
      advance();
      while (cur() && (isalnum(cur()) || cur() == '_'))
        advance();
      std::string id = src.substr(st, pos - st);
      push_tok(TokenKind::TK_IDENT, id, start_line, start_col);
      continue;
    }

    // unknown single char
    std::string tmp(1, c);
    push_tok(TokenKind::TK_UNKNOWN, tmp, line, col);
    advance();
  }

  // EOF
  push_tok(TokenKind::TK_EOF, "", line, col);

  // invoke parser with the specified aggregator creation flag
  Parser parser(tokens_out);
  auto funcs = parser.parse_program(create_aggregator);
  return funcs;
}

} // namespace Parser
