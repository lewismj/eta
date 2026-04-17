#pragma once

#ifdef _MSC_VER
    #define cache_align __declspec(align(64))
#else
    #define cache_align __attribute__((aligned(64)))
#endif

/* Inline force/hints. */
#if defined(__clang__) && defined(_MSC_VER)
    /* clang-cl: prefer clang-style always_inline even when _MSC_VER is defined */
    #define inline_always inline __attribute__((always_inline))
#elif defined(__linux__) || defined(__unix__) || defined(__APPLE__)
    #define inline_always inline __attribute__((always_inline))
#elif defined(_MSC_VER)
    #define inline_always __forceinline
#else
    #define inline_always inline
#endif

/* Clang allows us to specify inline always on recursive functions, GCC does not. */
#ifdef __clang__
    #define inline_hint inline __attribute__((always_inline))
#else
    #define inline_hint inline
#endif

/* Macros for pre-fetch. */
#if defined(__GNUC__) || defined(__clang__)
#define PREFETCH_DEFAULT(xs) __builtin_prefetch((xs), 0, 0)
#define PREFETCH_SOON(xs) __builtin_prefetch((xs), 0, 1)
#else
#define PREFETCH_DEFAULT(xs) (void)(xs)
#define PREFETCH_SOON(xs) (void)(xs)
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define _likely(x)   __builtin_expect(!!(x), 1)
#  define _unlikely(x) __builtin_expect(!!(x), 0)
#else
#  define _likely(x)   (x)
#  define _unlikely(x) (x)
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #include <emmintrin.h>  ///< SSE2
    #include <xmmintrin.h>  ///< SSE
    #if defined(__SSE4_2__)
        #include <nmmintrin.h>  ///< SSE4.2
    #endif
#endif
