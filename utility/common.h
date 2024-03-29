#ifndef _COMMON_H
#define _COMMON_H

#include <cassert>
#include "types.h"

#define Q_countof(Array) (sizeof(Array) / sizeof(Array[0]))

// stringize macros
#define xstr(s) str(s)
#define str(s) #s


// Assert macros
#if !defined(NDEBUG) || NDEBUG == 0

#define ASSERT_GL_ERROR		assert(glGetError() == GL_NO_ERROR)

#else

#define ASSERT_GL_ERROR

#endif


// Alignment macros, use only with pow2 alignments
#define is_aligned(addr, bytes)     (((uintptr_t)(const void *)(addr)) % (bytes) == 0)

#define _align_mask(addr, mask)     (((uintptr_t)(addr)+(mask))&~(mask))
#define _align(addr, bytes)          _align_mask(addr,(bytes)-1)

// static assert macro for checking struct size to a base alignment when packed in an array
#define static_assert_aligned_size(Type,bytes)	static_assert(sizeof(Type) % (bytes) == 0,\
                                                              #Type " size is not a multiple of " xstr(bytes))

// size in bytes macros
#define kilobytes(v)    v*1024
#define megabytes(v)    kilobytes(v)*1024
#define gigabytes(v)    megabytes(v)*1024

#define bytesToMegabytes(v)     v/1048576

// export macros
#ifdef _MSC_VER
#define _export __declspec(dllexport)
#else
#define _export __attribute__ ((visibility ("default")))
#endif

// malloc macros
#if defined(_ALLOW_MALLOC) && _ALLOW_MALLOC != 0

#define Q_malloc(size)   malloc(size)

#else

#if defined(NDEBUG) && NDEBUG != 0
#define Q_malloc(size)   nullptr; assert(false && "malloc not allowed")
#else
// if assert is ignored, just crash
#define Q_malloc(size)   nullptr; *(int*)0 = 0
#endif

#endif

// min/max functions, should avoid using macros for this due to possible double-evaluation
inline u8  min(u8 a, u8 b)   { return (a < b ? a : b); }
inline i8  min(i8 a, i8 b)   { return (a < b ? a : b); }
inline u16 min(u16 a, u16 b) { return (a < b ? a : b); }
inline i16 min(i16 a, i16 b) { return (a < b ? a : b); }
inline u32 min(u32 a, u32 b) { return (a < b ? a : b); }
inline i32 min(i32 a, i32 b) { return (a < b ? a : b); }
inline u64 min(u64 a, u64 b) { return (a < b ? a : b); }
inline i64 min(i64 a, i64 b) { return (a < b ? a : b); }
inline r32 min(r32 a, r32 b) { return (a < b ? a : b); }
inline r64 min(r64 a, r64 b) { return (a < b ? a : b); }

inline u8  max(u8 a, u8 b)   { return (a > b ? a : b); }
inline i8  max(i8 a, i8 b)   { return (a > b ? a : b); }
inline u16 max(u16 a, u16 b) { return (a > b ? a : b); }
inline i16 max(i16 a, i16 b) { return (a > b ? a : b); }
inline u32 max(u32 a, u32 b) { return (a > b ? a : b); }
inline i32 max(i32 a, i32 b) { return (a > b ? a : b); }
inline u64 max(u64 a, u64 b) { return (a > b ? a : b); }
inline i64 max(i64 a, i64 b) { return (a > b ? a : b); }
inline r32 max(r32 a, r32 b) { return (a > b ? a : b); }
inline r64 max(r64 a, r64 b) { return (a > b ? a : b); }

// c std lib macros for msvc-only *_s functions
#ifdef _MSC_VER
    #define _strcpy_s(dest,destsz,src)					strcpy_s(dest,destsz,src)
    #define _strncpy_s(dest,destsz,src,count)			strncpy_s(dest,destsz,src,count)
    #define _strcat_s(dest,destsz,src)					strcat_s(dest,destsz,src)
	#define _vsnprintf_s(dest,destsz,count,fmt,valist)	vsnprintf_s(dest,destsz,count,fmt,valist)
#else
    #define _strcpy_s(dest,destsz,src)					strcpy(dest,src)
    #define _strncpy_s(dest,destsz,src,count)			strncpy(dest,src,min(destsz,count))
    #define _strcat_s(dest,destsz,src)					strcat(dest,src)
	#define _vsnprintf_s(dest,destsz,count,fmt,valist)	vsnprintf(dest,count,fmt,valist)
	#define _vscprintf(fmt,valist)						vsnprintf(nullptr,0,fmt,valist)
#endif

#endif