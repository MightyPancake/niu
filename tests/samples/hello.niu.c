/* A file named .niu.c: its lowered copy is called hello.c. */
#include <stdio.h>
#include <niu.h>

static int line_here(void) { return __LINE__; }

int main(void)
{
    /* The #line niu writes keeps __LINE__ and __FILE__ pointing at this
     * file. tests/run.sh compares the second line with the name each
     * compiler is expected to make of the #line name. */
    if (line_here() != 5)
        return 1;
    printf("ok\n%s\n", __FILE__);
    return 0;
}
