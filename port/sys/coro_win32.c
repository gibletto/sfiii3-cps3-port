/*
 * coro_win32.c  Coroutines on Win32 fibers
 */

#include <windows.h>
#include <stdlib.h>

#include "coro.h"

struct coro {
    void* fiber;
    void (*fn)(void*);
    void* arg;
    int owns_fiber;
    unsigned stack_size;
};

static coro* current;

static void WINAPI fiber_entry(void* p) {
    coro* c = p;
    c->fn(c->arg);
    abort();                                            /* a coroutine must switch away, never return */
}

coro* coro_self(void) {
    if (current == NULL) {
        current = calloc(1, sizeof(coro));
        current->fiber = ConvertThreadToFiber(NULL);
    }
    return current;
}

coro* coro_create(void (*fn)(void*), void* arg, unsigned stack_size) {
    coro* c = calloc(1, sizeof(coro));
    coro_self();
    c->fn = fn;
    c->arg = arg;
    c->stack_size = stack_size;
    c->fiber = CreateFiber(stack_size, fiber_entry, c);
    c->owns_fiber = 1;
    return c;
}

/* a fiber can't be rewound: make a new one (the PC has the memory) */
void coro_restart(coro* c, void (*fn)(void*), void* arg) {
    if (c != current && c->owns_fiber) {
        DeleteFiber(c->fiber);
        c->fn = fn;
        c->arg = arg;
        c->fiber = CreateFiber(c->stack_size, fiber_entry, c);
    }
}

void coro_delete(coro* c) {
    if (c != NULL && c != current) {
        if (c->owns_fiber) {
            DeleteFiber(c->fiber);
        }
        free(c);
    }
}

void coro_switch(coro* to) {
    coro_self();
    if (to != current) {
        current = to;
        SwitchToFiber(to->fiber);
    }
}
