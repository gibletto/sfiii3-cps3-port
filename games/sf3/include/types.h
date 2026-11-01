#ifndef TYPES_H
#define TYPES_H

#ifdef HOST_PORT
/* The host's own C library defines the fixed-width types (newlib on ARM makes int32_t a long), and the port's
   files include both; the game's types are taken from it so they agree everywhere. */
#include <stddef.h>
#include <stdint.h>
typedef int8_t s8;
typedef uint8_t u8;
typedef int16_t s16;
typedef uint16_t u16;
typedef int32_t s32;
typedef uint32_t u32;
#else
typedef signed char s8;
typedef unsigned char u8;
typedef short s16;
typedef unsigned short u16;
typedef int s32;
typedef unsigned int u32;
typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef int ssize_t;
typedef unsigned int size_t;
typedef int intptr_t;
#endif

#endif
