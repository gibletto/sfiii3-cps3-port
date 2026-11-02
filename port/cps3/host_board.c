/*
 * host_board.c  Reads past the end of a table, in the board's byte order (host_board.h)
 *
 * Each data file lists its runs of consecutive tables of 16- or 32-bit elements (port/tools/host_data.py).
 * A byte that lies inside one of them is at the other end of its element from where the board has it, so
 * it is read from there; any other byte is as on the board.
 */

#include <stdlib.h>
#include "host_board.h"

struct host_range {
    const void* first;      /* the run's first table */
    const void* last;       /* and its last, last_size bytes long */
    unsigned last_size, unit;
};
extern struct host_range* const host_data_ranges[];     /* one list per data file, each ending in a null first */

#if HOST_LITTLE_ENDIAN
typedef struct {
    const unsigned char *lo, *hi;    /* the bytes a data file's runs span */
} Span;
static Span* spans;
static int nlists;

static const unsigned char* run_end(const struct host_range* r) {
    return (const unsigned char*)r->last + r->last_size;
}

static void find_spans(void) {
    const struct host_range* r;
    int i, n;

    for (n = 0; host_data_ranges[n] != NULL; n++) {
    }
    spans = calloc(n > 0 ? n : 1, sizeof(Span));
    if (spans == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        for (r = host_data_ranges[i]; r->first != NULL; r++) {
            if (spans[i].lo == NULL || (const unsigned char*)r->first < spans[i].lo) {
                spans[i].lo = r->first;
            }
            if (run_end(r) > spans[i].hi) {
                spans[i].hi = run_end(r);
            }
        }
    }
    nlists = n;
}
#endif

static unsigned board_byte(const unsigned char* a) {
#if HOST_LITTLE_ENDIAN
    const struct host_range* r;
    int i;

    if (spans == NULL) {
        find_spans();
    }
    for (i = 0; i < nlists; i++) {
        if (a < spans[i].lo || a >= spans[i].hi) {
            continue;
        }
        for (r = host_data_ranges[i]; r->first != NULL; r++) {
            const unsigned char* p = r->first;
            if (a >= p && a < run_end(r)) {
                return p[(unsigned)(a - p) ^ (r->unit - 1)];     /* the tables are aligned to their elements */
            }
        }
    }
#endif
    return *a;
}

u32 host_board_read(const void* p, u32 size) {
    const unsigned char* a = p;
    u32 v = 0;

    while (size-- != 0) {
        v = (v << 8) | board_byte(a++);
    }
    return v;
}
