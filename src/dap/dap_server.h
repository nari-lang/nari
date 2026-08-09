#pragma once

#include <string>
#include <vector>

namespace nari {
namespace dap {

// runs the Debug Adapter Protocol server on stdin/stdout.
// `script_path` is the user's .nari/.naric file from the `launch` request
//      args if already known, or empty if it should come in via launch.
// `argv` is the remaining argv the interpreter would have passed to the
//      program being debugged.
int run_dap_server(std::string initial_script, std::vector<std::string> argv);

} // namespace dap
} // namespace nari
