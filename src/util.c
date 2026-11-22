/* util.c - memory, growable buffers, string lists and messages.
 * Part of niu.c; not compiled on its own. */

#define NIU_VERSION "0.1.0-dev"

/* Exit codes. */
#define NIU_EXIT_OK 0
#define NIU_EXIT_ERROR 1
#define NIU_EXIT_USAGE 2

static void *niu_xrealloc(void *p, size_t size)
{
    void *q = realloc(p, size ? size : 1);
    if (!q) {
        fputs("niu: error: out of memory\n", stderr);
        exit(NIU_EXIT_ERROR);
    }
    return q;
}

static void *niu_xmalloc(size_t size)
{
    return niu_xrealloc(NULL, size);
}

static char *niu_strndup(const char *s, size_t n)
{
    char *d = (char *)niu_xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static char *niu_strdup(const char *s)
{
    return niu_strndup(s, strlen(s));
}

static int niu_starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int niu_ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && memcmp(s + n - m, suffix, m) == 0;
}

/* ---- growable byte buffer ---- */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} niu_buf;

static void niu_buf_reserve(niu_buf *b, size_t extra)
{
    if (b->len + extra + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < b->len + extra + 1)
            cap *= 2;
        b->data = (char *)niu_xrealloc(b->data, cap);
        b->cap = cap;
    }
}

static void niu_buf_append(niu_buf *b, const char *s, size_t n)
{
    niu_buf_reserve(b, n);
    if (n)
        memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void niu_buf_puts(niu_buf *b, const char *s)
{
    niu_buf_append(b, s, strlen(s));
}

static void niu_buf_putc(niu_buf *b, char c)
{
    niu_buf_append(b, &c, 1);
}

static void niu_buf_vprintf(niu_buf *b, const char *fmt, va_list ap)
{
    va_list ap2;
    int n;
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0)
        return;
    niu_buf_reserve(b, (size_t)n);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    b->len += (size_t)n;
}

static void niu_buf_printf(niu_buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    niu_buf_vprintf(b, fmt, ap);
    va_end(ap);
}

/* Returns the contents as a NUL-terminated string ("" when empty). */
static const char *niu_buf_str(niu_buf *b)
{
    if (!b->data) {
        niu_buf_reserve(b, 0);
        b->data[0] = '\0';
    }
    return b->data;
}

/* Hands the contents over to the caller, who frees them. */
static char *niu_buf_take(niu_buf *b)
{
    char *s;
    niu_buf_str(b);
    s = b->data;
    b->data = NULL;
    b->len = b->cap = 0;
    return s;
}

static void niu_buf_free(niu_buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static char *niu_format(const char *fmt, ...)
{
    niu_buf b = {0};
    va_list ap;
    va_start(ap, fmt);
    niu_buf_vprintf(&b, fmt, ap);
    va_end(ap);
    return niu_buf_take(&b);
}

/* ---- list of owned strings ---- */

typedef struct {
    char **items;
    size_t count;
    size_t cap;
} niu_strlist;

static void niu_strlist_push_owned(niu_strlist *l, char *s)
{
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->items = (char **)niu_xrealloc(l->items, l->cap * sizeof *l->items);
    }
    l->items[l->count++] = s;
}

static void niu_strlist_push(niu_strlist *l, const char *s)
{
    niu_strlist_push_owned(l, niu_strdup(s));
}

static int niu_strlist_contains(const niu_strlist *l, const char *s)
{
    size_t i;
    for (i = 0; i < l->count; i++)
        if (strcmp(l->items[i], s) == 0)
            return 1;
    return 0;
}

static void niu_strlist_free(niu_strlist *l)
{
    size_t i;
    for (i = 0; i < l->count; i++)
        free(l->items[i]);
    free(l->items);
    l->items = NULL;
    l->count = l->cap = 0;
}

/* ---- messages ---- */

/* "niu: <kind>: <message>" for problems that have no source location. */
static void niu_message(const char *kind, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "niu: %s: ", kind);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* "<where>: <kind>: <message>", where is a file or file:line:col. */
static void niu_message_at(const char *where, const char *kind, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s: %s: ", where, kind);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ---- whole-file I/O (binary, so line endings are kept) ---- */

static int niu_read_file(const char *path, niu_buf *out)
{
    FILE *f = fopen(path, "rb");
    char chunk[65536];
    size_t n;
    if (!f)
        return 0;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
        niu_buf_append(out, chunk, n);
    if (ferror(f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    niu_buf_str(out);
    return 1;
}

/* Reads at most max bytes from the start of a file. */
static size_t niu_read_prefix(const char *path, char *dst, size_t max)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f)
        return 0;
    n = fread(dst, 1, max, f);
    fclose(f);
    return n;
}
