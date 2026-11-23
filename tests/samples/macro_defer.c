/* A defer inside a macro definition. niu keeps every macro use as written:
 * it comments out the defer part of the #define and adds a companion macro
 * __niu_defer_<NAME> with the deferred statement, which the copies use. */
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

#define AUTOFREE(p) defer free(p)
#define DEFER defer
#define WITH_LOG(s) L("+" s); defer L("-" s)
#define CLEANUP defer L("k")
#define END }
#define UNUSED(x) defer x
#define ID(x) (x)
#define ZERO 0

static int f(int ok)
{
    char *buf = (char *)malloc(8);
    AUTOFREE(buf);
    DEFER L("d");
    WITH_LOG("w");
    CLEANUP;
    if (!ok)
        return -1; /* k -w d, free(buf) */
    L("=");
    return 0;
}

static void g(void)
{
    WITH_LOG("g");
    L("=");
END

/* a returned value that ends in a macro use */
static int h(int v)
{
    DEFER L("h");
    if (v)
        return ID(v) * ID(2);
    return ZERO;
}

int main(void)
{
    int ok = 1;
    ok &= f(0) == -1 && check("+wk-wd");
    ok &= f(1) == 0 && check("+w=k-wd");
    g();
    ok &= check("+g=-g");
    ok &= h(3) == 6 && check("h");
    ok &= h(0) == 0 && check("h");
    printf("%s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
