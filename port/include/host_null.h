/*
 * host_null.h  Reads through null pointers, as the board does them
 *
 * The game sometimes reads through null pointers. On the board, address 0 is the BIOS, so these
 * reads return BIOS bytes (mostly 0) and the game relies on that.
 *
 *     NULL_READ(p, field)     p->field, or the big-endian BIOS bytes at offsetof(field) when p is 0
 */

#ifndef HOST_NULL_H
#define HOST_NULL_H

#include <stddef.h>
#include "types.h"

u32 cps3_low_read_be(u32 addr, u32 size, const char* site);

#define HOST_NULL_STR2(x) #x
#define HOST_NULL_STR(x) HOST_NULL_STR2(x)
#define NULL_READ(p, field)                                                                                     \
    ((p) ? (p)->field                                                                                           \
         : (__typeof__((p)->field))cps3_low_read_be(offsetof(__typeof__(*(p)), field), sizeof((p)->field),       \
                                                    __FILE_NAME__ ":" HOST_NULL_STR(__LINE__) " " #p "->" #field))

#endif
