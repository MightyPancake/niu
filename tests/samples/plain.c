/* A plain C file: it does not include niu.h, so niu copies it unchanged.
 * Plain files may use niu's keywords as ordinary names. */
#include <stdio.h>

#define GREETING "ok"
#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))
#define SQUARE(x) \
	((x) * (x))

struct cleanup {
    int defer; /* a field named like a niu keyword */
};

static int sum(const int *v, size_t n)
{
    int total = 0;
    size_t i;
    for (i = 0; i < n; i++)
        total += v[i];
    return total;
}

int main(void)
{
    static const int values[] = {1, 2, 3, 4};
    struct cleanup c = {0};
    int defer = 0; // a C99 line comment
#if defined(__NIU_STD__)
    /* niu parses with __NIU_STD__ defined; the compiler of the output does not */
    defer = 1;
#endif
    c.defer = defer;
    if (sum(values, COUNT_OF(values)) != 10 || c.defer != 0 || SQUARE(3) != 9)
        return 1;
    printf("%s\n", GREETING);
    return 0;
}
