/*
 * Copyright (c) 2026 Michael Caisse
 *
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
 */
/*
 * Syscall stubs. These are never called -- they exist only to keep the linker
 * quiet.
 *
 * The startup code branches to _mainCRTStartup so that crt0 runs
 * __libc_init_array for the static constructors. crt0 also references exit(),
 * which drags in newlib's stdio teardown (libc_a-findfp.o) and with it the
 * FILE vtable in libc_a-stdio.o, which references _close_r/_lseek_r/_read_r/
 * _write_r. Those in turn reference the four syscalls below.
 *
 * Left undefined, they are resolved out of libnosys.a, whose members each
 * carry a .gnu.warning.<symbol> section -- that is what prints
 * "_write is not implemented and will always fail" at link time. Defining them
 * here means the linker never searches libnosys for them and the warning
 * sections are never pulled in.
 *
 * None of this costs flash: --gc-sections discards the whole chain (check the
 * "Discarded input sections" block of the map file), and it discards these
 * stubs along with it.
 *
 * Signatures match newlib's libnosys so that the definitions stay compatible
 * if a translation unit ever pulls in the real declarations.
 */

#include <errno.h>

int _close(int file) {
    (void)file;
    errno = ENOSYS;
    return -1;
}

int _lseek(int file, int ptr, int dir) {
    (void)file;
    (void)ptr;
    (void)dir;
    errno = ENOSYS;
    return -1;
}

int _read(int file, char *ptr, int len) {
    (void)file;
    (void)ptr;
    (void)len;
    errno = ENOSYS;
    return -1;
}

int _write(int file, char *ptr, int len) {
    (void)file;
    (void)ptr;
    (void)len;
    errno = ENOSYS;
    return -1;
}
