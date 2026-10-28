/* What devkitA64's gcc links from crti.o and crtbegin.o, for a clang build: newlib's
 * __libc_init_array/__libc_fini_array call _init/_fini, and C++ code refers to __dso_handle. The
 * constructors themselves are in .init_array (switch.ld collects them). */
void _init(void) {}
void _fini(void) {}
void* __dso_handle = &__dso_handle;
