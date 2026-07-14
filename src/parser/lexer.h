#pragma once

#include "token.h"

#include <string>
#include <vector>

namespace Parser {

struct LexError {
    std::string filename;
    int line = 0;
    int col = 0;
    std::string message;
};

// tokenize `src` into a token stream terminated by a TK_EOF token. `filename` is added onto each token for diagnostics
// when `errors` is provided, lexical errors are collected instead of terminating the process.
std::vector<Token> tokenize(const std::string &src, const std::string &filename, std::vector<LexError> *errors = nullptr);

// Print a fatal parse/lex diagnostic (with source context + include trace) and exit(1)
void fatal_error(const std::string &msg, const Token *tok = nullptr);

} // namespace Parser
