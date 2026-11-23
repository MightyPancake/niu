/* niu.h - the niu header (spec.md "Source files").
 *
 * Including it makes a file a niu file. It has three modes:
 *   __NIU_STD__      niu defines this while it reads the file. niu handles
 *                    the niu keywords itself, so nothing is defined here.
 *   __NIU_TOOLING__  editors, clangd and linters define this to read niu
 *                    files as plain C. Each niu keyword becomes a macro stub
 *                    that parses, but does not do what niu does.
 *   neither          a plain C compiler stops with an error.
 *
 * niu has its own copy of this file built in (src/niu_h.c); the two must be
 * identical, which tests/run.sh checks. */
#ifndef __NIU_H
#define __NIU_H

#if defined(__NIU_STD__)
/* Read by niu itself: the niu keywords are not macros. niu's pre-pass puts
 * code that refers to $ where each keyword is, to find them in the AST. */
#pragma clang system_header
/* $ is reserved in niu files; niu reports a macro named $ itself, so it is
 * set aside here, where it would only add errors about this header. */
#pragma push_macro("$")
#undef $
enum { $ };
#pragma pop_macro("$")
#elif defined(__NIU_TOOLING__)
/* Stubs for editor tools. A deferred statement is kept behind `if (0)`, so it
 * is still parsed and checked, but analysers do not see it run early (for
 * example a deferred free() is not taken as a free() right there). */
#define defer if (0)
#define _Defer if (0)
#define defer_now ((void)0)
#define _Defer_now ((void)0)
/* var and let need type inference, which C99 lacks: __auto_type is the GNU
 * extension Clang and GCC tools know. niu itself writes the type out. */
#define var __auto_type
#define let __auto_type const
#ifdef __clang__
/* -pedantic: __auto_type is a GNU extension (GCC does not warn) */
#pragma clang diagnostic ignored "-Wgnu-auto-type"
#endif
#else
#error "this file must be compiled with niu"
#endif

#endif
