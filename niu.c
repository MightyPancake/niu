/* niu - the Niu C transpiler.
 *
 * Build:  cc niu.c -o niu      (MSVC: cl niu.c)
 *
 * This is the only file to compile. It includes the rest of the sources
 * from src/, needs no build system, and links no libraries: libclang is
 * loaded when niu runs (see src/libclang.c). Portable C99 without VLAs;
 * everything platform-specific is in src/platform_specific.c. */

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS 1
#define _CRT_NONSTDC_NO_WARNINGS 1
#define WIN32_LEAN_AND_MEAN 1
#define NOMINMAX 1
#else
#define _XOPEN_SOURCE 700 /* POSIX.1-2008 with XSI: popen, realpath */
#endif

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "src/util.c"
#include "src/platform_specific.c"
#include "src/libclang.c"
#include "src/niu_h.c"
#include "src/edits.c"
#include "src/output.c"
#include "src/frontend.c"
#include "src/cli.c"

int main(int argc, char **argv)
{
    return niu_main(argc, argv);
}
