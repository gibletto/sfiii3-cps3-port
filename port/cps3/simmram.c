/*
 * simmram.c  SIMM RAM block allocator (simmram.src and simmram_10.src in C)
 *
 * *_link is a free list of s16 byte offsets (block n at 2n, offset 0 is the head).
 * *_owner holds the owner kind and run length per block. *_used counts blocks per kind.
 */

#include "structs.h"
#include "work.h"
#include "sys_config.h"

typedef struct {
    s16* owner;
    s16* link;
    s16* used;
    s32 end;        /* end mark: byte offset after the last block */
} SIMM_LIST;

s16 simm40_owner[3970];
s16 simm40_link[1986];
s16 simm40_used[64];
s16 simm10_owner[4098];
s16 simm10_link[2050];
s16 simm10_used[64];

static const SIMM_LIST list40 = { simm40_owner, simm40_link, simm40_used, 0xF82 };
static const SIMM_LIST list10 = { simm10_owner, simm10_link, simm10_used, 0x1002 };

#define LINK(l, ofs) ((l)->link[(ofs) >> 1])

static void freelist_init(const SIMM_LIST* l) {
    s32 ofs;
    s32 i;
    for (ofs = 0; ofs < l->end; ofs += 2) {
        LINK(l, ofs) = ofs + 2;
        l->owner[ofs] = 0;
        l->owner[ofs + 1] = 0;
    }
    for (i = 0; i < 64; i++) {
        l->used[i] = 0;
    }
}

/* Unlinks the run [start, last] after prev and returns its block number. */
static s32 take_run(const SIMM_LIST* l, s32 prev, s32 start, s32 last, s32 blocks, s32 kind) {
    LINK(l, prev) = LINK(l, last);
    l->owner[start] = kind;
    l->owner[start + 1] = blocks;
    l->used[kind] += blocks;
    return start >> 1;
}

/* First fit: the first free run of `blocks` contiguous blocks. */
static s32 block_alloc(const SIMM_LIST* l, s32 blocks, s32 kind) {
    s32 prev = 0;
    s32 start;
    s32 cur;
    s32 n;
    while (1) {
        start = LINK(l, prev);
        if (start == l->end) {
            return 0;
        }
        cur = start;
        n = blocks;
        while (1) {
            if (--n == 0) {
                return take_run(l, prev, start, cur, blocks, kind);
            }
            if (cur == l->end - 2) {
                return 0;
            }
            if (LINK(l, cur) - 2 != cur) {
                break;
            }
            cur += 2;
        }
        prev = cur;
    }
}

/* A free run starting at first, first + step, ... up to limit. */
static s32 page_alloc(const SIMM_LIST* l, s32 kind, s32 blocks, s32 first, s32 step, s32 limit) {
    s32 page = first;
    s32 prev = 0;
    s32 start;
    s32 cur;
    s32 n;
    while (1) {
        start = (u16)LINK(l, prev);
        if (start == l->end) {
            return 0;
        }
        if ((u32)start > (u32)page) {
            page += step;
            if ((u32)page > (u32)limit) {
                return 0;
            }
            continue;
        }
        if (start != page) {
            prev = start;
            continue;
        }
        cur = start;
        n = blocks;
        while (1) {
            if (--n == 0) {
                return take_run(l, prev, start, cur, blocks, kind);
            }
            if (cur == l->end - 2) {
                return 0;
            }
            if (LINK(l, cur) - 2 != cur) {
                break;
            }
            cur += 2;
        }
        prev = cur;
    }
}

static s32 block_free(const SIMM_LIST* l, s32 block) {
    s32 ofs = block << 1;
    s32 prev;
    s32 next = 0;
    u16 kind;
    u16 count;
    do {
        prev = next;
        next = (u16)LINK(l, prev);
    } while ((u32)ofs > (u32)next);
    LINK(l, prev) = ofs;
    kind = l->owner[ofs];
    count = l->owner[ofs + 1];
    l->owner[ofs] = 0;
    l->owner[ofs + 1] = 0;
    LINK(l, ofs + count * 2 - 2) = next;
    l->used[kind] -= count;
    return l->used[kind];
}

static s32 purge_by_owner(const SIMM_LIST* l, s32 kind) {
    s32 block = 0;
    s32 purged = 0;
    while (l->used[kind] != 0 && block < l->end / 2) {
        u16 owner = l->owner[block * 2];
        u16 count = l->owner[block * 2 + 1];
        if (count != 0 && owner == kind) {
            purged = 1;
            block_free(l, block);
        }
        block++;
    }
    return purged;
}

u32 simmram_freelist_init(void) {
    freelist_init(&list40);
    return 0;
}

s16 simmram_block_alloc_40(s32 blocks, s32 kind) {
    return block_alloc(&list40, blocks, kind);
}

u32 simmram_big_page_alloc_40(s32 kind) {
    return page_alloc(&list40, kind, 64, 0x42, 0x80, 0xEC2);
}

u32 simmram_small_page_alloc_40(s32 kind) {
    return page_alloc(&list40, kind, 16, 0x02, 0x20, 0xF62);
}

void simmram_block_free_40(s32 blk) {
    block_free(&list40, blk);
}

s32 simmram_purge_by_owner_40(s32 kind) {
    return purge_by_owner(&list40, kind);
}

u32 simmram_freelist_init_10(void) {
    freelist_init(&list10);
    return 0;
}

s32 simmram_block_alloc_10(s32 blocks, s32 kind) {
    return block_alloc(&list10, blocks, kind);
}

void simmram_block_free_10(s16 blk) {
    block_free(&list10, blk);
}

s32 simmram_purge_by_owner_10(s32 kind) {
    return purge_by_owner(&list10, kind);
}
