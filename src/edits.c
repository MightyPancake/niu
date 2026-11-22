/* edits.c - the edit list: replacements of byte ranges in an original file,
 * applied to its text in one pass. Everything outside the edited ranges is
 * copied byte for byte, so line endings, comments, macros and #includes
 * stay exactly as written. Part of niu.c; not compiled on its own. */

typedef struct {
    size_t begin; /* byte offset in the original file */
    size_t end;   /* one past the last replaced byte; begin == end inserts */
    char *text;   /* replacement, owned */
} niu_edit;

typedef struct {
    niu_edit *items;
    size_t count;
    size_t cap;
} niu_edits;

static void niu_edits_add(niu_edits *e, size_t begin, size_t end, char *text)
{
    if (e->count == e->cap) {
        e->cap = e->cap ? e->cap * 2 : 16;
        e->items = (niu_edit *)niu_xrealloc(e->items, e->cap * sizeof *e->items);
    }
    e->items[e->count].begin = begin;
    e->items[e->count].end = end;
    e->items[e->count].text = text;
    e->count++;
}

static void niu_edits_free(niu_edits *e)
{
    size_t i;
    for (i = 0; i < e->count; i++)
        free(e->items[i].text);
    free(e->items);
    e->items = NULL;
    e->count = e->cap = 0;
}

static int niu_edit_cmp(const void *a, const void *b)
{
    const niu_edit *x = (const niu_edit *)a, *y = (const niu_edit *)b;
    if (x->begin != y->begin)
        return x->begin < y->begin ? -1 : 1;
    if (x->end != y->end)
        return x->end < y->end ? -1 : 1;
    return 0;
}

/* Appends src with the edits applied to out. Returns 0 (and appends
 * nothing) when edits overlap or leave the text: that is a bug in niu. */
static int niu_edits_apply(niu_edits *e, const char *src, size_t len, niu_buf *out)
{
    size_t i, pos = 0;
    qsort(e->items, e->count, sizeof *e->items, niu_edit_cmp);
    for (i = 0; i < e->count; i++) {
        const niu_edit *ed = &e->items[i];
        if (ed->begin > ed->end || ed->end > len || ed->begin < pos)
            return 0;
    }
    for (i = 0; i < e->count; i++) {
        const niu_edit *ed = &e->items[i];
        niu_buf_append(out, src + pos, ed->begin - pos);
        niu_buf_puts(out, ed->text);
        pos = ed->end;
    }
    niu_buf_append(out, src + pos, len - pos);
    return 1;
}

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
