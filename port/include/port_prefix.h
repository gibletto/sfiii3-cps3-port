/*
 * port_prefix.h  Forced include for every game source file
 *
 * Maps the SHC compiler's builtins, and renames the game's own C library functions so they don't clash
 * with the host's.
 */

#ifndef PORT_PREFIX_H
#define PORT_PREFIX_H

/* HOST_PORT and HOST_LITTLE_ENDIAN are defined by the build */

/* SHC builtins */
void host_set_imask(int level);
int host_get_imask(void);
#define _builtin_set_imask(level) host_set_imask(level)
#define _builtin_get_imask() host_get_imask()
#define _builtin_strcpy(dst, src) strcpy(dst, src)

/* the game's own versions of C library functions */
#define memset sf3_memset
#define memcmp sf3_memcmp
#define strcpy sf3_strcpy
#define strcat sf3_strcat
#define strlen sf3_strlen
#define strstr sf3_strstr

#endif
