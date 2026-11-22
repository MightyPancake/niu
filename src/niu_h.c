/* niu_h.c - the niu.h header, embedded in the binary. libclang gets it as an
 * unsaved file, so `#include <niu.h>` resolves without installing anything.
 * Part of niu.c; not compiled on its own.
 *
 * This is a copy of include/niu.h, one string per line; editor tools use that
 * file. Change both together: tests/run.sh fails when they differ. */

static const char niu_h_text[] =
    "/* niu.h - the niu header (spec.md \"Source files\").\n"
    " *\n"
    " * Including it makes a file a niu file. It has three modes:\n"
    " *   __NIU_STD__      niu defines this while it reads the file. niu handles\n"
    " *                    the niu keywords itself, so nothing is defined here.\n"
    " *   __NIU_TOOLING__  editors, clangd and linters define this to read niu\n"
    " *                    files as plain C. Each niu keyword becomes a macro stub\n"
    " *                    that parses, but does not do what niu does.\n"
    " *   neither          a plain C compiler stops with an error.\n"
    " *\n"
    " * niu has its own copy of this file built in (src/niu_h.c); the two must be\n"
    " * identical, which tests/run.sh checks. */\n"
    "#ifndef __NIU_H\n"
    "#define __NIU_H\n"
    "\n"
    "#if defined(__NIU_STD__)\n"
    "/* Read by niu itself: the niu keywords are not macros. niu's pre-pass puts\n"
    " * code that refers to $ where each keyword is, to find them in the AST. */\n"
    "#pragma clang system_header\n"
    "enum { $ };\n"
    "#elif defined(__NIU_TOOLING__)\n"
    "/* Stubs for editor tools. A deferred statement is kept behind `if (0)`, so it\n"
    " * is still parsed and checked, but analysers do not see it run early (for\n"
    " * example a deferred free() is not taken as a free() right there). */\n"
    "#define defer if (0)\n"
    "#define _Defer if (0)\n"
    "#define defer_now ((void)0)\n"
    "#define _Defer_now ((void)0)\n"
    "#else\n"
    "#error \"this file must be compiled with niu\"\n"
    "#endif\n"
    "\n"
    "#endif\n";
