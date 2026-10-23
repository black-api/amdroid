#ifndef AMDROID_TYPES_H
#define AMDROID_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef size_t    usize;

#undef NULL
#define NULL ((void*)0)
#define UNUSED(x) ((void)(x))

#define ALIGN_UP(x,a) (((x)+((a)-1))&~((a)-1))
#define ALIGN_DOWN(x,a) ((x)&~((a)-1))

#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))

#endif
