#pragma once

#include "token.h"

#include <string>
#include <vector>

namespace Parser {

// Tokenize `src` into a token stream terminated by a TK_EOF token. `filename` is stamped onto each token for diagnostics
std::vector<Token> tokenize(const std::string &src, const std::string &filename);

// Print a fatal parse/lex diagnostic (with source context + include trace) and exit(1)
void fatal_error(const std::string &msg, const Token *tok = nullptr);

} // namespace Parser
