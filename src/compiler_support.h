// thank you for this zero318! :D

// Check for claimed MSVC compatibility
#ifdef _MSC_VER
#define MSVC_COMPAT 1
#endif
// Check for claimed GCC compatibility
#ifdef __GNUC__
#define GCC_COMPAT 1
#endif
// Check for clang specifically
#ifdef __clang__
#define CLANG_COMPAT 1
#endif

#if defined(CLANG_COMPAT) || defined(GCC_COMPAT)
#define CLANG_OR_GCC_COMPAT 1
#endif

// Use to prevent clang using MSVC workarounds just because it claims to be compatible
#if MSVC_COMPAT && !GCC_COMPAT && !CLANG_COMPAT
#define COMPILER_IS_REAL_MSVC 1
#else
#define COMPILER_IS_REAL_MSVC 0
#endif

#if COMPILER_IS_REAL_MSVC
#define NARI_ALWAYS_INLINE __forceinline
#elif defined(CLANG_OR_GCC_COMPAT)
#define NARI_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define NARI_ALWAYS_INLINE inline
#endif

#ifndef __has_builtin
#define __has_builtin(name) 0
#endif

#if __has_builtin(__builtin_expect)
#define NARI_EXPECT __builtin_expect
#else
#define NARI_EXPECT(cond, ...) (cond)
#endif

#if __has_builtin(__builtin_unreachable)
#define unreachable __builtin_unreachable
#else
#define unreachable()
#endif

#define NARI_LIKELY(x) NARI_EXPECT(!!(x), 1)
#define NARI_UNLIKELY(x) NARI_EXPECT(!!(x), 0)
