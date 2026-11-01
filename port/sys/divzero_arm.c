/*
 * divzero_arm.c  Division by zero returns 0 on 32-bit ARM, as on the arcade
 *
 * The game divides by zero in places, and the arcade's compiler (SHC) returned 0 for both quotient and
 * remainder. libgcc saturates instead, so the linker wraps the AEABI helpers (-Wl,--wrap).
 * The *divmod helpers return quotient and remainder in r0:r1, as a 64-bit value.
 */

#include <stdint.h>

int __real___aeabi_idiv(int a, int b);
unsigned __real___aeabi_uidiv(unsigned a, unsigned b);
uint64_t __real___aeabi_idivmod(int a, int b);
uint64_t __real___aeabi_uidivmod(unsigned a, unsigned b);

int __wrap___aeabi_idiv(int a, int b) {
    if (b == 0) {
        return 0;
    }
    return __real___aeabi_idiv(a, b);
}

unsigned __wrap___aeabi_uidiv(unsigned a, unsigned b) {
    if (b == 0) {
        return 0;
    }
    return __real___aeabi_uidiv(a, b);
}

uint64_t __wrap___aeabi_idivmod(int a, int b) {
    if (b == 0) {
        return 0;
    }
    return __real___aeabi_idivmod(a, b);
}

uint64_t __wrap___aeabi_uidivmod(unsigned a, unsigned b) {
    if (b == 0) {
        return 0;
    }
    return __real___aeabi_uidivmod(a, b);
}
