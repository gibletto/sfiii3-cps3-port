/*
 * host_board.h  Reads past the end of a table, as the board does them
 *
 * Some of the game's reads index past the end of a table and run on into the tables after it. The
 * data is laid out as on the board (port/tools/host_data.py), so such a read lands on the same
 * bytes, but where it lands inside a table of 16- or 32-bit elements those bytes are in the host's
 * order. BOARD_READ reads them in the board's (big-endian) order:
 *
 *     BOARD_READ(tbl, i)      tbl[i], or what the board reads there when i is outside tbl
 *
 * tbl must be an array of known size.
 */

#ifndef HOST_BOARD_H
#define HOST_BOARD_H

#include "types.h"

u32 host_board_read(const void* p, u32 size);    /* the big-endian value of the size bytes at p */

#define BOARD_READ(tbl, i)                                                                                      \
    ((unsigned long)(i) < sizeof(tbl) / sizeof((tbl)[0])                                                        \
         ? (tbl)[i]                                                                                             \
         : (__typeof__((tbl)[0]))host_board_read((const char*)(tbl) + (long)(i) * (long)sizeof((tbl)[0]),      \
                                                 sizeof((tbl)[0])))

#endif
