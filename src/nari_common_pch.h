#pragma once
// Precompiled header for the nari_common static library (see meson.build).
// Same rules as nari_pch.h, minus the JIT stack (no JIT TUs link the lib).
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast.h"
#include "bytecode/bytecode.h"
#include "core_types.h"
#include "parser_api.h"
