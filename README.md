# The Niu C Standard
The Niu (read as /ɲu/, which sounds like "new", but in Polish) is a C standard and dialect that introduces useful constructs like defer statements or generics containers into older C standards.
It is essentially a post-pre-processor which works after the standard C pre-processor does its thing, but before the actual process of compilation.
Because of this, Niu C (or a big part of it*) can be lowered to any C standard and works using all major C compilers.
It sits between an actual programming languages and a macro library.

# The goal
Niu aims at making modern C programmers being able to target older standards and use some tricks and every day, common quality of life changes that are present in other languages, while staying extremely close to the standard C as possible and always being able to take a look at the C output which is supposed to be very human readable.
Niu is aware of the target C standard and is attempting to use the best tools available in the given target to do its job.
It also comes with a lot of tests of industry used, open source projects going through it (unchanged) as well as its own test suite which runs on all major platforms (Linux, Windows, MacOS), on each available compiler (GCC, Clang, MSVC, TCC) forcing each supported output C standard (C89, C99, C11 and C23).

# Features
## Defer statements
TODO

# Installing Niu
To get it running you only need a C compiler. Most C compilers will do (Niu releases are supported by GCC, Clang, MSVC and TCC). Simply download the repo and compile Niu with your C compiler `$CC niu.c -o niu`.
