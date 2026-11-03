/*
 * divzero_win32.c  Division by zero returns 0 on 32-bit x86 Windows, as on the arcade
 *
 * The game divides by zero in places, and the arcade's compiler (SHC) returned 0 for both quotient and
 * remainder. x86 traps instead. This exception handler decodes the faulting DIV/IDIV, sets the result
 * registers to 0 and skips the instruction. Only divisions in the exe itself are handled.
 */

#if defined(_WIN32) && defined(_M_IX86) || defined(_WIN32) && defined(__i386__)

#include <windows.h>

static const unsigned char* image_lo;      /* the exe in memory */
static const unsigned char* image_hi;

static LONG CALLBACK divzero_handler(EXCEPTION_POINTERS* ep) {
    CONTEXT* c = ep->ContextRecord;
    const unsigned char* p = (const unsigned char*)c->Eip;
    int op16 = 0;
    int byte_op;
    int len;
    unsigned char modrm;
    int mod, rm;

    /* only the program's own code: a division by zero in SDL or a driver is a real bug */
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_INT_DIVIDE_BY_ZERO || p < image_lo || p >= image_hi) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    len = 0;
    while (p[len] == 0x66 || p[len] == 0x2E || p[len] == 0x3E || p[len] == 0x26 || p[len] == 0x36 || p[len] == 0x64 ||
           p[len] == 0x65) {
        if (p[len] == 0x66) {
            op16 = 1;
        }
        len++;
    }
    if (p[len] != 0xF6 && p[len] != 0xF7) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    byte_op = p[len] == 0xF6;
    modrm = p[len + 1];
    if (((modrm >> 3) & 7) < 6) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    mod = modrm >> 6;
    rm = modrm & 7;
    len += 2;
    if (mod != 3) {
        if (rm == 4) {
            unsigned char sib = p[len++];
            if (mod == 0 && (sib & 7) == 5) {
                len += 4;
            }
        }
        if (mod == 1) {
            len += 1;
        } else if (mod == 2 || (mod == 0 && rm == 5)) {
            len += 4;
        }
    }
    if (byte_op) {
        c->Eax &= 0xFFFF0000;               /* AL quotient, AH remainder */
    } else if (op16) {
        c->Eax &= 0xFFFF0000;
        c->Edx &= 0xFFFF0000;
    } else {
        c->Eax = 0;
        c->Edx = 0;
    }
    c->Eip += len;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void host_divzero_install(void) {
    const unsigned char* base = (const unsigned char*)GetModuleHandleA(NULL);
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    image_lo = base;
    image_hi = base + nt->OptionalHeader.SizeOfImage;
    AddVectoredExceptionHandler(1, divzero_handler);
}

#else

void host_divzero_install(void) {
}

#endif
