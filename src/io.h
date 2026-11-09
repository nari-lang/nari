#pragma once

// Threading selection for io_pool.h below: MCU and Emscripten targets have no
// pthreads, so the synchronous stub IOThreadPool is compiled instead.
#ifdef NARI_MCU
#ifndef NO_THREADS
#define NO_THREADS
#endif
#endif

#ifdef __EMSCRIPTEN__
#define NO_THREADS
#endif

#include "io_ops.h"

#include "io_pool.h"
