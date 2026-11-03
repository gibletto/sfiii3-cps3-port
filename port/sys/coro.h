/*
 * coro.h  Stackful coroutines for the game's task kernel
 *
 * Each game task runs on its own stack. coro_switch resumes a coroutine and returns when something
 * switches back. Backends: coro_win32.c (Windows fibers), coro_arm.c (3DS).
 */

#ifndef CORO_H
#define CORO_H

typedef struct coro coro;

coro* coro_self(void);                                  /* the running coroutine (the first call adopts the thread) */
coro* coro_create(void (*fn)(void*), void* arg, unsigned stack_size);
void coro_restart(coro* c, void (*fn)(void*), void* arg);   /* starts c's function again on the same stack */
void coro_delete(coro* c);
void coro_switch(coro* to);

#endif
