/* CRLF line endings must survive the round trip, also in the copies of
 * a deferred statement that spans several lines. */
#include <stdio.h>
#include \
    <niu.h>

static int done;

static int run(void)
{
    defer {
        done++;
        done *= 10;
    }
    return done;
}

int main(void)
{
    if (run() != 0 || done != 10)
        return 1;
    printf("ok\n");
    return 0;
}
