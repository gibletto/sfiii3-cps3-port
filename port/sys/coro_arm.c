/*
 * coro_arm.c  Coroutines for 32-bit ARM (3DS)
 *
 * coro_swap pushes the callee-saved registers (r4-r11, lr, and d8-d15 with hardware FP) and swaps stacks.
 * A new stack is set up as if it had switched away at the start of coro_start.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coro.h"

#define STACK_FILL 0xA5A5A5A5u    /* so new stacks start with known contents */

struct coro {
    void* sp;
    void* stack;
    unsigned stack_size;
    void (*fn)(void*);
    void* arg;
};

static coro main_coro;
static coro* current;

#if defined(__ARM_PCS_VFP)
#define SAVE_FP "vpush {d8-d15}\n"
#define LOAD_FP "vpop {d8-d15}\n"
#define FP_BYTES 64
#else
#define SAVE_FP ""
#define LOAD_FP ""
#define FP_BYTES 0
#endif

__attribute__((naked, target("arm"))) static void coro_swap(void** save_sp, void* load_sp) {
    __asm__ volatile(
        "push {r4-r11, lr}\n"
        SAVE_FP
        "str sp, [r0]\n"
        "mov sp, r1\n"
        LOAD_FP
        "pop {r4-r11, pc}\n");
}

static void coro_start(void) {
    current->fn(current->arg);
    abort();                                            /* a coroutine must switch away, never return */
}

coro* coro_self(void) {
    if (current == NULL) {
        current = &main_coro;
    }
    return current;
}

coro* coro_create(void (*fn)(void*), void* arg, unsigned stack_size) {
    coro* c = calloc(1, sizeof(coro));
    uint32_t* top;
    coro_self();
    c->fn = fn;
    c->arg = arg;
    c->stack = malloc(stack_size);
    if (c->stack == NULL) {
        fprintf(stderr, "coro_create: no memory for a %u byte stack\n", stack_size);
        abort();
    }
    c->stack_size = stack_size;
    {
        uint32_t* w = c->stack;
        unsigned i;
        for (i = 0; i < stack_size / 4; i++) {
            w[i] = STACK_FILL;
        }
    }
    top = (uint32_t*)(((uintptr_t)c->stack + stack_size) & ~(uintptr_t)7);
    /* What coro_swap pops: d8-d15, r4-r11, then pc = coro_start. Keeps sp 8-byte aligned. */
    top -= 1;
    *top = (uint32_t)(uintptr_t)coro_start;
    top -= 8;
    memset(top, 0, 8 * 4);
    top -= FP_BYTES / 4;
    memset(top, 0, FP_BYTES);
    c->sp = top;
    return c;
}

void coro_delete(coro* c) {
    if (c != NULL && c != current && c != &main_coro) {
        free(c->stack);
        free(c);
    }
}

void coro_switch(coro* to) {
    coro* from = coro_self();
    if (to != from) {
        current = to;
        coro_swap(&from->sp, to->sp);
    }
}
