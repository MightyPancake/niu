# The Niu C Standard
The Niu (read as /ɲu/, which sounds like "new", but in Polish) is a C standard and dialect that introduces useful constructs like defer statements or generics containers into older C standards.
It is essentially a post-pre-processor which works after the standard C pre-processor does its thing, but before the actual process of compilation.
Because of this, Niu C (or a big part of it*) can be lowered to any C standard and works using all major C compilers.
It sits between an actual programming languages and a macro library.

## Features
### Defer statement
The defer statement is a mechanism that allows to queue a cleanup action that will happen whenever the given computation unit is done.
They are either bound to functions or scopes. Niu defer statements are bound to scopes which means that a defer statement will be fired on each exit of the scope it was defined in.

It is important to mention that the defer statement only gets applied to the exits of the scope that happen after a given defer statement has been declared. For example:
```c
void foo(){
    defer( a; );
    return;
    defer( b; );
}
```

will translate to:

```c
void foo(){
    /* defer( a; ); */
    { a; return;}
    /* defer( b; ); */
    a; b;
}
```

Notice how the first return statement *does not* feature the `b;` statement as the defer statement that introduced `b;` is placed after the return itself.

#### Implementation
The implementation is a plain and simple copying of the statement wherever a scope exit occurs.
For example:
```c
{
    int* ptr = (int*)malloc(16*sizeof(int));
    defer( free(ptr); );
    //use pointer
}
```

will be translated into this:

```c
{
    int* ptr = (int*)malloc(16*sizeof(int));
    /* defer( free(ptr); ); */
    //use pointer
    free(ptr);
}
```

There are 2 things worth noting in this example:
1. The deffered statement got pasted as the *last* statement of the block, so it will have to trigger only after everything else is done in it.
2. The original use of the defer macro got replaced with a comment. This is to ease up the proccess of going through the resulting lowered C code after the macro expansion (to make people reasoning about the code easier).

If there are multiple statements the defer queue acts in a LIFO (last in, first out) fassion.

```c
{
    defer( a; );
    defer( b; );
}
```

means

```c
{
    /* defer( a; ); */
    /* defer( b; ); */
    b;
    a;
}
```

The defer statement works with correctly with `break` and `continue` inside loops.

```c
int i = 0;
while(10 > i++){
    int* ptr = (int*)malloc(16*sizeof(int));
    int* ptr2 = (int*)malloc(16*sizeof(int));
    defer( { free(ptr); free(ptr2); } );
    if (i==8) break;
    if (i==5) continue;
    foo(ptr1, ptr2);
}
```

will be translated into

```c
int i = 0;
while(10 > i++){
    int* ptr = (int*)malloc(16*sizeof(int));
    int* ptr2 = (int*)malloc(16*sizeof(int));
    /* defer( { free(ptr); free(ptr2); } ); */
    if (i==8) {{ free(ptr); free(ptr2); } break;}
    if (i==5) {{ free(ptr); free(ptr2); } continue};
    foo(ptr1, ptr2);
    { free(ptr); free(ptr2); }
}
```

Important things to notice here:
1. The statement is copied as is and can be a body itself.
2. In this case, the defer statement has 3 real exits and all 3 are covered.
3. In the case of `continue` and `break`, they get turned into a block where the original keyword is put as the last one.

The defer statement also works with function returns. The following code:

```c
int foo(){
    void* bar = func_that_needs_cleanup();
    defer( cleanup(bar); );
    return use_bar(bar);
}
```

will be translated into:

```c
int foo(){
    void* bar = func_that_needs_cleanup();
    /* defer( cleanup(); ); */
    {int __niu_return_temp = use_bar; cleanup(bar)return __niu_return_temp;}
}
```
Once again the return statement is wrapped in a block, but this time the first statement is an automatically generated assignment of the exact return value to a niu temporary return value which is then returned.
Note that since the block is new for each return, the __niu_return_temp will never collide (unless __niue_return_temp is declared by hand, which will pick up as an error).