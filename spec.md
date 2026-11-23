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
- **Member names are not keywords.** Right after `.` or `->`, a word spelled like a niu keyword is a struct or union member, so a niu file can still use the members of plain structs: `o.defer`, `p->var` and the designated initializer `.var = 1` all work. Declaring such a member in a niu file is still an error, because there it is a keyword.
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
Status: **TODO** means the feature is not specified yet, **Draft** means its chapter is written but still has open questions, **Specified** means its chapter is written, and **Done** means it is specified and implemented.

| Feature | Status | Summary |
|---|---|---|
| [`defer`](#defer-statement) | [Done](#defer-statement) | Run a cleanup statement on every exit from the current block. Includes `defer_now`, which runs the pending cleanups early, right before `exit` or `abort`. |
| [`var`](#var-and-let) | [Done](#var-and-let) | Declare a variable whose type comes from its initializer. |
| [`let`](#var-and-let) | [Done](#var-and-let) | Like `var`, but the variable is `const`. |
| [`%v`](#automatic-format-specifier-v) | [Done](#automatic-format-specifier-v) | Print any value in a printf-style call, with a format generated from its type. |
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

`defer` and `_Defer` (the spelling used by the TS) mean the same thing. `defer_now` and `_Defer_now` run the pending defers early (see Semantics). Like every niu keyword, they only work in niu files (see Source files). In niu files they are keywords, so they cannot be used as names there (`int defer = 3;` is an error). The one exception is a member name right after `.` or `->` (`o.defer`), see Source files. Code that needs to declare such a name can stay in a plain C file.

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
   This applies to variables, functions, typedef names, tags and enum constants. It also applies to the names niu writes into the deferred statement itself, such as the type it writes for a `var` or `let` declaration (see "var and let"): that type must mean the same thing at every exit too.

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

### var and let
`var` declares a variable whose type comes from its initializer. `let` does the same, but the variable itself is `const`:
```c
var n = strlen(s);     /* n is a size_t */
var p = &x;            /* p is an int * */
let q = &x;            /* q is an int *const */
```
niu writes the type out in the generated C, so the output needs no type inference from the compiler and works with C99.

#### Syntax
```
var-declaration:
    attributes(opt) storage-class-specifiers(opt) var identifier attributes(opt) = assignment-expression ;
    attributes(opt) storage-class-specifiers(opt) let identifier attributes(opt) = assignment-expression ;
```
Attributes (`[[maybe_unused]]`, `__attribute__((unused))`) are kept as written (see Lowering for where they go).
- `var` and `let` take the place of the type in an ordinary declaration. The name after them is a plain identifier: the type, including any `*`, `[]` or `()`, comes from the initializer.
- One `var` or `let` declares one variable.
- A `var` or `let` declaration declares a variable, and can appear where a variable declaration can: at file scope, in a block, and as the first clause of a `for` statement (`for (var i = 0; i < n; i++)`). It cannot declare anything else (see constraint 8).
- Like every niu keyword, `var` and `let` only work in niu files, and in niu files they cannot be used as names (see Source files).

#### Constraints
A program that breaks any of these rules is rejected with an error.

1. **There must be an initializer, and it must be an expression.** `var x;` has no type to take, and a brace initializer (`var a = {1, 2};`) has no type of its own. A compound literal is an expression, so `var a = (struct point){1, 2};` is fine.
2. **One variable per declaration.** `var a = 1, b = 2.0;` is an error: write two declarations.
3. **The name is a plain identifier.** `var *p = &x;`, `var a[] = ...` and `var f(void) = ...` are errors.
4. **The variable cannot appear in its own initializer.** In `var x = x + 1;` the type of `x` is not known yet.
5. **The type must be one niu can write.** niu writes the type out in C99, so a type without a name (an unnamed `struct`, `union` or `enum`) is an error. So is a variably modified type, such as a pointer to a variable-length array (`int (*)[n]`), because the output contains no VLAs. (The variable itself can never be an array: an array initializer becomes a pointer, see Semantics.)
6. **Only storage-class specifiers and attributes can come with `var` or `let`.** The storage classes are `static`, `_Thread_local` (also spelled `thread_local` or `__thread`) and `register`. (`auto` is left out: C23 uses it for type inference too, which would only confuse.) `extern` and `typedef` are errors, since neither declares a variable with an initializer. Type specifiers and qualifiers are errors too (`const var x = 1;`): use `let` for a `const` variable. `_Alignas` is an error as well, because the alignment belongs to the type, and the type comes from the initializer: write the type out instead.
7. **`var` and `let` cannot be used in a macro.** The type would differ at every use of the macro, and niu leaves macro uses as written. Ordinary macros inside a `var` declaration are fine: `var n = ARRAY_LEN(items);` takes the type of the expanded value. So is a `var` or `let` written in a macro *argument*, because that text is in the file and niu can lower it there: `TRY(var x = f());` becomes `TRY(int x = f());`. The macro must put the argument where a variable declaration can be (constraint 8).
8. **`var` and `let` only declare variables.** They cannot declare a function parameter (`void f(var x)`), a struct or union member, or a function, and they cannot appear in a cast, `sizeof`, `_Alignof`, a compound literal's type or a function's return type.
9. **The type must have a name that is visible where the variable is declared.** A declaration in an inner scope can hide a type's name there (see "Writing the type" under Semantics). If none of the type's names is visible, C99 has no way to write that type at that place, so it is an error:
   ```c
   struct S { int a; };
   struct S *get_s(void);
   void f(void) {
       struct S { double d; };   /* hides the outer struct S */
       var p = get_s();          /* error: 'struct S' means a different type here */
   }
   ```
   To fix it, give the outer type a typedef name at file scope, or rename one of the structs.

   The same holds for a type whose name is not hidden but out of scope, such as a struct declared inside a statement expression: `var v = ({ struct L { int a; } t = {1}; t; });` is an error, because `struct L` does not exist after the `)`.

#### Semantics
**The type is the type of the initializer, as a value.** It is the type the initializer has after the usual conversions that happen when a value is read:
- an array becomes a pointer to its first element: `var s = "abc";` makes `s` a `char *`, and `var e = arr;` makes `e` an `int *` when `arr` is an `int[4]`;
- a function becomes a pointer to the function: `var cb = handler;` makes `cb` a `void (*)(int)`;
- qualifiers of the value itself are dropped: if `ci` is a `const int`, `var b = ci;` makes `b` a plain `int`. Qualifiers further in are kept: `var p = &ci;` makes `p` a `const int *`.

**`let` adds `const` to the variable itself.** `let p = &x;` makes `p` an `int *const`: `p` cannot change, but `*p` can. `let s = "abc";` makes `s` a `char *const`, and `let n = strlen(s);` makes `n` a `const size_t`.

**The usual C rules decide the type, for the standard niu is run with.** `var` adds no rules of its own:
- `var c = 'a';` makes `c` an `int`, since a character constant is an `int` in C. So does a comparison or a logical operator: `var b = x < y;` makes `b` an `int`.
- Integer promotion applies: if `a` and `b` are `uint8_t`, `var s = a + b;` makes `s` an `int`.
- `var t = true;` makes `t` an `int` under C99 to C17, where `true` is a macro for `1`, and a `bool` under C23, where it is a keyword.
- `_Atomic` is a qualifier of the value too, so `var v = counter;` makes `v` a plain, non-atomic variable when `counter` is `_Atomic int`.

**Typedef names are kept.** When the type of the initializer *is* a typedef name, the variable gets that name: `var n = strlen(s);` makes `n` a `size_t`, not `unsigned long`. This also holds through arithmetic and `?:` when the operands have that type: with two `size_t` operands, `a + b` and `k ? a : b` are `size_t`. A `var` or `let` variable counts with the type niu wrote for it, so after `var n = strlen(s);`, `var m = n + a;` is a `size_t` too. When the type has no typedef name, niu writes the underlying type, which can differ between targets (`size_t` is `unsigned long` on 64-bit Linux, but `unsigned long long` on 64-bit Windows). Like all niu output, the result is for one configuration (see "The output is for one configuration" under Source files).

**Writing the type.** niu writes the type with a name that means that type where the variable is declared. A declaration in an inner scope can hide a name: a local variable `S` hides the typedef `S`, and an inner `struct S` hides an outer one. niu tries the names of the type in this order, and uses the first one that is visible:
1. the typedef name the type has, such as `S` or `size_t`;
2. its tag, such as `struct S`;
3. for a `struct`, `union` or `enum`, another typedef name for the same type that is visible there: the innermost local one, else the first one at file scope;
4. for a typedef of a type that is not a `struct`, `union` or `enum`, the underlying type, such as `unsigned long` for `size_t`.

Step 3 is only for `struct`, `union` and `enum` types. Any other type has step 4 instead, which is predictable; step 3 would pick whichever typedef of the same type (`unsigned long`, say) the headers happen to declare first, often a name internal to the C library.

If none of them is visible, constraint 9 makes it an error.

**Later (TODO):** C23 and GNU C have `typeof`, which can name the type of any expression. When niu is run with `-std=c23` or a GNU standard, it could write `typeof(get_s()) p = get_s();` instead of reporting constraint 9, because the initializer's type can then always be written. That is left out for now. Adding it later only accepts programs that were errors before, so it breaks no existing code.
```c
typedef struct S { int a; } S;
S get_s(void);
void f(void) {
    int S = 0;           /* hides the typedef S */
    var p = get_s();     /* niu writes: struct S p = get_s(); */
}
```

**Everything else is an ordinary declaration.** The variable is initialized, scoped and stored exactly as if its type had been written out. Assigning to a `let` variable is an error from the C compiler, the same as for any `const` variable.

#### Lowering
`var` or `let` and the name are replaced by the written-out declaration. Everything else, including the initializer, stays as written:
```c
var n = strlen(s);
let p = &x;
static var count = 0;
for (var i = 0; i < n; i++) { }
```
becomes
```c
size_t n = strlen(s);
int *const p = &x;
static int count = 0;
for (int i = 0; i < n; i++) { }
```
The `0` in the `for` loop is an `int`, so `i` is an `int` too, and a compiler warns about comparing it with the `size_t` `n` (`-Wsign-compare`). Write `var i = (size_t)0` to get a `size_t`.

When the type is a pointer to a function or to an array, the name goes inside the declarator:
```c
var cb = handler;      /* void handler(int); */
var row = &grid[0];    /* int grid[8][8]; */
```
becomes
```c
void (*cb)(int) = handler;
int (*row)[8] = &grid[0];
```
For `let`, the `const` goes inside the declarator too, next to the name, so that the pointer itself is `const`:
```c
let cb = handler;      /* void (*const cb)(int) = handler; */
let row = &grid[0];    /* int (*const row)[8] = &grid[0]; */
```
When the type is not a pointer, the `const` goes before the type: `let n = strlen(s);` becomes `const size_t n = strlen(s);`.

Attributes after the name stay where they are, except when the name goes inside the declarator. Then a `[[...]]` attribute goes inside too, right after the name, because after the `)` it would apply to the type; a GNU `__attribute__((...))` cannot go there and stays after the declarator:
```c
let cb [[maybe_unused]] = handler;          /* void (*const cb [[maybe_unused]])(int) = handler; */
var row __attribute__((unused)) = &grid[0]; /* int (*row)[8] __attribute__((unused)) = &grid[0]; */
```
The written-out declaration stays on the line of the original, so line numbers and `__LINE__` do not change. Only the columns after it on that line move.

#### Editor tools
Under `__NIU_TOOLING__`, `niu.h` defines `var` as `__auto_type` and `let` as `__auto_type const`. These stubs are only for editor tools: the generated C never contains `__auto_type`, because niu writes the type out.
- C99 has no way to infer a type, so the stubs need an extension. Tools based on Clang or GCC (clangd, for example) understand `__auto_type` and show the inferred types. Tools that do not know it, such as MSVC IntelliSense, report errors on `var` and `let` lines.
- With `-pedantic`, Clang warns that `__auto_type` is a GNU extension. In tooling mode, `niu.h` turns that warning off with `#pragma clang diagnostic ignored "-Wgnu-auto-type"`, only when `__clang__` is defined. GCC does not give this warning.
- `__auto_type` accepts more than niu does: several variables in one declaration, unnamed types and variable-length array types. Editor tools do not flag those, but niu rejects them.

#### Diagnostics
| Constraint | Message |
|---|---|
| 1 | `'var' requires an initializer`, `'var' cannot take a brace initializer; write the type instead` |
| 2 | `'var' declares a single variable` |
| 3 | `the name after 'var' must be a plain identifier; its type comes from the initializer` |
| 4 | `'x' cannot appear in its own initializer` |
| 5 | `the type of 'x' has no name, so niu cannot write it out`, `the type of 'x' is a variably modified type, which niu cannot write out` |
| 6 | `'var' cannot be used with 'extern'`, `'var' cannot be used with 'typedef'`, `'var' cannot be used with 'auto'`, `'var' takes the place of the type; use 'let' for a 'const' variable` (for `let`: `'let' takes the place of the type; 'let' already makes the variable 'const'`), `'var' cannot be used with '_Alignas'; write the type out instead` |
| 7 | `'var' cannot be used in a macro` |
| 8 | `'var' can only declare a variable` |
| 9 | `the type of 'p' cannot be written here: 'struct S' means a different type in this scope`, with a note `'struct S' is declared here` at the declaration that hides it (no note when the name is only out of scope) |

Each message names the keyword that was used (`var` or `let`), and `_Alignas` as it was spelled (`alignas`). An error about the declaration as a whole (constraints 1 without an initializer, 5, 7, 8 and 9) is reported at the keyword; one about a single token (the `{`, the second variable, the `*`, the use of the variable, the specifier) at that token. When the type had several names to try, constraint 9 names the first one tried.

Wrong uses of the keywords themselves:

| Situation | Message |
|---|---|
| A keyword used as a name (`int var = 3;`) | `'var' is a keyword in niu files and cannot be used as a name` |

### Automatic format specifier (%v)
In a call to a `printf`-style function, the `%v` conversion prints its argument in a form niu generates from the argument's type. For example:

```c
typedef struct my_struct {
    int a;
    char b[10];
} my_struct;

void foo(void) {
    my_struct s = (my_struct){.a = 42};
    strcpy(s.b, "hello");
    printf("%v\n", s);
}
```

prints `(my_struct){.a = 42, .b = "hello"}`.

#### Constraints
A program that breaks any of these rules is rejected with an error.

1. **`%v` only works where niu can see the type of every variadic argument.** That means calls to the standard `printf` family that take `...`: `printf`, `fprintf`, `sprintf` and `snprintf`, and the bounds-checked `printf_s`, `fprintf_s`, `sprintf_s` and `snprintf_s` from C11 Annex K, wherever the C library declares them (MSVC has most of them, glibc has none). niu knows these by name, when they are declared in a system header, because some C libraries (MSVC's among them) do not mark them with format attributes. It also covers calls to any function declared with `__attribute__((format(printf, m, n)))` where `n` is not 0, that is, a function that takes `...`. The attribute can be written through a macro, as portable code does for compilers without `__attribute__` (`#define PRINTF_LIKE(f, a) __attribute__((format(printf, f, a)))`). A `%v` in the format string of a function that takes a `va_list` (`vprintf`, or `format(printf, m, 0)`) or of a `scanf`-style function (`format(scanf, ...)`) is an error.
2. **The format string must be a string literal.** Adjacent literals (`"x = " "%v"`) count as one. As in printf, `%%` is a literal percent sign, so `"%%v"` prints `%v` and is left as it is. niu cannot see a `%v` in a format string built at run time. The C library would get an unknown conversion there, which is undefined behavior, so don't build one.
3. **Every `%v` needs an argument.** A `%v` with no matching argument is an error. A `*` width or precision uses up an argument too, as in printf, so `printf("%*v", w, s)` prints `s` with width `w`, and `printf("%*v", s)` is an error.
4. **The argument must have a type `%v` can print.** Those are the types listed under Output. `_Complex` types are an error. So are `_Atomic` structs and unions, at any depth, because C does not allow reading the members of one, and types nested more than 30 levels deep (structs, arrays and strings inside one another), which is more than the runtime keeps track of.
5. **A struct, union or enum argument must have a type declared at file scope, with a name.** niu prints it with a function at file scope (see Lowering), which has to name the type as a parameter. A type declared inside a function, or an unnamed type without a typedef name, is an error. This applies only to the argument's own type: its members can have unnamed types.
6. **`%v` cannot come from a macro definition.** niu leaves macros as written, so it cannot rewrite a `%v` that is in the body of a `#define` (`#define FMT "%v\n"`), or an argument that is. A call built by a macro is fine when the format literal and the argument are written in the file, as macro arguments, and each of them appears once in the expansion. That covers the usual logging macro:
   ```c
   #define LOG(...) fprintf(stderr, __VA_ARGS__)
   LOG("got %v\n", s);   /* fine: "got %v\n" and s are both in the file */
   ```
   A macro that uses such an argument twice, or turns it into a string with `#`, is an error. So is a format string that the macro builds from its argument and text of its own (`#define LOG2(fmt, ...) LOG("[log] " fmt, __VA_ARGS__)`): part of the literal is in the definition.
7. **A `%v` can only have a width, the `-` flag and a precision.** The form is `%[-][width][.precision]v`, where the width and the precision are each a number or `*`: `%20v`, `%-*v`, `%.4v`, `%-20.*v`. The other flags (`+`, space, `#`, `0`), length modifiers (`hh`, `l`, `z`, `L` and the rest, which have no use because niu knows the type) and positional arguments (`%1$v`) are errors. Allowing some of them later only accepts programs that were errors before, so it breaks no existing code.
8. **`<stdio.h>` must be included before the first function that uses `%v`.** The generated functions format values with `snprintf` (see Lowering), and they go right before that function. A file that calls `printf` includes `<stdio.h>` anyway, but one that only uses `%v` with its own printf-style function might not.

#### Output
The output depends on the type of the argument as niu sees it before the default argument promotions, so a `char`, `bool` or enum argument still counts as that type and not as the `int` it is promoted to. The same rules apply at every depth, to the members of a struct and the elements of an array.

- **Integers** print in decimal: `42`, `-7`.
- **`char`, `signed char` and `unsigned char`** print as a number, followed by the character in quotes and parentheses when it is a visible ASCII character (`0x21` to `0x7E`): `97 ('a')`, but `10` for a newline. The number is the value of the type, so the byte `0xC8` prints as `200` for an `unsigned char` and as `-56` for a `signed char`, and for a plain `char` it depends on whether `char` is signed on the target (it is on x86, so `-56` there).
- **`bool` (`_Bool`)** prints as `true` or `false`. niu reads a `bool` through its bytes, as an `unsigned char`, because a `bool` whose byte is not 0 or 1 is undefined behavior to read, and compilers assume it never happens. That byte can be anything in a union member that is not the active one, or in a member that was never set. 0 prints as `false`, 1 as `true`, and any other value as a cast: `(bool)2`.
- **Floating types** (`float`, `double`, `long double`) print in fixed notation with 2 decimal places by default: `3.14`. A precision before the conversion letter overrides that for every floating value the `%v` prints, at any depth: `%.4v` prints `3.1416`. The precision applies only to floating values. As in printf, a `.` with no number means a precision of 0: `%.v` prints `3`. Infinity prints as `inf` or `-inf`, and a NaN as `nan`. niu prints these itself instead of leaving them to the C library, which spells them differently on different targets (`-nan(ind)` on Windows, for example). With MinGW, whose default `snprintf` (from the Microsoft C library) cannot print a `long double`, the generated code calls MinGW's own `__mingw_snprintf` instead, which `<stdio.h>` always declares there.
- **Pointers**, including `char *` and pointers to functions, are never followed. They print as `0x` followed by the address in lowercase hex: `0x7ffd5e8c1a40`, and `0x0` for a null pointer. The address is converted with a cast to `size_t` and then to `unsigned long long`, which C allows for every pointer type, including pointers to functions, which cannot be converted to `void *`. `size_t` has the size of a pointer on every target niu supports, so compilers do not warn about a cast to a smaller integer, and it needs no header but `<stdio.h>`, unlike `uintptr_t`.
- **Enums** print the name of the enumerator with the stored value:
  ```c
  typedef enum my_enum {
      my_value_0,
      my_value_1,
      my_value_2,
      my_value_3
  } my_enum;

  void foo(void) {
      my_enum e = my_value_2;
      printf("%v\n", e);   /* prints my_value_2, not 2 */
  }
  ```
  When no enumerator has the stored value, the value prints as a cast: `(my_enum)7`. When several enumerators share the value, the first one declared is printed.

  An enumerator written directly as the argument (`printf("%v\n", my_value_2);`) prints `2`, because in C an enumerator constant has type `int`, not the enum type. For the same reason a character constant prints as a number (`printf("%v", 'a')` prints `97`, while a `char` variable holding it prints `97 ('a')`), and `printf("%v", true)` prints `1` under C99 to C17, where `true` is a macro for `1`.
- **Structs** print the name of the type in parentheses, followed by every member in curly braces, in declaration order: `(my_struct){.a = 42, .b = "hello"}`. A bit-field prints like a value of its declared type, so a `bool` bit-field prints as `true` or `false` and an enum bit-field as an enumerator.
- **Unions** print the same way, with every member, as if each were the active one: `(my_union){.i = 42, .f = 0.00}`. C99 allows reading any member of a union, so this is well defined, but only the active member's value is meaningful. Pointers are never followed anyway, so a member that is not the active one cannot crash.
- **Arrays** that are struct or union members print their elements in curly braces: `{1, 2, 3}`, and `{{1, 2}, {3, 4}}` for an array of arrays. A flexible array member has no known length and prints as `{...}`. An array passed directly as the argument has already become a pointer, so it prints as an address.
- **`char` arrays** print as a string in double quotes, up to the first null character or the end of the array, whichever comes first, so an array without a terminating null never reads past its end. Printable ASCII (`0x20` to `0x7E`, the space included) is printed as it is, `\"` and `\\` escaped; the rest is escaped as in C: `\n`, `\t`, and `\x` with two hex digits (`\x01`). Arrays of `signed char` and `unsigned char` (such as `uint8_t`) are data, not text, so they print as arrays of plain numbers, without the character: `{200, 0}`.

**A width pads the whole output**, the same as for `%s`: with spaces on the left, or on the right with `-`. `printf("[%-24v]", s)` prints `[(my_struct){.a = 42, .b = "hello"}]` unchanged, since it is longer than 24 bytes, and `printf("[%6v]", e)` with `int e = 42` prints `[    42]`. The width counts bytes. With `*` it comes from an `int` argument before the value, as in printf.

**The name of a type** is chosen by the same steps as for `var` (see "Writing the type" under `var` and `let`), with visibility taken at file scope, where the generated function is (see Lowering). The name is the typedef name the type has, else its tag (`(struct S){...}`, `(enum E)7`), else another typedef name for the same type at file scope. Every call that prints a type therefore prints the same name, even where a local declaration hides that name.

**Unnamed types** (an unnamed `struct`, `union` or `enum`) print the keyword in place of the name, without parentheses: `struct{.a = 1}`, `union{.i = 42, .f = 0.00}`, and `enum 7` for a value with no enumerator. Parentheses only ever surround a name. An anonymous struct or union member prints the same way in its place among the members: `(S){.a = 1, struct{.x = 2, .y = 3}}`.

#### Length limit
**The output of one `%v` is at most `NIU_V_LIMIT` bytes, including the terminating null.** The default is 2048. Change it when compiling the generated C, with `-DNIU_V_LIMIT=4096` on the command line or a `#define NIU_V_LIMIT 4096` before the first function in the file that uses `%v`. It must be a plain integer constant of at least 64, because the generated C uses it in `#if` and as an array size. The limit is per `%v`, not per call: `printf("%v %v", a, b)` can use twice as much stack. A buffer lives until the end of its block (see Lowering), so niu puts each call that is a whole statement in a block of its own, and the compiler can reuse the space for the next one. A call that is part of a declaration, such as `int n = printf("%v", s);`, keeps its buffer until the end of the enclosing block. A smaller limit helps on targets with little stack and in deeply recursive code.

**Most output never reaches the limit.** niu knows the longest output each type can produce, for the configuration it is run with: integers, pointers and enum names have a fixed maximum length, a `char` array of length n takes at most 4n + 2 bytes (every character escaped as `\xNN`, plus the quotes), and so on. A `%v` whose longest output fits uses a buffer of exactly that size, so it is never cut, and a small type uses a small buffer however large the limit is. Only types whose longest output is larger than the limit can be cut, for example a struct with a `char name[1024]` member, a `long double` (whose largest value has over 4900 digits in fixed notation on x86), or a `%.*v` precision, which niu cannot know in advance.

**Output that does not fit is cut, and the cut is marked.** Everything that fits is printed. At the cut, niu writes `...` and then closes everything still open, so the output keeps its shape:
- an open string is closed with `..."`;
- each open pair of braces, from the innermost one out, is closed with `, ...}`, or `...}` when nothing has been written inside it yet.

```
(my_struct){.a = 42, .b = "hel...", ...}
(outer){.id = 7, .inner = (my_struct){.a = 42, ...}, ...}
```

A number, an enum name, a `true` or `false`, an address or an escape sequence is never split: if it does not fit, it is left out and the cut comes before it. A number cut in half would read as a different, smaller number. Room for the closing text is kept free from the start, so the closing text always fits. When even the opening of the value does not fit (a type nested so deeply that the closing text alone is larger than the limit), the output is only `...`.

Output that fits is printed exactly as described under Output, with no `...`. Because the cut output is still an ordinary string, the rest of the format string, the return value of the call and the truncation of `snprintf` all work as usual.

#### Lowering
Each `%v` in the format string becomes `%s`, and its argument becomes a call to a function niu generates for the argument's type, which formats the value into a buffer and returns the buffer:
```c
printf("x = %v\n", s);
```
becomes
```c
printf("x = %s\n", __niu_v_t_my_struct((char[(N) < NIU_V_LIMIT ? (N) : NIU_V_LIMIT]){0}, (N) < NIU_V_LIMIT ? (N) : NIU_V_LIMIT, 2, s));
```
where niu writes the longest output of the type, plus one, as the number in place of `N`. The arguments of `__niu_v_t_my_struct` are the buffer, its size, the precision for floating values (2, or the precision given as `%.4v`) and the value.
- **The buffer is a compound literal.** In C99 it lives until the end of the enclosing block, so it is still valid while `printf` reads it. Its size is the smaller of the longest output and `NIU_V_LIMIT`, an integer constant expression, so the compound literal is an ordinary array and not a variable-length one.
- **It stays one call to the same function.** The other arguments, the order they are evaluated in and the return value are unchanged, and every argument is still evaluated once.
- **A call that is a whole statement gets braces.** A compound literal lives until the end of the block it is in, and the compiler cannot reuse its space while it lives, since the called function might have kept the pointer. Ten `%v` calls in one function body would hold ten buffers at once. So niu wraps the statement in `{ }`, on the same line: `printf("%v\n", s);` becomes `{ printf("%s\n", __niu_v_t_my_struct(...)); }`, and `if (x) printf("%v", s);` becomes `if (x) { printf("%s", ...); }`, which means the same. A call inside a larger expression is wrapped as the whole statement: `n += printf("%v", s);` becomes `{ n += printf(...); }`. A declaration is never wrapped, because braces would change the scope of what it declares. Neither is the last statement of a GNU statement expression (`({ ...; printf("%v", s); })`), because its value is the value of the whole expression.
- **`%.*v`** takes its precision from an `int` argument, which moves into the call to the generated function: `printf("%.*v", p, s)` becomes `printf("%s", __niu_v_t_my_struct((char[NIU_V_LIMIT]){0}, NIU_V_LIMIT, p, s))`. The buffer is always `NIU_V_LIMIT` bytes, because the longest output depends on `p`.
- **A width and the `-` flag stay on the `%s`**, so the C library pads the output and niu does not have to: `%-20v` becomes `%-20s`, and `%*v` becomes `%*s`, with the width argument left where it is. The precision is never put on the `%s`, where it would cut the string: it goes to the generated function instead. So `printf("%-*.*v", w, p, s)` becomes `printf("%-*s", w, __niu_v_t_my_struct((char[NIU_V_LIMIT]){0}, NIU_V_LIMIT, p, s))`.
- **Arguments of a type without members** (integers, `char`, `bool`, floating types, pointers) use generated functions in the same way, named after the type: `__niu_v_int`, `__niu_v_double`, `__niu_v_ptr`.

**The generated functions are `static inline` and go at file scope**, before the first function in the output file that uses them. (`inline`, because some compilers warn about a `static` function that is not used.) There is one per type of a `%v` argument per output file, `__niu_v_<name>`, and one that writes each named struct, union and enum into the buffer, `__niu_vp_<name>`, which the others call for their members. Members of an unnamed type, and arrays, are written inline. The functions they share (the runtime: writing into the buffer, cutting the output, printing a number) come with them, each only when a generated function in the file uses it. The first one in each output file comes after the default limit:
```c
#ifndef NIU_V_LIMIT
#define NIU_V_LIMIT 2048
#endif
#if NIU_V_LIMIT < 64
#error "NIU_V_LIMIT must be at least 64"
#endif
```
A `#line` directive after the generated code keeps the line numbers of the function that follows. **Names.** A generated function is named after the type, with the kind of tag in the name so that `struct S` and a typedef `S` of a different type do not collide, and with `t_` before a typedef name so that a typedef named `ptr` or `int8` does not collide with niu's own names:

| Type | Function |
|---|---|
| `struct S`, `union U`, `enum E` | `__niu_v_struct_S`, `__niu_v_union_U`, `__niu_v_enum_E` |
| a typedef name `T` (of a struct, union or enum) | `__niu_v_t_T` |
| a type without members | `__niu_v_int`, `__niu_v_unsigned_long_long`, `__niu_v_long_double`, `__niu_v_bool`, `__niu_v_ptr` |

The runtime's functions start with `__niu_vr_`.

A typedef of a type without members, such as `size_t`, uses the function of the underlying type.

**Guards.** A header can be included by several niu files, and a file can print a type that a header it includes already prints. So in every output file, each generated function, and each piece of the runtime, is wrapped in `#ifndef __niu_v_<name>_defined` and `#define __niu_v_<name>_defined` (`__niu_vp_`, `__niu_vr_` for the others), and a translation unit never gets the same function twice. The generated functions call only `snprintf` (`__mingw_snprintf` with MinGW), so the output stays C99 and needs no niu runtime and no header but `<stdio.h>`. They read the bytes of a `bool` through an `unsigned char` lvalue, which C allows for any object, so they do not need `memcpy` either.

#### Editor tools
Clang checks the format strings of printf-style functions, and `%v` is not a conversion it knows, so it would report `invalid conversion specifier 'v'` [`-Wformat-invalid-specifier`] on every `%v`, and `data argument not used by format string` [`-Wformat-extra-args`] for its argument.
- **niu turns these warnings off while it reads a niu file**, and checks each `%v` itself (constraints 1 to 8). The rest of the format string is still checked, by the C compiler on the lowered call: there the `%v` is a `%s` with a `char *` argument, so `-Wformat` checks every other conversion as usual, and `#line` points its warnings at the original.
- **Under `__NIU_TOOLING__`, `niu.h` turns them off too**, with `#pragma clang diagnostic ignored "-Wformat-invalid-specifier"` and `"-Wformat-extra-args"`, only when `__clang__` is defined. Editors based on Clang then show no warning for `%v` itself. The pragmas cover the rest of the translation unit, so in the editor they also hide invalid conversions and unused arguments in other format strings; the build still reports those. GCC has no separate warnings for this, so with GCC-based tools the warnings stay.
- **A conversion after a `%v` can get a false warning in the editor.** Clang does not take an argument for a conversion it does not know, so it checks each later conversion against the argument before it: in `printf("%v %d", 1.5, 2)` the editor says `%d` gets a `double`. The build is not affected, because niu and the C compiler see the right arguments.

#### Diagnostics
| Constraint | Message |
|---|---|
| 1 | `'%v' cannot be used with 'vprintf': the types of a 'va_list' are not known`, `'%v' cannot be used in a scanf format string` |
| 3 | `'%v' has no matching argument` |
| 4 | `'%v' cannot print a value of type '_Complex double'`, also for a member's type; `'%v' cannot print a type nested more than 30 levels deep` |
| 5 | `'%v' cannot print 'struct L': it is declared inside a function`, `'%v' cannot print a value of an unnamed type; give it a typedef name` |
| 6 | `'%v' cannot be used in a macro definition`, `'%v' cannot be used here: the macro 'LOG' uses its argument more than once`, `'%v' cannot be used here: the macro 'LOG' turns its argument into a string`, each with a note at the macro's definition |
| 7 | `'%v' cannot take the '+' flag; only a width, '-' and a precision are allowed`, `'%v' cannot take a length modifier: the type of the argument is already known`, `'%v' cannot take a positional argument` |
| 8 | `'%v' needs <stdio.h>; include it before 'foo'`, with a note at the function |

Each message names the function that was called and the type as it is written in the source. The error is reported at the `%v` in the format string, with a note at the argument for constraints 4 and 5, a note at the macro for constraint 6, and a note at the function for constraint 8. Constraint 2 has no diagnostic, because niu cannot see a format string built at run time.

**`%v` passed to an unknown function (warning, `-Wniu-v-no-format`).** A string literal with a `%v` in it, passed as an argument to a variadic function that constraint 1 does not cover, is left unchanged. If the function hands it to the C library as a format string, that is undefined behavior at run time, so niu warns:
```
main.c:8:12: warning: '%v' in a string passed to 'my_log', which niu does not know as a printf-style function [-Wniu-v-no-format]
main.c:3:6: note: add '__attribute__((format(printf, 1, 2)))' to 'my_log' to use '%v' with it
```
The note gives the positions of the literal and of the `...` in that function. Functions that are not variadic never get this warning, so `puts("%v")` prints `%v` without complaint.

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
