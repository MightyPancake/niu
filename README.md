# The Niu C Standard
The Niu (read as /ɲu/, which sounds like "new", but in Polish) is a C standard and dialect that introduces useful constructs like defer statements or generics containers into older C standards.
It works on your source code before the C compiler sees it: niu constructs are rewritten into plain C, and everything else (your macros, `#include`s and comments) is left exactly as written.
Because of this, most of Niu C can be lowered to C99 or any newer C standard and works using all major C compilers.
It sits between an actual programming language and a macro library.

# The goal
Niu aims at making modern C programmers being able to target older standards and use some tricks and every day, common quality of life changes that are present in other languages, while staying extremely close to the standard C as possible and always being able to take a look at the C output which is supposed to be very human readable.
Niu is aware of the target C standard and is attempting to use the best tools available in the given target to do its job.
It also comes with a lot of tests of industry used, open source projects going through it (unchanged) as well as its own test suite which runs on all major platforms (Linux, Windows, MacOS), on each available compiler (GCC, Clang, MSVC, TCC) forcing each supported output C standard (C99, C11 and C23).

# Features
Niu's features, and their status, are described in the specification: [spec.md](spec.md).

# Installing Niu
To build Niu you only need a C compiler. Most C compilers will do (Niu releases are supported by GCC, Clang, MSVC and TCC). Simply download the repo and compile Niu with your C compiler `$CC niu.c -o niu`.

To run, Niu also needs **libclang 18 or newer**. Niu uses it to parse C, and loads it when it starts, so it is not needed to build Niu.
If Niu cannot find libclang, set `NIU_LIBCLANG` to the path of the library file.
