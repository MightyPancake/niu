/* fmtv.c - the %v conversion (spec.md "Automatic format specifier (%v)").
 * Portable C99: it must compile with gcc, clang, tcc and cl, and come out
 * the same on every target, so it avoids types whose size differs between
 * them (long, long double) and addresses other than a null pointer. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <niu.h>

/* a small limit, to see output cut (spec.md "Length limit") */
#define NIU_V_LIMIT 64

static int failures;

typedef enum color { RED, GREEN, BLUE, VERT = 1 } color;
typedef struct point {
    int x, y;
} point;
struct shape {
    char name[8];
    point at;
    color c;
    enum { SMALL, BIG } size;
    struct {
        int w, h;
    } box;
    unsigned bits : 3;
    _Bool flag : 1;
    int arr[3];
    unsigned char bytes[2];
    union bits {
        int i;
        _Bool b;
    } u;
};

static void check(const char *got, const char *want)
{
    if (strcmp(got, want) != 0) {
        printf("FAIL got [%s], want [%s]\n", got, want);
        failures++;
    }
}

/* a printf-style function, its attribute written through a macro (cl has
 * no __attribute__) */
#if defined(__GNUC__) || defined(__clang__)
#define PRINTF_LIKE(f, a) __attribute__((format(printf, f, a)))
#else
#define PRINTF_LIKE(f, a)
#endif
PRINTF_LIKE(2, 3) static void say(char *out, const char *fmt, ...);

int main(void)
{
    char buf[256];
    int i = -42, n;
    unsigned int u = 4000000000u;
    short sh = -7;
    long long ll = -9000000000LL;
    signed char sc = -56;
    unsigned char uc = 200;
    char a = 'a';
    _Bool yes = 1, no = 0;
    float f = 1.5f;
    double d = 3.14159, zero = 0.0;
    int *np = NULL;
    void (*fp)(void) = NULL;

    snprintf(buf, sizeof buf, "%v %v %v %v", i, u, sh, ll);
    check(buf, "-42 4000000000 -7 -9000000000");
    snprintf(buf, sizeof buf, "%v|%v|%v|%v", sc, uc, a, (char)'\'');
    check(buf, "-56|200|97 ('a')|39 ('\\'')");
    snprintf(buf, sizeof buf, "%v %v", yes, no);
    check(buf, "true false");
    snprintf(buf, sizeof buf, "%v %v %.4v %.v", f, d, d, d);
    check(buf, "1.50 3.14 3.1416 3");
    snprintf(buf, sizeof buf, "%v %v %v", 1.0 / zero, -1.0 / zero, zero / zero);
    check(buf, "inf -inf nan");
    snprintf(buf, sizeof buf, "%v %v", np, fp);
    check(buf, "0x0 0x0");
    /* width and '-' pad the whole output; a '*' precision is an argument */
    snprintf(buf, sizeof buf, "[%6v][%-6v][%*v][%.*v]", i, i, 5, yes, 3, d);
    check(buf, "[   -42][-42   ][ true][3.142]");
    /* other conversions, %%, and the return value */
    n = snprintf(buf, sizeof buf, "%d %%v %s %v", 1, "x", i);
    check(buf, "1 %v x -42");
    if (n != 10)
        check("wrong return value", "10");
    /* adjacent literals, a character constant (an int) */
    snprintf(buf, sizeof buf, "%" "v" " and %v", 'a', a);
    check(buf, "97 and 97 ('a')");
    /* a function declared with a format attribute */
    say(buf, "%v", d);
    check(buf, "3.14");
    /* output longer than the limit is cut; a number is never split */
    n = snprintf(buf, sizeof buf, "[%.70v] [%.20v]", d, 2.5);
    check(buf, "[...] [2.50000000000000000000]");
    if (n != 30)
        check("wrong return value", "30");
    /* structs, unions, enums, arrays */
    {
        point p = {1, -2};
        struct shape shp;
        color odd = (color)7;
        memset(&shp, 0, sizeof shp);
        memcpy(shp.name, "a\"b", 4);
        shp.at = p;
        shp.c = VERT;
        shp.size = BIG;
        shp.box.w = 3;
        shp.bits = 5;
        shp.flag = 1;
        shp.arr[1] = 7;
        shp.bytes[0] = 200;
        shp.u.i = 2;
        snprintf(buf, sizeof buf, "%v %v %v", p, odd, shp.c);
        check(buf, "(point){.x = 1, .y = -2} (color)7 GREEN");
        say(buf, "%v", shp);
        /* longer than the limit of 64: cut where it still keeps its shape */
        check(buf, "(struct shape){.name = \"a\\\"b\", .at = (point){.x = 1, ...}, ...}");
        say(buf, "%v", shp.u);
        check(buf, "(union bits){.i = 2, .b = (bool)2}");
    }
    /* a call built by a macro, with the format and the value written as its
     * arguments; through two macros */
#define LOG(...) snprintf(buf, sizeof buf, __VA_ARGS__)
#define LOG2(fmt, ...) LOG(fmt, __VA_ARGS__)
    LOG("got %v", d);
    check(buf, "got 3.14");
    LOG2("[%-4v] %.*v", i, 1, d);
    check(buf, "[-42 ] 3.1");
    /* statements that get braces */
    if (i < 0)
        snprintf(buf, sizeof buf, "%v", i);
    else
        snprintf(buf, sizeof buf, "positive");
    check(buf, "-42");
    for (n = 0; n < 2; n++)
        snprintf(buf, sizeof buf, "%v", n);
    check(buf, "1");

    if (failures == 0)
        printf("ok\n");
    return failures != 0;
}

static void say(char *out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out, 256, fmt, ap);
    va_end(ap);
}
