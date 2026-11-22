/* edits.c - how niu writes original code into a comment (the `#include
 * <niu.h>` line, a defer, defer_now). Part of niu.c; not compiled on its own.
 * The edits themselves are described in defer.c and rendered in lower.c. */

/* Appends text so that it can sit inside a block comment: a backslash goes
 * between every '/' and '*' that touch, so the text contains neither "*" "/"
 * nor "/" "*" (spec.md writes "*\/" and "/\*"). Line breaks are kept, so
 * the line numbers of everything after the comment stay the same. */
static void niu_append_comment_safe(niu_buf *out, const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        niu_buf_putc(out, s[i]);
        if (i + 1 < n && ((s[i] == '*' && s[i + 1] == '/') || (s[i] == '/' && s[i + 1] == '*')))
            niu_buf_putc(out, '\\');
    }
}

/* "/" "* <original text> *" "/": how niu turns a piece of code into a comment. */
static char *niu_comment_out(const char *s, size_t n)
{
    niu_buf b = {0};
    niu_buf_puts(&b, "/* ");
    niu_append_comment_safe(&b, s, n);
    niu_buf_puts(&b, " */");
    return niu_buf_take(&b);
}
