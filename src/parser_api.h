#pragma once

#include "ast.h"
#include <string>
#include <vector>
#include <memory>

namespace Parser {

// sets the source filename used by the lexer/parser for diagnostics.
void set_source_filename(const std::string &filename);
void push_error_context(const std::string &ctx);
void pop_error_context();
std::vector<std::unique_ptr<nari::Function>> parse_program_from_source(const std::string &src, bool create_aggregator = true);

// type registry
bool is_registered_type(const std::string &name);
const nari::TypeDecl* get_registered_type(const std::string &name);
bool is_registered_enum(const std::string &name);
bool is_registered_class(const std::string &name);
const nari::ClassDecl* get_registered_class(const std::string &name);
void clear_type_registry();

} // namespace Parser
