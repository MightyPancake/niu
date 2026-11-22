/* defer with GNU C: statement expressions, and `return f();` in a void
 * function (spec.md "Lowering"). gcc, clang and tcc only: cl has neither. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <niu.h>

static char log_[64];
/* (no strcat: cl -W4 -WX rejects it as deprecated) */
static void L(const char *s)
{
    size_t n = strlen(log_);
    while (*s && n + 1 < sizeof log_)
        log_[n++] = *s++;
    log_[n] = '\0';
}
static int check(const char *want)
{
    int ok = strcmp(log_, want) == 0;
    if (!ok)
        printf("expected \"%s\", got \"%s\"\n", want, log_);
    log_[0] = '\0';
    return ok;
}

static char *get(void)
{
    char *s = (char *)malloc(4);
    memcpy(s, "abc", 4); /* (no strcpy: the MSVC headers deprecate it) */
    return s;
}

/* spec.md, as written there */
static size_t spec_value(void)
{
    size_t n = ({ char *s = get(); defer free(s); strlen(s); });
    return n;
}

static int value(void)
{
    int v = ({ defer L("s"); L("t"); 7; }); /* t, then s, then v = 7 */
    return v;
}

static void no_value(void)
{
    ({ defer L("b"); L("a"); (void)0; });
}

static void callee(void) { L("c"); }

static void void_return(void)
{
    defer L("d");
    return callee(); /* c, then d */
}

int main(void)
{
    int ok = 1;
    ok &= spec_value() == 3;
    ok &= value() == 7 && check("ts");
    no_value();
    ok &= check("ab");
    void_return();
    ok &= check("cd");
    printf("%s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
