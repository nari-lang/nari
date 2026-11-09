/*
  parser.cpp
  Expression-precedence parser (precedence-climbing) that builds the AST for
  expressions.
  read: https://en.wikipedia.org/wiki/Operator-precedence_parser#Precedence_climbing_method
*/

#include "ast.h"
#include "int_overflow.h"
#include "nari_fs.h"
#include "parser/lexer.h"
#include "parser/module_resolver.h"
#include "parser/parser_internal.h"
#include "parser/token.h"
#include "parser_api.h"

#include <cstdio>
#include <ctype.h>
#include <errno.h>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include "win_funcs.h"
#endif

#ifdef __linux__
#include <elf.h>
#endif

using nari::BlockPtr;
using nari::ExprPtr;
using nari::FunctionPtr;
using nari::StmtPtr;
using nari::VarDeclCtrl;

const std::string &nari_std_module_source(const std::string &name);
#include "parser/parser.h"
namespace Parser {
static ParseResult parse(const std::string &src, bool create_aggregator, bool recover_mode) {
    std::vector<LexError> lex_errors;
    std::vector<Token> tokens_out = tokenize(src, current_filename, recover_mode ? &lex_errors : nullptr);
    if (!lex_errors.empty()) {
        std::vector<ParseError> errors;
        errors.reserve(lex_errors.size());
        for (auto &error : lex_errors) {
            errors.push_back({ std::move(error.filename), error.line, error.col, std::move(error.message) });
        }
        return ParseResult{ {}, std::move(errors) };
    }

    // invoke parser with the specified aggregator creation flag
    Parser parser(tokens_out, recover_mode);
    auto funcs = parser.parse_program(create_aggregator);
    return ParseResult{ std::move(funcs), parser.take_errors() };
}

std::vector<FunctionPtr> parse_program_from_source(const std::string &src, bool create_aggregator) {
    // standard mode: parser calls exit(1) on error, so errors is always empty.
    auto result = parse(src, create_aggregator, false);
    resolve_interp_exprs(result.functions);
    return std::move(result.functions);
}

ParseResult parse_program_recovering(const std::string &src, bool create_aggregator) {
    auto result = parse(src, create_aggregator, true);
    resolve_interp_exprs(result.functions, &result.errors);
    return result;
}

std::string resolve_import_path(const std::string &inc, const std::string &basefile) {
    return resolve_include_path(inc, basefile);
}

} // namespace Parser
