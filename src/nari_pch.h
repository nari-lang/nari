#pragma once
// Precompiled header for the nari executable (see meson.build cpp_pch).
// Covers the expensive, rarely-changing headers every TU parses: the libc++
// set plus the core type model, bytecode model and JIT stack. Keep this list
// tight: each entry is parsed once per configuration but loaded by every TU.
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

#include "bytecode/bytecode.h"
#include "core_types.h"
#include "jit/ir.h"
#include "jit/jit_arch.h"
#include "jit/jit_helpers.h"
