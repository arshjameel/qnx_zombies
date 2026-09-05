#ifndef COMMON_H
#define COMMON_H

#if !defined(__QNXNTO__)
#  define _POSIX_C_SOURCE 199309L
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef NDEBUG
#  define ASSERT(cond) ((void)(cond))
#else
#  include <stdlib.h>
#  define ASSERT(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "ASSERT failed: %s  (%s:%d)\n", \
                    #cond, __FILE__, __LINE__); \
            abort(); \
        } \
    } while (0)
#endif

#ifdef NDEBUG
#  define DLOG(fmt, ...) ((void)0)
#else
#  define DLOG(fmt, ...) \
    fprintf(stderr, "[DBG] %s:%d  " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#endif

#define ARRAY_LEN(a)    (sizeof(a) / sizeof((a)[0]))
#define CLAMP(v,lo,hi)  ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define MIN(a,b)        ((a) < (b) ? (a) : (b))
#define MAX(a,b)        ((a) > (b) ? (a) : (b))

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef float    f32;

#endif 