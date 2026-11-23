# The Niu C Standard
The Niu (read as /ɲu/, which sounds like "new", but in Polish) is a C standard and dialect that introduces useful constructs like defer statements or generics containers into older C standards.
It works on your source code before the C compiler sees it: niu constructs are rewritten into plain C, and everything else (your macros, `#include`s and comments) is left exactly as written.
Because of this, Niu C can be lowered to C99 or any newer C standard and works using all major C compilers. A few specific uses of a feature cannot be written in an older standard (for example inferring the type of an unnamed struct); niu reports those as errors, and each feature's chapter lists them.
It sits between an actual programming language and a macro library.

## Source files
Niu code lives in ordinary `.c` and `.h` files. A file becomes a *niu file* by including the niu header:

```c
#include <niu.h>
```

- **niu keywords only work in niu files,** from the `#include <niu.h>` line on. All other files (system headers, plain C files, headers that don't include `niu.h`) are passed to the compiler untouched, so they can keep using names like `defer`.
- **A header that uses niu features includes `<niu.h>` itself.**
- **`niu.h` is also a guard.** If a niu file is compiled by a plain C compiler instead of niu, `niu.h` stops the build with `#error "this file must be compiled with niu"`. Editors and other tools can define `__NIU_TOOLING__` to read niu files without niu.
- **`niu.h` has three modes**, chosen by what is defined when it is read:

  | Defined | Used by | What `niu.h` does |
  |---|---|---|
  | `__NIU_STD__` | niu itself, which defines it while it reads a file | Declares `enum { $ };`, which niu uses internally to find its keywords (see "Reserved names" below). The niu keywords are not macros here. niu handles them itself. |
  | `__NIU_TOOLING__` | editors, clangd, linters | Defines each niu keyword as a macro stub that parses but does not do what niu does: `defer` and `_Defer` become `if (0)`, `defer_now` and `_Defer_now` become `((void)0)`. See "Editor tools" below. |
  | neither | a plain C compiler | Stops with `#error "this file must be compiled with niu"`. |
- **Reserved names.** In a niu file, identifiers starting with `__niu` are reserved, because niu generates names with this prefix. The identifier `$` is reserved too, because `niu.h` declares it. Declaring `$` yourself (as a variable, function, parameter, typedef or enum constant) or defining an object-like macro named `$` is an error. A function-like macro `$(x)`, a tag, a struct member or a label named `$`, and names that only contain a `$` (such as `sys$open`), are fine. Plain files are not affected.
- **The generated C does not need niu.** In the output, niu replaces the `#include <niu.h>` line with a comment, so the result compiles with any C compiler.
- **Being a niu file does not spread.** A file that gets `niu.h` only indirectly, through another header, is still a plain C file. Every niu file includes `<niu.h>` itself.
- **Include plain headers first.** In a niu file, plain C headers (system headers included) should be included before any niu header and before `<niu.h>`. See "Editor tools" below for why.

### Editor tools
Editors, clangd, linters and other C tools read niu files as plain C. To let them understand niu code, `niu.h` defines each niu keyword as a macro when `__NIU_TOOLING__` is defined (for example in `compile_commands.json`). Because C macros stay active until the end of the translation unit, those macros also reach every file included after `niu.h`, including plain ones. This causes two kinds of mismatch between what an editor shows and what niu does:

- **A plain header included after `niu.h`** that uses a name like `defer` (for example a struct field) looks broken in the editor, although niu builds it without problems. Including plain headers first avoids this.
- **A file that gets `niu.h` only indirectly** looks fine in the editor when it uses niu keywords, but niu rejects it, because the file is not a niu file.

### Output
`niu transpile` writes a lowered copy of each niu file next to the original. The name of the copy depends on the name of the original:

- **A name ending in `.niu.c` or `.niu.h`:** the `.niu` is dropped, so the lowered copy gets the plain name.
- **Any other name:** `.plain` is added before the extension.

```
src/main.niu.c  →  src/main.c
src/util.niu.h  →  src/util.h
src/app.c       →  src/app.plain.c
src/vec.h       →  src/vec.plain.h
```

Naming files `.niu.c` is optional. It is useful when the lowered C is meant to be kept or shipped under normal names. Plain `.c` names keep the generated files clearly marked instead. The name only decides the name of the output: a `.niu.c` file still needs `#include <niu.h>` to be a niu file, and niu warns when it doesn't have one.

- **Which files are written:** the `.c` file being transpiled, every niu file it includes (directly or indirectly), and every plain file that lies on an include chain leading to a niu file. For example, if the plain header `common.h` includes the niu header `util.h`, the output also needs a `common.plain.h` that includes the lowered `util.h` instead of the original. In such plain files only the include lines change. All other plain files and system headers are not touched.
- **Only includes of written files change.** In the lowered files, each `#include` of a file that niu writes a copy of (a niu file, or a plain file on a chain leading to one) is renamed the same way as the file itself: `"util.niu.h"` becomes `"util.h"`, `"vec.h"` becomes `"vec.plain.h"`, and `"common.h"` becomes `"common.plain.h"`. Every other `#include` stays exactly as written. Nothing moves, so all relative includes, `-I` paths and absolute paths find the same files as before.
- **Errors still point at the originals.** Each lowered file starts with a comment saying it was generated by niu, followed by `#line 1 "util.h"`. Compiler errors and `__FILE__` therefore name `util.h` and its real line numbers.
- **`--no-line` leaves out the `#line` directives.** Use it for output that is shipped or kept without the niu sources, where `#line` would point at files that are not there. Errors then point at the lowered files themselves.
- **Existing files are never overwritten by accident.** niu only replaces a file that it generated itself. If the name is taken by a file that niu did not write (for example a hand-written `main.c` next to `main.niu.c`), niu reports an error.
- **A niu header included by several `.c` files is written once.** Its lowered form must be the same for all of them. If it would differ, niu reports an error.
- **Version control:** generated `.plain` files can be ignored with `*.plain.c` and `*.plain.h`. Files generated from `.niu.c` and `.niu.h` sources have plain names, so they need to be listed by name if they should be ignored.

**`niu build` and `niucc` write nothing next to your sources.** Their output is not kept, so niu writes a single temporary file instead: the lowered `.c` file, with the lowered text of each niu header pasted in place of its `#include`, between `#line` markers. Plain files on an include chain leading to a niu header are pasted the same way, so that their includes reach the lowered header too. A header with an include guard or `#pragma once` is pasted only once. This way parallel builds with different configurations cannot overwrite each other's files, and read-only source trees work.

**The output is for one configuration.** niu can only lower code that the preprocessor keeps. niu code inside an `#if`/`#ifdef` branch that was not active (because of the `-D` flags, the target or the standard niu was run with) is left as written, and niu warns about it. Compiling the output with a different configuration that turns such a branch on would hit unlowered niu code. To support several configurations, transpile once per configuration.

**Later (TODO):**
- `niu eject`: leave niu for good. Replaces each niu file with its lowered version: a `.niu.c` file is removed in favour of its plain `.c` output, and a `.plain` file takes over the original name, with the `.plain` suffix removed from includes. The result never contains `#line` directives, because the niu sources are gone. Meant to be used with version control.
- `niu export <dir>`: copy the lowered files and the plain files they use into a separate directory, for shipping plain C to people without niu.

### Diagnostics
**Niu keyword in a plain file (error).** When the C compiler reports an error on a token spelled like a niu keyword, in a file that is not a niu file, niu replaces the message:
```
main.c:5:5: error: 'defer' used in a file that is not a niu file
main.c:1:1: note: add '#include <niu.h>' to make main.c a niu file
common.h:1:10: note: <niu.h> is included here, but that only makes common.h a niu file
```
The last note appears only when `niu.h` reached the file indirectly. Because niu only reacts to existing compiler errors, a plain file that legitimately uses `defer` as a name is never reported.

**Plain header after niu code (warning, `-Wniu-include-order`).** In a niu file, an `#include` of a plain file that comes after `<niu.h>` or after a niu header is reported:
```
main.c:3:1: warning: plain header '<string.h>' is included after niu code [-Wniu-include-order]
main.c:2:1: note: "util.h" is a niu file and makes niu's keywords visible to editor tools
main.c:3:1: note: move this #include above "util.h"
```
Files that must be included late on purpose, such as X-macro `.def` files, have two options:
- make the late file a niu file by adding `#include <niu.h>` to it. Niu files are allowed after niu code, so the warning goes away. `niu.h` has an include guard, so this works even in files that are included many times.
- turn the warning off with `-Wno-niu-include-order`.

**Niu code in an inactive branch (warning, `-Wniu-inactive`).** Only reported by `niu transpile`, because `niu build` and `niucc` lower every build fresh. See "The output is for one configuration" above:
```
main.c:12:5: warning: niu code in an inactive '#ifdef DEBUG' branch is not lowered [-Wniu-inactive]
main.c:12:5: note: the output will not compile with this branch enabled; transpile again with that configuration
```

**Niu header lowered differently (error).** When two `.c` files transpiled together would need different lowered versions of the same niu header (for example because they `#define` different macros before including it):
```
util.h: error: 'util.h' would be lowered differently for main.c and other.c
```

**`.niu` name without `niu.h` (warning, `-Wniu-file-name`).** A file named `.niu.c` or `.niu.h` that does not include `<niu.h>`, so it is not a niu file:
```
src/util.niu.h: warning: 'util.niu.h' is named like a niu file but does not include <niu.h> [-Wniu-file-name]
```

**Transpiling a plain file (warning, `-Wniu-plain-file`).** `niu transpile` on a file that does not include `<niu.h>` writes an unchanged copy, which is usually a mistake. `niu check` does not give this warning:
```
src/app.c: warning: 'app.c' is not a niu file (it does not include <niu.h>), so 'app.plain.c' is an unchanged copy [-Wniu-plain-file]
```

**Include of a niu file that cannot be renamed (error).** niu can only rename includes whose file name is written out literally. An include whose name comes from a macro (`#include CONFIG_HEADER`), or a `__has_include` check, that refers to a niu file is reported:
```
src/main.c:4:10: error: cannot rename this include of the niu file 'util.h': its name comes from the macro 'CONFIG_HEADER'
src/main.c:4:10: note: write the file name out, for example #include "util.h"
```

**Generated file name taken (error).** When the name of a lowered copy is already taken by a file that was not written by niu:
```
src/util.h: error: cannot write 'src/util.plain.h': the file exists and was not generated by niu
```

## Features
Status: **TODO** means the feature is not specified yet, **Specified** means its chapter is written, and **Done** means it is specified and implemented.

| Feature | Status | Summary |
|---|---|---|
| [`defer`](#defer-statement) | [Done](#defer-statement) | Run a cleanup statement on every exit from the current block. Includes `defer_now`, which runs the pending cleanups early, right before `exit` or `abort`. |
| [`infer`](#infer) | TODO | Declare a variable whose type comes from its initializer. |
| [`let`](#let) | TODO | Like `infer`, but the variable is `const`. |
| [Generic types](#generic-types) | TODO | Define a type template once and use it with different types. |
| [`__mangle`](#__mangle) | TODO | Turn a type into a name fragment for building identifiers. |
| [Variable-length arrays](#variable-length-arrays) | TODO | VLAs in niu code without VLAs in the output. |

### Defer statement
The defer statement queues a cleanup action that runs when the surrounding code is done.
In some languages (like Go) defers are bound to functions. Niu defers are bound to scopes, as in the WG14 defer TS: a deferred statement runs on every exit from the block it was written in.

```c
{
    int* ptr = (int*)malloc(16*sizeof(int));
    defer free(ptr);
    //use pointer
}   /* free(ptr) runs here */
```

#### Syntax
```
defer-statement:
    defer statement
    _Defer statement

defer-now-statement:
    defer_now ;
    _Defer_now ;
```
The statement after `defer` is called the *deferred statement*. It is usually a single statement or a block:

```c
defer free(ptr);
defer {
    flush(log);
    fclose(log);
}
```

`defer` and `_Defer` (the spelling used by the TS) mean the same thing. `defer_now` and `_Defer_now` run the pending defers early (see Semantics). Like every niu keyword, they only work in niu files (see Source files). In niu files they are always keywords, so they cannot be used as names there (`int defer = 3;` is an error). Code that needs such a name can stay in a plain C file.

#### Constraints
A program that breaks any of these rules is rejected with an error.

1. **A defer may only appear directly inside braces `{ }`.** It cannot be the body of an `if`, `else`, `while`, `for`, `do` or `switch` without braces, it cannot have a label, and it cannot appear at file scope.
   ```c
   if (p) defer free(p);        /* error: the defer would run immediately */
   ```
   Adding braces makes it valid, but probably not what you meant: the defer belongs to the `if` block, so `free(p)` runs as soon as that block ends, not at the end of the function. To clean up conditionally at the end of the enclosing block, put the condition inside the deferred statement:
   ```c
   if (p) { defer free(p); }    /* valid, but free(p) runs right here, when the if block ends */
   defer if (p) free(p);        /* runs at the end of the enclosing block, only if p is set */
   ```

2. **The deferred statement cannot jump out of itself.** It cannot contain `return`, nor a `break`, `continue` or `goto` whose target is outside the deferred statement. Jumps that stay inside it are fine.
   ```c
   defer { if (!ok) return 1; }               /* error */
   defer { for (;;) { if (x) break; } }       /* fine: the break stays inside */
   ```

3. **Nothing may jump into the scope of a defer.** The scope of a defer runs from the defer statement to the end of its block. A `goto` or a `case`/`default` label must not land inside that scope from outside it, because the defer would never have been reached.
   ```c
   goto skip;            /* error: jumps past the defer into its scope */
   defer free(p);
   skip:
   use(p);

   switch (x) {
       defer log_exit();
   case 1:               /* error: 'case' label is inside the scope of the defer */
       break;
   }
   ```

4. **The deferred statement cannot contain labels.** It is copied to every exit, so a label inside it would be defined more than once. (`case` and `default` labels of a `switch` that is itself inside the deferred statement are fine.)

5. **The deferred statement cannot declare `static` or `_Thread_local` variables.** Each copy would create a separate variable, which changes what the code means.

6. **Names in the deferred statement must mean the same thing at every exit.** Because the deferred statement is copied, a name declared between the defer and an exit could silently change which variable a copy uses:
   ```c
   int *p = malloc(n);
   defer free(p);
   {
       int *p = other;
       return 0;         /* error: 'p' in the deferred statement would refer to the inner 'p' here */
   }
   ```
   This applies to variables, functions, typedef names, tags and enum constants.

7. **No computed `goto` (`goto *ptr;`, a GNU extension) inside the scope of a defer.** niu cannot know where it lands.

8. **Identifiers starting with `__niu`, and the identifier `$`, are reserved.** niu generates names with the `__niu` prefix, and it uses `$` internally to find its keywords. Declaring either yourself is an error. See "Reserved names" under Source files for exactly which uses of `$` are allowed.

9. **After `defer_now`, control must not reach an exit that would run the same defers again.** Every path from a `defer_now` must end in a call to a `noreturn` function (`exit`, `abort`, `longjmp`, or a function declared `_Noreturn`, `[[noreturn]]` or `__attribute__((noreturn))`) before it reaches the end of a block, a `return`, `break`, `continue` or `goto` that would run any of the defers it already ran. Otherwise those defers would run twice.
   ```c
   char *p = malloc(8);
   defer free(p);
   if (bad) {
       defer_now;
       fprintf(stderr, "fatal\n");   /* fine: no exit is reached yet */
       exit(1);                      /* fine: noreturn */
   }
   if (worse) {
       defer_now;
       log_error("worse");
   }                                 /* error: control continues, free(p) would run again later */
   ```

10. **`defer_now` cannot appear inside a deferred statement.** It would run the defer that is currently running.

11. **Macros in the deferred statement must not change before an exit.** The deferred statement is copied as written, so the compiler expands its macros again at every exit. A macro it uses (directly, or through another macro) must not be `#define`d or `#undef`d between the defer and any exit it is copied to. Otherwise that copy would do something different from what was written:
    ```c
    #define LOG(msg) fprintf(stderr, "%s\n", msg)
    defer LOG("leaving");
    ...
    #undef LOG              /* error: 'LOG' is used by the defer above */
    #define LOG(msg)
    ...
    return 0;               /* this copy of LOG("leaving") would expand to nothing */
    ```
    For the same reason, `__COUNTER__` cannot be used in a deferred statement: it would have a different value in every copy. (`__LINE__` is fine, see "Line numbers in copies" under Lowering.)

A defer inside a deferred statement is allowed. Its scope is the deferred statement, and all the rules above apply to it as well.

#### Macros
niu leaves every macro use as written, and it cannot edit the text a macro expands to. That leads to these rules:

- **An exit that comes from a macro cannot leave the scope of a defer.** niu would have to add the copies inside the macro's expansion:
  ```c
  #define CHECK(x) if (!(x)) return -1
  defer free(p);
  CHECK(p != NULL);       /* error: the 'return' comes from the macro 'CHECK' */
  ```
  Exits from macros that no defer covers are fine, and so are exits from macros used inside a deferred statement when they stay inside it.
- **A defer can be the last statement of a macro.** The defer runs at the exits of the block where the macro is used, as if it had been written there:
  ```c
  #define AUTOFREE(p) defer free(p)
  #define WITH_LOG(s) log_enter(s); defer log_leave(s)

  char *buf = malloc(64);
  AUTOFREE(buf);          /* free(buf) runs at every exit of this block */
  ```
  Such a macro must follow these rules:
  - It can contain only one `defer`, and that `defer` must be its last statement. The deferred statement runs to the end of the macro.
  - The deferred statement cannot contain another `defer`, nor `defer_now`.
  - The macro must be defined in the niu file itself, and used directly in the code, not from inside another macro.
- **A macro can stand for the keyword alone.** `#define DEFER defer` works like `defer`. The deferred statement is then written out after the macro's use (`DEFER free(p);`), not taken from another macro.
- **`defer_now` cannot be used in a macro.**

Any other macro that contains a niu keyword is an error.

#### Semantics
**When a defer runs.** A defer is registered when execution reaches it. It runs when execution leaves its scope (from the defer statement to the end of the block) by any of these:
- reaching the closing `}` of the block;
- `return`;
- `break` or `continue` that leaves the block (the loop or `switch` it targets is outside the block);
- `goto` to a label outside the scope, including a label earlier in the same block.

Exits that happen before a defer is reached do not run it:
```c
void foo(){
    defer a;
    return;      /* runs only a */
    defer b;
}                /* (unreachable) would run b, then a */
```

**Order.** When several defers are left at once, they run in reverse order: the most recently registered first, and inner blocks before outer ones.

```c
int f(void) {
    char *a = malloc(8);
    defer free(a);
    {
        char *b = malloc(8);
        defer free(b);
        if (err) return -1;   /* runs free(b), then free(a) */
    }
    return 0;                 /* runs free(a) */
}
```

**Values.** The deferred statement sees variables as they are when it runs, not as they were when the defer was reached:
```c
int n = 1;
defer printf("%d\n", n);   /* prints 2 */
n = 2;
```

The **`return`** expression is evaluated first (and converted to the function's return type). Then the defers run, and then the function returns that value. So a deferred statement can safely clean up something the return expression used:
```c
return use_bar(bar);   /* use_bar runs first, then cleanup(bar) */
```

**`break` and `continue`** run only the defers of the blocks they leave. A `break` inside a `switch` leaves only the `switch`:
```c
for (;;) {
    char *msg = next();
    defer free(msg);
    switch (kind(msg)) {
    case QUIT: break;      /* leaves only the switch: free(msg) does not run here */
    case SKIP: continue;   /* leaves the loop body: free(msg) runs */
    }
    handle(msg);
}                          /* free(msg) runs at the end of every iteration */
```

**`goto`** runs the defers of every scope it leaves:
```c
{
    char *tmp = malloc(64);
    defer free(tmp);
    if (fail()) goto out;   /* free(tmp) runs */
    use(tmp);
}
out:
```
This includes jumping backwards to a label before the defer, since that leaves the defer's scope. The defer is registered again when execution reaches it the next time:
```c
{
    conn *c;
retry:
    c = open_conn();
    defer close_conn(c);
    if (busy(c)) goto retry;   /* close_conn(c) runs, then the loop tries again */
    use(c);
}
```

**Statement expressions** (`({ ... })`, a GNU extension) work like `return`. The value of a statement expression is its last expression statement. That value is computed first, then the defers of the statement expression's block run, and then the value is used:
```c
size_t n = ({ char *s = get(); defer free(s); strlen(s); });   /* strlen(s) runs, then free(s) */
```

**Exits that skip defers.** Leaving through `exit`, `_Exit`, `quick_exit`, `abort`, `longjmp`, a signal, or any other `noreturn` function does **not** run defers:
```c
{
    char *p = malloc(8);
    defer free(p);
    if (!p) exit(1);   /* free(p) does not run */
}
```

**Running defers early with `defer_now`.** To clean up before one of those exits, write `defer_now;` first. It runs every pending defer of the current function, in the same order a `return` at that point would:
```c
int parse(const char *path){
    FILE *f = fopen(path, "r");
    defer fclose(f);
    char *buf = malloc(4096);
    defer free(buf);
    if (!valid(buf)) {
        defer_now;     /* runs free(buf), then fclose(f) */
        exit(1);
    }
    ...
}
```
`defer_now` only reaches the defers of the function it is written in. Defers in the functions that called it do not run, because `exit` or `longjmp` leaves those functions too and niu cannot reach them. Constraint 9 makes sure the defers it runs are never run a second time. The one case niu cannot check is a `longjmp` that lands on a `setjmp` in the same function: execution then continues inside the scope of defers that already ran, and they run again at the next exit. Avoiding that is up to the programmer.

#### Lowering
This section describes the C code niu generates. The approach is plain and simple copying: the deferred statement is copied to every exit it covers.

**The defer itself** is replaced by a comment containing the original text, so the output is still easy to read. Inside the comment, any `*/` is written as `*\/` and any `/*` as `/\*`, so the comment can't end early and compilers don't warn about nested comments. The comment also keeps the same line breaks as the original, so the line numbers of everything after it stay the same.

```c
{
    int* ptr = (int*)malloc(16*sizeof(int));
    defer free(ptr);
    //use pointer
}
```
becomes
```c
{
    int* ptr = (int*)malloc(16*sizeof(int));
    /* defer free(ptr); */
    //use pointer
free(ptr); }
```

**End of block.** The deferred statements are pasted on the line of the closing `}`, right before it, most recent first. Using the brace's line keeps every later line number unchanged. The line before the brace cannot be used, because it might end in a `//` comment that would swallow the copy. If the `}` comes from a macro, the copies go right before that macro's use.
```c
{
    defer a;
    defer b;
}
```
becomes
```c
{
    /* defer a; */
    /* defer b; */
b; a; }
```
If the block's last statement is a `return`, `break`, `continue` or `goto`, control can never reach the `}`, so niu leaves out this copy.

**`break`, `continue` and `goto`** are turned into a block that runs the copies and then jumps:
```c
int i = 0;
while(10 > i++){
    int* ptr = (int*)malloc(16*sizeof(int));
    int* ptr2 = (int*)malloc(16*sizeof(int));
    defer { free(ptr); free(ptr2); }
    if (i==8) break;
    if (i==5) continue;
    foo(ptr, ptr2);
}
```
becomes
```c
int i = 0;
while(10 > i++){
    int* ptr = (int*)malloc(16*sizeof(int));
    int* ptr2 = (int*)malloc(16*sizeof(int));
    /* defer { free(ptr); free(ptr2); } */
    if (i==8) { { free(ptr); free(ptr2); } break; }
    if (i==5) { { free(ptr); free(ptr2); } continue; }
    foo(ptr, ptr2);
{ free(ptr); free(ptr2); } }
```
The deferred statement is copied as written, so it can be a block itself. Here it has 3 exits (`break`, `continue` and the end of the loop body), and all 3 are covered.

**`return`** stores the return value in a temporary first, then runs the copies, then returns the temporary:
```c
int foo(){
    void* bar = func_that_needs_cleanup();
    defer cleanup(bar);
    return use_bar(bar);
}
```
becomes
```c
int foo(){
    void* bar = func_that_needs_cleanup();
    /* defer cleanup(bar); */
    { int __niu_return_temp = use_bar(bar); cleanup(bar); return __niu_return_temp; }
}
```
- The temporary has the function's declared return type (`int` here), without qualifiers such as `const`.
- Each `return` gets its own block, so the temporaries never collide.
- In a `void` function, `return;` becomes `{ cleanup(bar); return; }`, with no temporary.
- GCC and Clang also accept `return f();` in a `void` function when `f` returns `void`. This becomes `{ f(); cleanup(bar); return; }`.
- A `return` that no defer covers is left unchanged.

**Statement expressions** are lowered like `return`. The value of the last expression statement is stored in a temporary, the copies run, and the temporary becomes the last expression statement:
```c
size_t n = ({ char *s = get(); defer free(s); strlen(s); });
```
becomes
```c
size_t n = ({ char *s = get(); /* defer free(s); */ size_t __niu_value_temp = strlen(s); free(s); __niu_value_temp; });
```
The temporary has the type of that expression, without qualifiers. If the last statement is not an expression, or its type is `void`, the copies are simply pasted after it.

**Several scopes at once.** An exit that leaves several scopes gets the copies of all of them, innermost first:
```c
int parse(const char *path){
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    defer fclose(f);
    char *buf = malloc(4096);
    if (!buf) return -2;
    defer free(buf);
    while (fgets(buf, 4096, f)) {
        char *line = strdup(buf);
        defer free(line);
        if (line[0] == '#') continue;
        if (bad(line)) return -3;
    }
    return 0;
}
```
becomes
```c
int parse(const char *path){
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    /* defer fclose(f); */
    char *buf = malloc(4096);
    if (!buf) { int __niu_return_temp = -2; fclose(f); return __niu_return_temp; }
    /* defer free(buf); */
    while (fgets(buf, 4096, f)) {
        char *line = strdup(buf);
        /* defer free(line); */
        if (line[0] == '#') { free(line); continue; }
        if (bad(line)) { int __niu_return_temp = -3; free(line); free(buf); fclose(f); return __niu_return_temp; }
    free(line); }
    { int __niu_return_temp = 0; free(buf); fclose(f); return __niu_return_temp; }
}
```
The first `return -1` comes before any defer, so it is unchanged. The function body ends in a `return`, so there is no end-of-block copy.

**Line numbers in copies.** Comments are left out of the copies. A copy that fits on one line is pasted inline, as in the examples above. A copy of a deferred statement that spans several lines, or that uses macros, is placed on its own lines between `#line` directives. Compiler errors in it then point at the original defer, and `__LINE__` inside it has the same value as at the defer:
```c
12    defer {
13        flush(log);
14        fclose(log);
15    }
      ...
30    if (err) return -1;
```
becomes
```c
    if (err) { int __niu_return_temp = -1;
#line 12 "main.c"
    {
        flush(log);
        fclose(log);
    }
#line 30 "main.c"
    return __niu_return_temp; }
```

**A macro that contains a defer** keeps its name and every use as written. niu comments out the defer part of the `#define`, and adds a *companion macro* `__niu_defer_<NAME>` before it, with the same parameters and the deferred statement as its body. A `#line` directive after the companion keeps the line numbers of the original `#define`. Each copy is a use of the companion macro, with the arguments of the original use, so it goes on its own lines between `#line` directives:
```c
26  #define AUTOFREE(p) defer free(p)
    ...
38      char *buf = malloc(8);
39      AUTOFREE(buf);
40      if (!ok)
41          return -1;
```
becomes
```c
#define __niu_defer_AUTOFREE(p) free(p)
#line 26 "main.c"
#define AUTOFREE(p) /* defer free(p) */
    ...
    char *buf = malloc(8);
    AUTOFREE(buf);
    if (!ok)
        { int __niu_return_temp = -1;
#line 39 "main.c"
    __niu_defer_AUTOFREE(buf);
#line 41 "main.c"
        return __niu_return_temp; }
```
A macro that stands for the keyword alone (`#define DEFER defer`) becomes `#define DEFER /* defer */`. Its defers are lowered like any other, with the statement written after the use as the deferred statement.

**Types without a name.** A temporary for a returned value or a statement expression's value needs a type that can be written down. If that type has no name (for example an unnamed `struct` returned by a function, or the value of `({ ... })` having one), niu reports an error instead of lowering the code. Give the type a tag or a typedef name.

**`defer_now`** becomes the copies a `return` at the same place would get, and nothing else:
```c
if (!valid(buf)) {
    /* defer_now; */ { free(buf); fclose(f); }
    exit(1);
}
```

#### Diagnostics
niu reports each broken constraint as an error at the offending location, and points back to the defer involved:

| Constraint | Message |
|---|---|
| 1 | `'defer' must appear directly inside a block '{ }'` |
| 2 | `'return' cannot leave a deferred statement` (and the same for `break`, `continue`, `goto`) |
| 3 | `'goto' jumps into the scope of a 'defer'`, `'case' label is inside the scope of a 'defer'`, `'default' label is inside the scope of a 'defer'` |
| 4 | `labels are not allowed in a deferred statement` |
| 5 | `'static' variables are not allowed in a deferred statement`, `'_Thread_local' variables are not allowed in a deferred statement` |
| 6 | `'p' in this deferred statement would refer to a different declaration at this exit`, with a note at the inner declaration |
| 7 | `computed 'goto' is not allowed in the scope of a 'defer'` |
| 8 | `identifiers starting with '__niu' are reserved`, `'$' is reserved in niu files` |
| 9 | `control can leave the scope of a 'defer' after 'defer_now'; its cleanup would run twice`, with a note at the `defer_now` |
| 10 | `'defer_now' cannot appear inside a deferred statement` |
| 11 | `'LOG' is redefined while a deferred statement that uses it is still pending`, with a note at the defer; `'__COUNTER__' cannot be used in a deferred statement` |

Errors in the deferred statement itself (for example a misspelled name) are reported once, at the defer, not once per copy.

Wrong uses of the keywords themselves:

| Situation | Message |
|---|---|
| A keyword used as a name (`int defer = 3;`) | `'defer' is a keyword in niu files and cannot be used as a name` |
| Nothing after `defer` | `expected a statement after 'defer'` |
| `else` after a defer statement | `'else' without a previous 'if'` |
| `defer_now` without its `;` | `expected ';' after 'defer_now'` |
| `defer_now` inside an expression | `'defer_now' must be a statement of its own` |

Macros (see "Macros" above):

| Situation | Message |
|---|---|
| An exit from a macro leaves the scope of a defer | `'return' comes from the macro 'CHECK' and leaves the scope of a 'defer'; niu cannot add the cleanup there` (and the same for `break`, `continue`, `goto`), with a note at the defer |
| More than one defer in a macro | `a macro can contain only one 'defer', as its last statement` |
| The defer is not the macro's last statement | `the 'defer' must be the last statement in the macro 'NAME'` |
| A defer inside the deferred statement of a macro | `a 'defer' in a macro cannot contain another 'defer'` |
| `defer_now` in a macro | `'defer_now' cannot be used in a macro` |
| The macro is used from another macro | `the macro 'NAME' contains a 'defer', so it must be used directly, not from another macro`, with a note at the `#define` |
| `DEFER` followed by a statement that comes from a macro | `the statement after 'DEFER' must be written out, not come from a macro` |
| A defer from a macro defined outside the niu file | `this 'defer' comes from a macro niu cannot find` |

Types without a name (see Lowering):

| Situation | Message |
|---|---|
| A function returning an unnamed type | `the return type of 'f' has no name, so niu cannot store the returned value while the defers run` |
| A statement expression with an unnamed type | `the value of this statement expression has a type without a name, so niu cannot store it while the defers run` |

niu also warns about code that is valid but probably a mistake:

| Situation | Warning |
|---|---|
| `defer_now` with no pending defers | `'defer_now' has no pending defers to run [-Wniu-empty-defer-now]` |

### Infer
TODO: declare a variable whose type comes from its initializer:
```c
infer a = add(1, 2);   /* a is an int */
```
niu writes the inferred type out in the generated C.

### Let
TODO: like `infer`, but the variable itself is `const`:
```c
let p = &x;            /* p is an int *const */
```

### Generic types
TODO: define a type template once and use it with different types:
```c
type_factory(my_t, T, Y, struct { int x; T one; Y two; });
my_t(int, char) a = {0};
```
Each distinct set of types becomes one generated struct.

### __mangle
TODO: `__mangle(T)` turns a type into a name fragment (Itanium-style encoding), for building identifiers per type, such as the names of generated structs and functions.

### Variable-length arrays
TODO: VLAs are allowed in niu code. niu allocates them on its own data stack instead of the C stack, so the generated C never contains a VLA.
