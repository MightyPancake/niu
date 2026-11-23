/* fmtv.c - the %v conversion (spec.md "Automatic format specifier (%v)"):
 * finding it in the format strings of printf-style calls, checking it, and
 * the edits that lower it. Part of niu.c; not compiled on its own.
 *
 * Each %v becomes a %s, and its argument becomes a call to a function niu
 * generates for the argument's type, which formats the value into a buffer
 * (a compound literal) and returns it. The generated functions and the
 * small runtime they share go at file scope, before the first function in
 * the file that needs them. */

#define NIU_K_STRING_LITERAL 109

/* clang type kinds (enum CXTypeKind) of the types %v prints */
#define NIU_T_BOOL 3
#define NIU_T_CHAR_U 4
#define NIU_T_UCHAR 5
#define NIU_T_USHORT 8
#define NIU_T_UINT 9
#define NIU_T_ULONG 10
#define NIU_T_ULONGLONG 11
#define NIU_T_CHAR_S 13
#define NIU_T_SCHAR 14
#define NIU_T_SHORT 16
#define NIU_T_INT 17
#define NIU_T_LONG 18
#define NIU_T_LONGLONG 19
#define NIU_T_FLOAT 21
#define NIU_T_DOUBLE 22
#define NIU_T_LONGDOUBLE 23

/* ---- which functions take a format string ---- */

enum { NIU_FMT_NONE, NIU_FMT_PRINTF, NIU_FMT_VA_LIST, NIU_FMT_SCANF };

typedef struct {
    const char *name;
    int kind;
    int fmt; /* the format string's argument (0-based) */
} niu_fmt_fn;

/* Known by name when declared in a system header: some C libraries (MSVC's)
 * do not mark them with format attributes. */
static const niu_fmt_fn niu_fmt_fns[] = {
    {"printf", NIU_FMT_PRINTF, 0},      {"fprintf", NIU_FMT_PRINTF, 1},    {"sprintf", NIU_FMT_PRINTF, 1},
    {"snprintf", NIU_FMT_PRINTF, 2},    {"printf_s", NIU_FMT_PRINTF, 0},   {"fprintf_s", NIU_FMT_PRINTF, 1},
    {"sprintf_s", NIU_FMT_PRINTF, 2},   {"snprintf_s", NIU_FMT_PRINTF, 2}, {"vprintf", NIU_FMT_VA_LIST, 0},
    {"vfprintf", NIU_FMT_VA_LIST, 1},   {"vsprintf", NIU_FMT_VA_LIST, 1},  {"vsnprintf", NIU_FMT_VA_LIST, 2},
    {"vprintf_s", NIU_FMT_VA_LIST, 0},  {"vfprintf_s", NIU_FMT_VA_LIST, 1}, {"vsprintf_s", NIU_FMT_VA_LIST, 2},
    {"vsnprintf_s", NIU_FMT_VA_LIST, 2}, {"scanf", NIU_FMT_SCANF, 0},      {"fscanf", NIU_FMT_SCANF, 1},
    {"sscanf", NIU_FMT_SCANF, 1},       {"vscanf", NIU_FMT_SCANF, 0},      {"vfscanf", NIU_FMT_SCANF, 1},
    {"vsscanf", NIU_FMT_SCANF, 1},      {"scanf_s", NIU_FMT_SCANF, 0},     {"fscanf_s", NIU_FMT_SCANF, 1},
    {"sscanf_s", NIU_FMT_SCANF, 1},
    /* the fortified versions headers use instead (macOS always; glibc with
     * _FORTIFY_SOURCE), and clang's builtins */
    {"__builtin___printf_chk", NIU_FMT_PRINTF, 1}, {"__builtin___fprintf_chk", NIU_FMT_PRINTF, 2},
    {"__builtin___sprintf_chk", NIU_FMT_PRINTF, 3}, {"__builtin___snprintf_chk", NIU_FMT_PRINTF, 4},
    {"__builtin___vprintf_chk", NIU_FMT_VA_LIST, 1}, {"__builtin___vfprintf_chk", NIU_FMT_VA_LIST, 2},
    {"__builtin___vsprintf_chk", NIU_FMT_VA_LIST, 3}, {"__builtin___vsnprintf_chk", NIU_FMT_VA_LIST, 4},
    {"__printf_chk", NIU_FMT_PRINTF, 1},  {"__fprintf_chk", NIU_FMT_PRINTF, 2},
    {"__sprintf_chk", NIU_FMT_PRINTF, 3}, {"__snprintf_chk", NIU_FMT_PRINTF, 4},
    {"__vprintf_chk", NIU_FMT_VA_LIST, 1}, {"__vfprintf_chk", NIU_FMT_VA_LIST, 2},
    {"__vsprintf_chk", NIU_FMT_VA_LIST, 3}, {"__vsnprintf_chk", NIU_FMT_VA_LIST, 4},
    {"__builtin_printf", NIU_FMT_PRINTF, 0}, {"__builtin_fprintf", NIU_FMT_PRINTF, 1},
    {"__builtin_sprintf", NIU_FMT_PRINTF, 1}, {"__builtin_snprintf", NIU_FMT_PRINTF, 2},
    {"__builtin_vprintf", NIU_FMT_VA_LIST, 0}, {"__builtin_vfprintf", NIU_FMT_VA_LIST, 1},
    {"__builtin_vsprintf", NIU_FMT_VA_LIST, 1}, {"__builtin_vsnprintf", NIU_FMT_VA_LIST, 2},
    {"__builtin_scanf", NIU_FMT_SCANF, 0}, {"__builtin_sscanf", NIU_FMT_SCANF, 1},
    {"__builtin_fscanf", NIU_FMT_SCANF, 1}, {NULL, 0, 0}};

typedef struct {
    niu_dctx *c;
    int kind;
    int fmt;
} niu_fmt_attr_ctx;

/* Reads `format(printf, m, n)` (or `__format__(__printf__, m, n)`) from the
 * text of an attribute: libclang does not expose its arguments. */
static int niu_fmt_parse_attr(niu_fmt_attr_ctx *ac, const char *text, size_t len)
{
    niu_lexer lx;
    niu_tok t[8];
    int i;
    niu_lex_init(&lx, text, len, 0);
    lx.bol = 0;
    for (;;) {
        t[0] = niu_lex_next(&lx);
        if (t[0].kind == NIU_TOK_EOF)
            return 0;
        if (niu_tok_is(text, &t[0], "format") || niu_tok_is(text, &t[0], "__format__"))
            break;
    }
    for (i = 1; i < 8; i++)
        t[i] = niu_lex_next(&lx);
    /* ( archetype , m , n ) */
    if (t[1].kind != NIU_TOK_PUNCT || text[t[1].b] != '(' || t[2].kind != NIU_TOK_IDENT ||
        t[3].kind != NIU_TOK_PUNCT || text[t[3].b] != ',' || t[4].kind != NIU_TOK_NUMBER ||
        t[5].kind != NIU_TOK_PUNCT || text[t[5].b] != ',' || t[6].kind != NIU_TOK_NUMBER)
        return 0;
    {
        char *arch = niu_strndup(text + t[2].b, t[2].e - t[2].b);
        const char *a = arch;
        int m = atoi(text + t[4].b), n = atoi(text + t[6].b);
        if (strncmp(a, "__", 2) == 0)
            a += 2;
        if (m >= 1) {
            if (strncmp(a, "printf", 6) == 0 || strncmp(a, "gnu_printf", 10) == 0 || strncmp(a, "ms_printf", 9) == 0) {
                ac->kind = n == 0 ? NIU_FMT_VA_LIST : NIU_FMT_PRINTF;
                ac->fmt = m - 1;
            } else if (strncmp(a, "scanf", 5) == 0 || strncmp(a, "gnu_scanf", 9) == 0 ||
                       strncmp(a, "ms_scanf", 8) == 0) {
                ac->kind = NIU_FMT_SCANF;
                ac->fmt = m - 1;
            }
        }
        free(arch);
    }
    return 1;
}

/* The text of the macro use at the start of text, expanded one level:
 * `PRINTF_LIKE(2, 3)` with `#define PRINTF_LIKE(f, a)
 * __attribute__((format(printf, f, a)))`. NULL when it is not a macro use. */
static char *niu_fmt_expand(niu_dctx *c, const char *text, size_t len)
{
    niu_lexer lx;
    niu_tok t;
    char *name, *body;
    size_t i, nb;
    niu_strlist params = {0}, args = {0};
    niu_buf out = {0};
    int fnlike = 0;
    niu_lex_init(&lx, text, len, 0);
    lx.bol = 0;
    t = niu_lex_next(&lx);
    if (t.kind != NIU_TOK_IDENT)
        return NULL;
    name = niu_strndup(text + t.b, t.e - t.b);
    for (i = 0; i < c->macro_names.count && strcmp(c->macro_names.items[i], name) != 0; i++)
        ;
    free(name);
    if (i == c->macro_names.count)
        return NULL;
    body = niu_macro_body(c, c->macro_defs[i]);
    if (!body)
        return NULL;
    /* the use's arguments */
    {
        niu_lexer save = lx;
        niu_tok o = niu_lex_next(&lx);
        if (o.kind == NIU_TOK_PUNCT && text[o.b] == '(') {
            int depth = 0;
            size_t ab = o.e;
            for (;;) {
                niu_tok a = niu_lex_next(&lx);
                if (a.kind == NIU_TOK_EOF)
                    break;
                if (a.kind == NIU_TOK_PUNCT && text[a.b] == '(')
                    depth++;
                else if (a.kind == NIU_TOK_PUNCT && text[a.b] == ')' && depth-- == 0) {
                    niu_strlist_push_owned(&args, niu_strndup(text + ab, a.b - ab));
                    break;
                } else if (a.kind == NIU_TOK_PUNCT && text[a.b] == ',' && depth == 0) {
                    niu_strlist_push_owned(&args, niu_strndup(text + ab, a.b - ab));
                    ab = a.e;
                }
            }
        } else {
            lx = save;
        }
    }
    /* the definition: name, parameters, replacement list */
    nb = strlen(body);
    niu_lex_init(&lx, body, nb, 0);
    lx.bol = 0;
    t = niu_lex_next(&lx); /* the name */
    if (t.e < nb && body[t.e] == '(') {
        fnlike = 1;
        niu_lex_next(&lx);
        for (;;) {
            niu_tok p = niu_lex_next(&lx);
            if (p.kind == NIU_TOK_EOF || (p.kind == NIU_TOK_PUNCT && body[p.b] == ')'))
                break;
            if (p.kind == NIU_TOK_IDENT)
                niu_strlist_push_owned(&params, niu_strndup(body + p.b, p.e - p.b));
        }
    }
    for (;;) {
        niu_tok r = niu_lex_next(&lx);
        size_t k;
        if (r.kind == NIU_TOK_EOF)
            break;
        niu_buf_putc(&out, ' ');
        if (fnlike && r.kind == NIU_TOK_IDENT)
            for (k = 0; k < params.count && k < args.count; k++)
                if (r.e - r.b == strlen(params.items[k]) && memcmp(body + r.b, params.items[k], r.e - r.b) == 0)
                    break;
        if (fnlike && r.kind == NIU_TOK_IDENT && k < params.count && k < args.count)
            niu_buf_puts(&out, args.items[k]);
        else
            niu_buf_append(&out, body + r.b, r.e - r.b);
    }
    free(body);
    niu_strlist_free(&params);
    niu_strlist_free(&args);
    return niu_buf_take(&out);
}

static enum CXChildVisitResult niu_fmt_attr_visit(CXCursor cur, CXCursor parent, CXClientData data)
{
    niu_fmt_attr_ctx *ac = (niu_fmt_attr_ctx *)data;
    (void)parent;
    if (cx.clang_isAttribute(cx.clang_getCursorKind(cur))) {
        CXSourceRange r = cx.clang_getCursorExtent(cur);
        CXFile f = NULL;
        unsigned line, col, b = 0, e = 0;
        size_t size = 0;
        const char *text;
        int done = 0;
        cx.clang_getSpellingLocation(cx.clang_getRangeStart(r), &f, &line, &col, &b);
        cx.clang_getSpellingLocation(cx.clang_getRangeEnd(r), &f, &line, &col, &e);
        text = f ? cx.clang_getFileContents(ac->c->tu, f, &size) : NULL;
        if (text && b < size) {
            /* the extent can end before the closing ')' */
            size_t end = e + 2 < size ? e + 2 : size;
            if (end < b + 8)
                end = b + 8 < size ? b + 8 : size;
            while (end < size && text[end - 1] != ')' && end < b + 256)
                end++;
            done = niu_fmt_parse_attr(ac, text + b, end - b);
        }
        if (!done) {
            /* written through a macro: expand the use, up to a few levels */
            int depth;
            char *x;
            f = NULL;
            cx.clang_getExpansionLocation(cx.clang_getRangeStart(r), &f, &line, &col, &b);
            text = f ? cx.clang_getFileContents(ac->c->tu, f, &size) : NULL;
            x = text && b < size ? niu_fmt_expand(ac->c, text + b, size - b < 512 ? size - b : 512) : NULL;
            for (depth = 0; x && depth < 4; depth++) {
                char *y;
                if (niu_fmt_parse_attr(ac, x, strlen(x)))
                    break;
                y = niu_fmt_expand(ac->c, x, strlen(x));
                free(x);
                x = y;
            }
            free(x);
        }
    }
    return CXChildVisit_Continue;
}

/* What kind of format string function f takes, and which argument it is. */
static int niu_fmt_kind(niu_dctx *c, CXCursor f, int *fmt)
{
    niu_fmt_attr_ctx ac;
    char *name = niu_cxstr(cx.clang_getCursorSpelling(f));
    size_t i;
    ac.c = c;
    ac.kind = NIU_FMT_NONE;
    ac.fmt = 0;
    /* (a builtin has no header) */
    if (strncmp(name, "__builtin", 9) == 0 || cx.clang_Location_isInSystemHeader(cx.clang_getCursorLocation(f)))
        for (i = 0; niu_fmt_fns[i].name; i++)
            if (strcmp(niu_fmt_fns[i].name, name) == 0) {
                ac.kind = niu_fmt_fns[i].kind;
                ac.fmt = niu_fmt_fns[i].fmt;
            }
    free(name);
    if (ac.kind == NIU_FMT_NONE)
        cx.clang_visitChildren(f, niu_fmt_attr_visit, &ac);
    *fmt = ac.fmt;
    return ac.kind;
}

/* ---- the format string ---- */

/* One conversion specification, with offsets in the niu file. */
typedef struct {
    size_t b, e;      /* the whole conversion, from '%' to its letter */
    char conv;        /* the letter */
    int positional;   /* %1$d, or a *1$ width or precision */
    char bad_flag;    /* a flag other than '-' (for %v), or 0 */
    int minus;
    int width_star;
    size_t wb, we;    /* a number width as written, or wb == we */
    int has_prec;     /* a '.' */
    int prec_star;
    long prec;        /* the number after the '.' (0 for a bare '.') */
    int length;       /* a length modifier */
    int arg_width, arg_prec, arg_value; /* argument indices (0-based in the call), or -1 */
} niu_conv;

typedef struct {
    niu_conv *items;
    size_t count, cap;
} niu_convs;

/* The characters of a string literal's contents, decoded, each with the
 * offset of where it is spelled. */
typedef struct {
    int *ch;
    size_t *off; /* begin of each character; off[count] is the end of the last */
    size_t count, cap;
} niu_lchars;

static void niu_lchars_push(niu_lchars *l, int ch, size_t off)
{
    if (l->count + 1 >= l->cap) {
        l->cap = l->cap ? 2 * l->cap : 64;
        l->ch = (int *)niu_xrealloc(l->ch, l->cap * sizeof *l->ch);
        l->off = (size_t *)niu_xrealloc(l->off, l->cap * sizeof *l->off);
    }
    l->ch[l->count] = ch;
    l->off[l->count] = off;
    l->count++;
}

static int niu_hexval(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

/* Decodes the contents of the string literal token [b, e) of s. Returns 0
 * for a wide or UTF-16/32 literal, which is not a printf format string. */
static int niu_decode_literal(const char *s, size_t b, size_t e, niu_lchars *out)
{
    size_t i = b;
    if (s[i] == 'u' && i + 1 < e && s[i + 1] == '8')
        i += 2;
    else if (s[i] == 'u' || s[i] == 'U' || s[i] == 'L')
        return 0;
    if (s[i] != '"')
        return 0;
    i++;
    if (e > i && s[e - 1] == '"')
        e--;
    while (i < e) {
        size_t start = i, sp = niu_splice_at(s, e, i);
        int v;
        if (sp) {
            i += sp;
            continue;
        }
        if (s[i] != '\\' || i + 1 >= e) {
            niu_lchars_push(out, (unsigned char)s[i], start);
            i++;
            continue;
        }
        i++;
        switch (s[i]) {
        case 'x':
            v = 0;
            i++;
            while (i < e && niu_hexval(s[i]) >= 0)
                v = (v * 16 + niu_hexval(s[i++])) & 0xffff;
            break;
        case 'u':
        case 'U': {
            int n = s[i] == 'u' ? 4 : 8;
            v = 0x100; /* not ASCII: a character niu does not look at */
            i++;
            while (n-- > 0 && i < e && niu_hexval(s[i]) >= 0)
                i++;
            break;
        }
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
            int n = 0;
            v = 0;
            while (n < 3 && i < e && s[i] >= '0' && s[i] <= '7') {
                v = v * 8 + (s[i++] - '0');
                n++;
            }
            break;
        }
        default:
            v = s[i] == 'n' ? '\n' : s[i] == 't' ? '\t' : (unsigned char)s[i];
            i++;
            break;
        }
        niu_lchars_push(out, v, start);
    }
    return 1;
}

/* Parses the conversions in the decoded format string; arguments are
 * numbered from first_arg. */
static void niu_parse_format(const niu_lchars *l, int first_arg, niu_convs *out)
{
    size_t i = 0, n = l->count;
    int arg = first_arg;
#define NIU_CH(k) ((k) < n ? l->ch[k] : 0)
    while (i < n) {
        niu_conv cv;
        size_t k;
        if (l->ch[i] != '%') {
            i++;
            continue;
        }
        memset(&cv, 0, sizeof cv);
        cv.b = l->off[i];
        cv.arg_width = cv.arg_prec = cv.arg_value = -1;
        k = i + 1;
        if (NIU_CH(k) == '%') {
            i = k + 1;
            continue;
        }
        /* n$ */
        {
            size_t d = k;
            while (NIU_CH(d) >= '0' && NIU_CH(d) <= '9')
                d++;
            if (d > k && NIU_CH(d) == '$') {
                cv.positional = 1;
                k = d + 1;
            }
        }
        /* flags */
        for (;; k++) {
            int ch = NIU_CH(k);
            if (ch == '-')
                cv.minus = 1;
            else if (ch == '+' || ch == ' ' || ch == '#' || ch == '0' || ch == '\'') {
                if (!cv.bad_flag)
                    cv.bad_flag = (char)ch;
            } else
                break;
        }
        /* width */
        if (NIU_CH(k) == '*') {
            cv.width_star = 1;
            k++;
            while (NIU_CH(k) >= '0' && NIU_CH(k) <= '9')
                k++;
            if (NIU_CH(k) == '$') {
                cv.positional = 1;
                k++;
            }
        } else if (NIU_CH(k) >= '1' && NIU_CH(k) <= '9') {
            cv.wb = l->off[k];
            while (NIU_CH(k) >= '0' && NIU_CH(k) <= '9')
                k++;
            cv.we = l->off[k];
        }
        /* precision */
        if (NIU_CH(k) == '.') {
            cv.has_prec = 1;
            k++;
            if (NIU_CH(k) == '*') {
                cv.prec_star = 1;
                k++;
                while (NIU_CH(k) >= '0' && NIU_CH(k) <= '9')
                    k++;
                if (NIU_CH(k) == '$') {
                    cv.positional = 1;
                    k++;
                }
            } else {
                while (NIU_CH(k) >= '0' && NIU_CH(k) <= '9') {
                    if (cv.prec < 100000000L)
                        cv.prec = cv.prec * 10 + (NIU_CH(k) - '0');
                    k++;
                }
            }
        }
        /* length modifiers (ISO C, and MSVC's I, I32, I64) */
        for (;;) {
            int ch = NIU_CH(k);
            if (ch == 'I' && ((NIU_CH(k + 1) == '3' && NIU_CH(k + 2) == '2') ||
                              (NIU_CH(k + 1) == '6' && NIU_CH(k + 2) == '4'))) {
                cv.length = 1;
                k += 3;
            } else if (ch == 'h' || ch == 'l' || ch == 'j' || ch == 'z' || ch == 't' || ch == 'L' || ch == 'q' ||
                       ch == 'I' || ch == 'w') {
                cv.length = 1;
                k++;
            } else
                break;
        }
        if (k >= n)
            break; /* a '%' at the end: the C library's problem */
        cv.conv = (char)l->ch[k];
        cv.e = l->off[k + 1]; /* off[n] is the end of the last character */
        if (cv.width_star)
            cv.arg_width = arg++;
        if (cv.prec_star)
            cv.arg_prec = arg++;
        if (cv.conv != 'm')
            cv.arg_value = arg++;
        if (out->count == out->cap) {
            out->cap = out->cap ? 2 * out->cap : 8;
            out->items = (niu_conv *)niu_xrealloc(out->items, out->cap * sizeof *out->items);
        }
        out->items[out->count++] = cv;
        i = k + 1;
    }
#undef NIU_CH
}

/* ---- the generated C: the runtime ---- */

/* The runtime the generated functions share (spec.md "Length limit"): the
 * output so far, and the last place it can be cut, with what is open there
 * ('"' a string, '{' braces with nothing in them yet, '}' braces with
 * something). Each piece is written only when a generated function needs
 * it, since a compiler may warn about unused static functions. */
static const char niu_v_runtime_head[] =
    "#ifndef __niu_v_defined\n"
    "#define __niu_v_defined\n"
    "#ifndef NIU_V_LIMIT\n"
    "#define NIU_V_LIMIT 2048\n"
    "#endif\n"
    "#if NIU_V_LIMIT < 64\n"
    "#error \"NIU_V_LIMIT must be at least 64\"\n"
    "#endif\n"
    "#ifdef __MINGW32__\n"
    "#define __niu_vr_snprintf __mingw_snprintf\n"
    "#else\n"
    "#define __niu_vr_snprintf snprintf\n"
    "#endif\n"
    "typedef struct {\n"
    "    char *p;\n"
    "    size_t n, len, safe;\n"
    "    int depth, safe_depth, cut;\n"
    "    char open[32], safe_open[32];\n"
    "} __niu_vr_out;\n"
    "static inline void __niu_vr_begin(__niu_vr_out *o, char *p, size_t n)\n"
    "{\n"
    "    o->p = p;\n"
    "    o->n = n;\n"
    "    o->len = o->safe = 0;\n"
    "    o->depth = o->safe_depth = o->cut = 0;\n"
    "}\n"
    "static inline char *__niu_vr_end(__niu_vr_out *o)\n"
    "{\n"
    "    o->p[o->len] = '\\0';\n"
    "    return o->p;\n"
    "}\n"
    "#endif\n";

typedef struct {
    const char *name;
    const char *deps[3];
    const char *text;
} niu_v_piece;

static const niu_v_piece niu_v_pieces[] = {
    {"closing", {NULL},
     /* the length of the text that closes everything open */
     "static inline size_t __niu_vr_closing(const char *open, int depth)\n"
     "{\n"
     "    if (depth == 0)\n"
     "        return 3;\n"
     "    return (open[depth - 1] == '}' ? 6 : 4) + 6 * (size_t)(depth - 1);\n"
     "}\n"},
    {"mark", {"closing", NULL},
     /* a place the output can be cut, if the closing text fits after it */
     "static inline void __niu_vr_mark(__niu_vr_out *o)\n"
     "{\n"
     "    int i;\n"
     "    if (o->cut || o->len + __niu_vr_closing(o->open, o->depth) + 1 > o->n)\n"
     "        return;\n"
     "    o->safe = o->len;\n"
     "    o->safe_depth = o->depth;\n"
     "    for (i = 0; i < o->depth; i++)\n"
     "        o->safe_open[i] = o->open[i];\n"
     "}\n"},
    {"raw", {NULL},
     "static inline void __niu_vr_raw(__niu_vr_out *o, const char *s)\n"
     "{\n"
     "    while (*s && o->len + 1 < o->n)\n"
     "        o->p[o->len++] = *s++;\n"
     "}\n"},
    {"cut", {"raw", NULL},
     /* back to the last mark, then "..." and the closing text */
     "static inline void __niu_vr_cut(__niu_vr_out *o)\n"
     "{\n"
     "    int d = o->safe_depth;\n"
     "    o->len = o->safe;\n"
     "    o->cut = 1;\n"
     "    if (d == 0) {\n"
     "        __niu_vr_raw(o, \"...\");\n"
     "        return;\n"
     "    }\n"
     "    __niu_vr_raw(o, o->safe_open[d - 1] == '\"' ? \"...\\\"\" : o->safe_open[d - 1] == '{' ? \"...}\" : \", ...}\");\n"
     "    while (--d > 0)\n"
     "        __niu_vr_raw(o, \", ...}\");\n"
     "}\n"},
    {"put", {"cut", NULL},
     "static inline void __niu_vr_put(__niu_vr_out *o, const char *s, size_t k)\n"
     "{\n"
     "    size_t i;\n"
     "    if (o->cut)\n"
     "        return;\n"
     "    if (o->len + k + 1 > o->n) {\n"
     "        __niu_vr_cut(o);\n"
     "        return;\n"
     "    }\n"
     "    for (i = 0; i < k; i++)\n"
     "        o->p[o->len + i] = s[i];\n"
     "    o->len += k;\n"
     "}\n"},
    {"puts", {"put", NULL},
     "static inline void __niu_vr_puts(__niu_vr_out *o, const char *s)\n"
     "{\n"
     "    size_t k = 0;\n"
     "    while (s[k])\n"
     "        k++;\n"
     "    __niu_vr_put(o, s, k);\n"
     "}\n"},
    {"done", {"cut", NULL},
     /* after an snprintf straight into the buffer */
     "static inline void __niu_vr_done(__niu_vr_out *o, int k)\n"
     "{\n"
     "    if (k < 0 || o->len + (size_t)k + 1 > o->n)\n"
     "        __niu_vr_cut(o);\n"
     "    else\n"
     "        o->len += (size_t)k;\n"
     "}\n"},
    {"ll", {"done", NULL},
     "static inline void __niu_vr_ll(__niu_vr_out *o, long long v)\n"
     "{\n"
     "    if (!o->cut)\n"
     "        __niu_vr_done(o, __niu_vr_snprintf(o->p + o->len, o->n - o->len, \"%lld\", v));\n"
     "}\n"},
    {"ull", {"done", NULL},
     "static inline void __niu_vr_ull(__niu_vr_out *o, unsigned long long v)\n"
     "{\n"
     "    if (!o->cut)\n"
     "        __niu_vr_done(o, __niu_vr_snprintf(o->p + o->len, o->n - o->len, \"%llu\", v));\n"
     "}\n"},
    {"ptr", {"done", NULL},
     "static inline void __niu_vr_ptr(__niu_vr_out *o, unsigned long long v)\n"
     "{\n"
     "    if (!o->cut)\n"
     "        __niu_vr_done(o, __niu_vr_snprintf(o->p + o->len, o->n - o->len, \"0x%llx\", v));\n"
     "}\n"},
    {"c", {"put", NULL},
     "static inline void __niu_vr_c(__niu_vr_out *o, int v)\n"
     "{\n"
     "    char t[32];\n"
     "    int k;\n"
     "    if (v > 0x20 && v < 0x7f)\n"
     "        k = __niu_vr_snprintf(t, sizeof t, v == '\\'' || v == '\\\\' ? \"%d ('\\\\%c')\" : \"%d ('%c')\", v, v);\n"
     "    else\n"
     "        k = __niu_vr_snprintf(t, sizeof t, \"%d\", v);\n"
     "    __niu_vr_put(o, t, k > 0 ? (size_t)k : 0);\n"
     "}\n"},
    {"bool", {"put", NULL},
     /* a bool read through its byte, which can be anything in a union */
     "static inline void __niu_vr_bool(__niu_vr_out *o, unsigned char b)\n"
     "{\n"
     "    char t[16];\n"
     "    int k;\n"
     "    if (b <= 1) {\n"
     "        __niu_vr_put(o, b ? \"true\" : \"false\", b ? 4 : 5);\n"
     "        return;\n"
     "    }\n"
     "    k = __niu_vr_snprintf(t, sizeof t, \"(bool)%d\", b);\n"
     "    __niu_vr_put(o, t, k > 0 ? (size_t)k : 0);\n"
     "}\n"},
    {"d", {"puts", "done", NULL},
     "static inline void __niu_vr_d(__niu_vr_out *o, double v, int prec)\n"
     "{\n"
     "    if (o->cut)\n"
     "        return;\n"
     "    if (v != v)\n"
     "        __niu_vr_puts(o, \"nan\");\n"
     "    else if (v - v != 0)\n"
     "        __niu_vr_puts(o, v < 0 ? \"-inf\" : \"inf\");\n"
     "    else\n"
     "        __niu_vr_done(o, __niu_vr_snprintf(o->p + o->len, o->n - o->len, \"%.*f\", prec < 0 ? 2 : prec, v));\n"
     "}\n"},
    {"ld", {"puts", "done", NULL},
     "static inline void __niu_vr_ld(__niu_vr_out *o, long double v, int prec)\n"
     "{\n"
     "    if (o->cut)\n"
     "        return;\n"
     "    if (v != v)\n"
     "        __niu_vr_puts(o, \"nan\");\n"
     "    else if (v - v != 0)\n"
     "        __niu_vr_puts(o, v < 0 ? \"-inf\" : \"inf\");\n"
     "    else\n"
     "        __niu_vr_done(o, __niu_vr_snprintf(o->p + o->len, o->n - o->len, \"%.*Lf\", prec < 0 ? 2 : prec, v));\n"
     "}\n"},
    {"open", {"puts", "mark", NULL},
     /* "(name){": braces with nothing in them yet */
     "static inline void __niu_vr_open(__niu_vr_out *o, const char *s)\n"
     "{\n"
     "    __niu_vr_puts(o, s);\n"
     "    if (o->cut)\n"
     "        return;\n"
     "    o->open[o->depth++] = '{';\n"
     "    __niu_vr_mark(o);\n"
     "}\n"},
    {"close", {"puts", NULL},
     "static inline void __niu_vr_close(__niu_vr_out *o, const char *s)\n"
     "{\n"
     "    __niu_vr_puts(o, s);\n"
     "    if (!o->cut)\n"
     "        o->depth--;\n"
     "}\n"},
    {"field", {"puts", NULL},
     /* ", .name = ": the braces have something in them now */
     "static inline void __niu_vr_field(__niu_vr_out *o, const char *s)\n"
     "{\n"
     "    __niu_vr_puts(o, s);\n"
     "    if (!o->cut)\n"
     "        o->open[o->depth - 1] = '}';\n"
     "}\n"},
    {"str", {"puts", "mark", "close"},
     /* a char array: up to its first null character or its end */
     "static inline void __niu_vr_str(__niu_vr_out *o, const char *s, size_t max)\n"
     "{\n"
     "    size_t i;\n"
     "    char t[8];\n"
     "    __niu_vr_puts(o, \"\\\"\");\n"
     "    if (o->cut)\n"
     "        return;\n"
     "    o->open[o->depth++] = '\"';\n"
     "    __niu_vr_mark(o);\n"
     "    for (i = 0; i < max && s[i]; i++) {\n"
     "        unsigned char ch = (unsigned char)s[i];\n"
     "        if (ch == '\"' || ch == '\\\\') {\n"
     "            t[0] = '\\\\';\n"
     "            t[1] = (char)ch;\n"
     "            t[2] = '\\0';\n"
     "        } else if (ch == '\\n' || ch == '\\t') {\n"
     "            t[0] = '\\\\';\n"
     "            t[1] = ch == '\\n' ? 'n' : 't';\n"
     "            t[2] = '\\0';\n"
     "        } else if (ch < 0x20 || ch > 0x7e) {\n"
     "            __niu_vr_snprintf(t, sizeof t, \"\\\\x%02x\", (unsigned)ch);\n"
     "        } else {\n"
     "            t[0] = (char)ch;\n"
     "            t[1] = '\\0';\n"
     "        }\n"
     "        __niu_vr_puts(o, t);\n"
     "        __niu_vr_mark(o);\n"
     "    }\n"
     "    __niu_vr_close(o, \"\\\"\");\n"
     "}\n"},
    {"enum", {"done", NULL},
     /* a value no enumerator has: "(my_enum)7" */
     "static inline void __niu_vr_enum(__niu_vr_out *o, const char *prefix, long long v)\n"
     "{\n"
     "    if (!o->cut)\n"
     "        __niu_vr_done(o, __niu_vr_snprintf(o->p + o->len, o->n - o->len, \"%s%lld\", prefix, v));\n"
     "}\n"},
    {NULL, {NULL}, NULL}};

/* Appends text, with its '\n's turned into the file's line ending. */
static void niu_put_eol_text(niu_buf *b, const char *s, const char *eol)
{
    for (; *s; s++) {
        if (*s == '\n')
            niu_buf_puts(b, eol);
        else
            niu_buf_putc(b, *s);
    }
}

/* ---- the generated C: one function per type ---- */

/* What the generated C before one function needs. */
typedef struct {
    niu_node *fn;      /* the top-level declaration the calls are in */
    size_t first_v;    /* the first %v in it */
    niu_strlist rts;   /* runtime pieces */
    niu_strlist texts; /* generated functions, in the order they must come */
} niu_v_need;

typedef struct {
    niu_dctx *c;
    niu_v_need *needs;
    size_t n_needs;
    niu_strlist generated; /* generated functions, by name */
    niu_strlist emitted;   /* runtime pieces written so far */
    niu_nlist wrapped;     /* statements put in braces */
    size_t *lits;          /* the format strings rewritten, by offset */
    size_t n_lits;
    size_t stdio_from;     /* where snprintf is declared, or (size_t)-1 */
    int stdio_done;
} niu_vctx;

static niu_v_need *niu_v_need_for(niu_vctx *vc, niu_node *fn, size_t v_off)
{
    size_t i;
    niu_v_need *nd;
    for (i = 0; i < vc->n_needs; i++)
        if (vc->needs[i].fn == fn)
            return &vc->needs[i];
    vc->needs = (niu_v_need *)niu_xrealloc(vc->needs, (vc->n_needs + 1) * sizeof *vc->needs);
    nd = &vc->needs[vc->n_needs++];
    memset(nd, 0, sizeof *nd);
    nd->fn = fn;
    nd->first_v = v_off;
    return nd;
}

static void niu_v_rt(niu_v_need *nd, const char *piece)
{
    if (!niu_strlist_contains(&nd->rts, piece))
        niu_strlist_push(&nd->rts, piece);
}

/* Generating the code for one value. */
typedef struct {
    niu_vctx *vc;
    niu_v_need *nd;
    char *err_type; /* a type %v cannot print, as written */
    int vars;       /* loop variables used */
} niu_gen;

static long long niu_sat_add(long long a, long long b)
{
    const long long cap = (long long)1 << 40;
    if (a < 0 || b < 0)
        return -1;
    return a + b > cap ? cap : a + b;
}

static long long niu_sat_mul(long long a, long long b)
{
    const long long cap = (long long)1 << 40;
    if (a < 0 || b < 0)
        return -1;
    if (b != 0 && a > cap / b)
        return cap;
    return a * b;
}

/* Strips attributes and the elaborated wrapper, keeping typedef names. */
static CXType niu_v_unsugar(CXType t)
{
    int guard;
    for (guard = 0; guard < 32; guard++) {
        if (t.kind == NIU_T_ATTRIBUTED)
            t = cx.clang_Type_getModifiedType(t);
        else if (t.kind == NIU_T_ELABORATED)
            t = cx.clang_Type_getNamedType(t);
        else
            break;
    }
    return t;
}

/* Is the declaration at file scope (not inside a function)? */
static int niu_v_file_scope(CXCursor d)
{
    int guard;
    for (guard = 0; guard < 64; guard++) {
        int k;
        d = cx.clang_getCursorSemanticParent(d);
        if (cx.clang_Cursor_isNull(d))
            return 1;
        k = cx.clang_getCursorKind(d);
        if (k == NIU_K_TRANSLATION_UNIT)
            return 1;
        if (k == NIU_K_FUNCTION_DECL)
            return 0;
    }
    return 1;
}

/* The name of a struct, union or enum type t (spec.md "The name of a type"):
 * as printed ("my_struct", "struct S"), as a C type, and as part of the
 * generated functions' names ("t_my_struct", "struct_S"). Returns 0 for an
 * unnamed type; *local is set when it is declared inside a function. */
static int niu_v_named(niu_dctx *c, CXType t, char **print, char **key, int *local)
{
    CXType canon = cx.clang_getCanonicalType(cx.clang_getUnqualifiedType(t));
    CXCursor r = cx.clang_getTypeDeclaration(canon);
    char *tag;
    size_t i;
    int guard;
    *local = !niu_v_file_scope(r);
    /* 1. the typedef name the type has, if it is at file scope */
    t = niu_v_unsugar(t);
    for (guard = 0; guard < 32 && t.kind == NIU_T_TYPEDEF; guard++) {
        CXCursor e = cx.clang_getTypeDeclaration(t);
        if (niu_v_file_scope(e)) {
            *print = niu_cxstr(cx.clang_getCursorSpelling(e));
            *key = niu_format("t_%s", *print);
            return 1;
        }
        t = niu_v_unsugar(cx.clang_getTypedefDeclUnderlyingType(e));
    }
    /* 2. its tag */
    tag = niu_record_tag(r);
    if (tag) {
        const char *w = niu_tag_word(cx.clang_getCursorKind(r));
        *print = niu_format("%s %s", w, tag);
        *key = niu_format("%s_%s", w, tag);
        free(tag);
        return 1;
    }
    /* 3. another typedef name for it at file scope */
    niu_load_typedefs(c);
    for (i = 0; i < c->typedef_names.count; i++)
        if (cx.clang_equalTypes(cx.clang_getCanonicalType(cx.clang_getCursorType(c->typedefs[i])), canon) &&
            niu_v_file_scope(c->typedefs[i])) {
            *print = niu_strdup(c->typedef_names.items[i]);
            *key = niu_format("t_%s", *print);
            return 1;
        }
    return 0;
}

typedef struct {
    CXCursor *items;
    size_t count;
} niu_cursors;

static enum CXVisitorResult niu_field_visit(CXCursor f, CXClientData data)
{
    niu_cursors *l = (niu_cursors *)data;
    l->items = (CXCursor *)niu_xrealloc(l->items, (l->count + 1) * sizeof *l->items);
    l->items[l->count++] = f;
    return CXVisit_Continue;
}

static enum CXChildVisitResult niu_enum_visit(CXCursor e, CXCursor parent, CXClientData data)
{
    (void)parent;
    if (cx.clang_getCursorKind(e) == NIU_K_ENUM_CONSTANT_DECL)
        niu_field_visit(e, data);
    return CXChildVisit_Continue;
}

/* Is the member an anonymous struct or union (C11)? */
static int niu_v_anonymous(CXCursor field)
{
    CXType ft = cx.clang_getCanonicalType(cx.clang_getCursorType(field));
    char *name = niu_cxstr(cx.clang_getCursorSpelling(field));
    int yes = ft.kind == NIU_T_RECORD &&
              (!*name || cx.clang_Cursor_isAnonymousRecordDecl(cx.clang_getTypeDeclaration(ft)));
    free(name);
    return yes;
}

/* The scalar runtime call for a type without members, or NULL. ck is the
 * canonical kind. */
static const char *niu_v_scalar_put(int ck)
{
    switch (ck) {
    case NIU_T_CHAR_U:
    case NIU_T_CHAR_S:
    case NIU_T_SCHAR:
    case NIU_T_UCHAR:
        return "c";
    case NIU_T_SHORT:
    case NIU_T_INT:
    case NIU_T_LONG:
    case NIU_T_LONGLONG:
        return "ll";
    case NIU_T_USHORT:
    case NIU_T_UINT:
    case NIU_T_ULONG:
    case NIU_T_ULONGLONG:
        return "ull";
    case NIU_T_FLOAT:
    case NIU_T_DOUBLE:
        return "d";
    case NIU_T_LONGDOUBLE:
        return "ld";
    case NIU_T_POINTER:
        return "ptr";
    case NIU_T_BOOL:
        return "bool";
    default:
        return NULL;
    }
}

/* Digits of the largest value of an n-byte integer (with the sign). */
static long long niu_int_width(long long size, int is_signed)
{
    switch (size) {
    case 1:
        return is_signed ? 4 : 3;
    case 2:
        return is_signed ? 6 : 5;
    case 4:
        return is_signed ? 11 : 10;
    default:
        return 20;
    }
}

/* The atomic type's value type, if t is _Atomic. */
static CXType niu_v_atomic_value(CXType t, int *atomic)
{
    CXType canon = cx.clang_getCanonicalType(t);
    *atomic = canon.kind == NIU_T_ATOMIC;
    return *atomic ? cx.clang_Type_getValueType(canon) : t;
}

/* The longest output of a value of type t, with precision prec (-1 for a
 * '*' precision: then -1, unless no floating value is in it). */
static long long niu_v_bound(niu_dctx *c, CXType t, long prec)
{
    int atomic;
    CXType canon;
    long long size;
    t = niu_v_atomic_value(t, &atomic);
    canon = cx.clang_getCanonicalType(cx.clang_getUnqualifiedType(t));
    size = cx.clang_Type_getSizeOf(canon);
    switch (canon.kind) {
    case NIU_T_BOOL:
        return 9;
    case NIU_T_CHAR_U:
    case NIU_T_CHAR_S:
    case NIU_T_SCHAR:
    case NIU_T_UCHAR:
        return 11;
    case NIU_T_SHORT:
    case NIU_T_INT:
    case NIU_T_LONG:
    case NIU_T_LONGLONG:
        return niu_int_width(size, 1);
    case NIU_T_USHORT:
    case NIU_T_UINT:
    case NIU_T_ULONG:
    case NIU_T_ULONGLONG:
        return niu_int_width(size, 0);
    case NIU_T_POINTER:
        return 2 + 2 * (size > 0 ? size : 8);
    case NIU_T_FLOAT:
    case NIU_T_DOUBLE:
    case NIU_T_LONGDOUBLE: {
        /* sign, every digit of the largest value, '.', the decimals */
        long long maxexp = canon.kind == NIU_T_FLOAT ? 38 : canon.kind == NIU_T_DOUBLE || size <= 8 ? 308 : 4932;
        if (prec < 0)
            return -1;
        return 1 + (maxexp + 1) + (prec > 0 ? 1 + prec : 0);
    }
    case NIU_T_ENUM: {
        niu_cursors ec = {0};
        char *print = NULL, *key = NULL;
        int local;
        long long b, k;
        size_t i;
        b = niu_v_named(c, t, &print, &key, &local) ? 2 + (long long)strlen(print) + 20 : 5 + 20;
        cx.clang_visitChildren(cx.clang_getTypeDeclaration(canon), niu_enum_visit, &ec);
        for (i = 0; i < ec.count; i++) {
            char *n = niu_cxstr(cx.clang_getCursorSpelling(ec.items[i]));
            k = (long long)strlen(n);
            if (k > b)
                b = k;
            free(n);
        }
        free(ec.items);
        free(print);
        free(key);
        return b;
    }
    case NIU_T_RECORD: {
        niu_cursors fl = {0};
        char *print = NULL, *key = NULL;
        int local;
        long long b;
        size_t i;
        b = niu_v_named(c, t, &print, &key, &local) ? 3 + (long long)strlen(print) : 7; /* "(S){" or "struct{" */
        cx.clang_Type_visitFields(canon, niu_field_visit, &fl);
        for (i = 0; i < fl.count; i++) {
            char *n = niu_cxstr(cx.clang_getCursorSpelling(fl.items[i]));
            long long fb = niu_v_bound(c, cx.clang_getCursorType(fl.items[i]), prec);
            if (i)
                b = niu_sat_add(b, 2);
            if (!niu_v_anonymous(fl.items[i]))
                b = niu_sat_add(b, (long long)strlen(n) + 4); /* ".a = " */
            b = niu_sat_add(b, fb);
            free(n);
        }
        free(fl.items);
        free(print);
        free(key);
        return niu_sat_add(b, 1);
    }
    case NIU_T_CONSTANT_ARRAY: {
        long long n = cx.clang_getArraySize(canon);
        CXType el = cx.clang_getArrayElementType(t);
        int ek = cx.clang_getCanonicalType(el).kind;
        if (ek == NIU_T_CHAR_S || ek == NIU_T_CHAR_U)
            return niu_sat_add(niu_sat_mul(4, n), 2);
        if (n <= 0)
            return 2;
        return niu_sat_add(niu_sat_mul(n, niu_sat_add(niu_v_bound(c, el, prec), 2)), 0);
    }
    case NIU_T_INCOMPLETE_ARRAY:
        return 5;
    default:
        return 0;
    }
}

/* The deepest nesting of braces and strings in a value of type t. */
static int niu_v_depth(CXType t)
{
    int atomic;
    CXType canon;
    t = niu_v_atomic_value(t, &atomic);
    canon = cx.clang_getCanonicalType(cx.clang_getUnqualifiedType(t));
    if (canon.kind == NIU_T_RECORD) {
        niu_cursors fl = {0};
        size_t i;
        int d = 0;
        cx.clang_Type_visitFields(canon, niu_field_visit, &fl);
        for (i = 0; i < fl.count; i++) {
            int k = niu_v_depth(cx.clang_getCursorType(fl.items[i]));
            if (k > d)
                d = k;
        }
        free(fl.items);
        return 1 + d;
    }
    if (canon.kind == NIU_T_CONSTANT_ARRAY) {
        int ek = cx.clang_getCanonicalType(cx.clang_getArrayElementType(canon)).kind;
        if (ek == NIU_T_CHAR_S || ek == NIU_T_CHAR_U)
            return 1;
        return 1 + niu_v_depth(cx.clang_getArrayElementType(canon));
    }
    return 0;
}

static void niu_gen_value(niu_gen *g, CXType t, const char *acc, int addressable, niu_buf *code, const char *ind);
static int niu_gen_named(niu_gen *g, CXType t, char **key);

/* The members of a struct or union, inline: "(name){.a = 1, .b = 2}". The
 * members of an anonymous member (C11) are read through acc too. */
static void niu_gen_record_body(niu_gen *g, CXType t, const char *open, const char *acc, niu_buf *code,
                                const char *ind)
{
    CXType canon = cx.clang_getCanonicalType(cx.clang_getUnqualifiedType(t));
    niu_cursors fl = {0};
    size_t i;
    niu_v_rt(g->nd, "open");
    niu_v_rt(g->nd, "field");
    niu_v_rt(g->nd, "mark");
    niu_v_rt(g->nd, "close");
    niu_buf_printf(code, "%s__niu_vr_open(o, \"%s\");\n", ind, open);
    cx.clang_Type_visitFields(canon, niu_field_visit, &fl);
    for (i = 0; i < fl.count; i++) {
        CXCursor f = fl.items[i];
        char *name = niu_cxstr(cx.clang_getCursorSpelling(f));
        CXType ft = cx.clang_getCursorType(f);
        if (niu_v_anonymous(f)) {
            CXCursor r = cx.clang_getTypeDeclaration(cx.clang_getCanonicalType(ft));
            char *o2 = niu_format("%s{", niu_tag_word(cx.clang_getCursorKind(r)));
            niu_buf_printf(code, "%s__niu_vr_field(o, \"%s\");\n", ind, i ? ", " : "");
            niu_gen_record_body(g, ft, o2, acc, code, ind);
            free(o2);
        } else {
            char *a = niu_format("%s.%s", acc, name);
            niu_buf_printf(code, "%s__niu_vr_field(o, \"%s.%s = \");\n", ind, i ? ", " : "", name);
            niu_gen_value(g, ft, a, !cx.clang_Cursor_isBitField(f), code, ind);
            free(a);
        }
        niu_buf_printf(code, "%s__niu_vr_mark(o);\n", ind);
        free(name);
    }
    free(fl.items);
    niu_buf_printf(code, "%s__niu_vr_close(o, \"}\");\n", ind);
}

/* An enum value, inline: its enumerator, else "(my_enum)7". */
static void niu_gen_enum_body(niu_gen *g, CXType t, const char *prefix, const char *acc, niu_buf *code,
                              const char *ind)
{
    CXType canon = cx.clang_getCanonicalType(cx.clang_getUnqualifiedType(t));
    niu_cursors ec = {0};
    size_t i, k;
    niu_v_rt(g->nd, "puts");
    niu_v_rt(g->nd, "enum");
    cx.clang_visitChildren(cx.clang_getTypeDeclaration(canon), niu_enum_visit, &ec);
    niu_buf_printf(code, "%sswitch ((long long)(%s)) {\n", ind, acc);
    for (i = 0; i < ec.count; i++) {
        long long v = cx.clang_getEnumConstantDeclValue(ec.items[i]);
        char *name;
        /* the first enumerator with a value wins */
        for (k = 0; k < i && cx.clang_getEnumConstantDeclValue(ec.items[k]) != v; k++)
            ;
        if (k < i)
            continue;
        name = niu_cxstr(cx.clang_getCursorSpelling(ec.items[i]));
        if (v == -9223372036854775807LL - 1)
            niu_buf_printf(code, "%scase -9223372036854775807LL - 1:\n", ind);
        else
            niu_buf_printf(code, "%scase %lldLL:\n", ind, v);
        niu_buf_printf(code, "%s    __niu_vr_puts(o, \"%s\");\n%s    break;\n", ind, name, ind);
        free(name);
    }
    niu_buf_printf(code, "%sdefault:\n%s    __niu_vr_enum(o, \"%s\", (long long)(%s));\n%s    break;\n%s}\n", ind,
                   ind, prefix, acc, ind, ind);
    free(ec.items);
}

/* The code that prints the value acc of type t. addressable: acc is an
 * object whose bytes can be read (not a bit-field). */
static void niu_gen_value(niu_gen *g, CXType t, const char *acc, int addressable, niu_buf *code, const char *ind)
{
    int atomic, ck;
    CXType canon;
    const char *put;
    t = niu_v_atomic_value(t, &atomic);
    canon = cx.clang_getCanonicalType(cx.clang_getUnqualifiedType(t));
    ck = canon.kind;
    if (atomic && (ck == NIU_T_RECORD)) {
        /* C does not allow reading the members of an _Atomic struct */
        if (!g->err_type) {
            char *s = niu_cxstr(cx.clang_getTypeSpelling(t));
            g->err_type = niu_format("_Atomic(%s)", s);
            free(s);
        }
        return;
    }
    if (ck == NIU_T_BOOL) {
        niu_v_rt(g->nd, "bool");
        if (addressable && !atomic)
            niu_buf_printf(code, "%s__niu_vr_bool(o, *(const unsigned char *)&%s);\n", ind, acc);
        else
            niu_buf_printf(code, "%s__niu_vr_bool(o, (unsigned char)((%s) ? 1 : 0));\n", ind, acc);
        return;
    }
    put = niu_v_scalar_put(ck);
    if (put) {
        niu_v_rt(g->nd, put);
        if (ck == NIU_T_POINTER)
            niu_buf_printf(code, "%s__niu_vr_ptr(o, (unsigned long long)(size_t)(%s));\n", ind, acc);
        else if (ck == NIU_T_FLOAT || ck == NIU_T_DOUBLE || ck == NIU_T_LONGDOUBLE)
            niu_buf_printf(code, "%s__niu_vr_%s(o, %s, prec);\n", ind, put, acc);
        else
            niu_buf_printf(code, "%s__niu_vr_%s(o, %s);\n", ind, put, acc);
        return;
    }
    if (ck == NIU_T_RECORD || ck == NIU_T_ENUM) {
        char *key = NULL;
        if (niu_gen_named(g, t, &key)) {
            if (ck == NIU_T_RECORD)
                niu_buf_printf(code, "%s__niu_vp_%s(o, &%s, prec);\n", ind, key, acc);
            else
                niu_buf_printf(code, "%s__niu_vp_%s(o, %s);\n", ind, key, acc);
            free(key);
        } else if (ck == NIU_T_RECORD) {
            CXCursor r = cx.clang_getTypeDeclaration(canon);
            char *open = niu_format("%s{", niu_tag_word(cx.clang_getCursorKind(r)));
            niu_gen_record_body(g, t, open, acc, code, ind);
            free(open);
        } else {
            niu_gen_enum_body(g, t, "enum ", acc, code, ind);
        }
        return;
    }
    if (ck == NIU_T_CONSTANT_ARRAY) {
        long long n = cx.clang_getArraySize(canon);
        CXType el = cx.clang_getArrayElementType(t);
        int ek = cx.clang_getCanonicalType(el).kind;
        if (ek == NIU_T_CHAR_S || ek == NIU_T_CHAR_U) {
            niu_v_rt(g->nd, "str");
            niu_buf_printf(code, "%s__niu_vr_str(o, %s, %lld);\n", ind, acc, n);
            return;
        }
        {
            int v = g->vars++;
            char *a = niu_format("%s[__niu_i%d]", acc, v);
            char *ind2 = niu_format("%s        ", ind);
            niu_v_rt(g->nd, "open");
            niu_v_rt(g->nd, "field");
            niu_v_rt(g->nd, "mark");
            niu_v_rt(g->nd, "close");
            niu_buf_printf(code, "%s__niu_vr_open(o, \"{\");\n", ind);
            niu_buf_printf(code, "%s{\n%s    size_t __niu_i%d;\n", ind, ind, v);
            niu_buf_printf(code, "%s    for (__niu_i%d = 0; __niu_i%d < %lld; __niu_i%d++) {\n", ind, v, v, n, v);
            niu_buf_printf(code, "%s__niu_vr_field(o, __niu_i%d ? \", \" : \"\");\n", ind2, v);
            if (ek == NIU_T_SCHAR || ek == NIU_T_UCHAR) {
                /* data, not text: plain numbers */
                niu_v_rt(g->nd, ek == NIU_T_SCHAR ? "ll" : "ull");
                niu_buf_printf(code, "%s__niu_vr_%s(o, %s);\n", ind2, ek == NIU_T_SCHAR ? "ll" : "ull", a);
            } else {
                niu_gen_value(g, el, a, 1, code, ind2);
            }
            niu_buf_printf(code, "%s__niu_vr_mark(o);\n%s    }\n%s}\n", ind2, ind, ind);
            niu_buf_printf(code, "%s__niu_vr_close(o, \"}\");\n", ind);
            free(a);
            free(ind2);
        }
        return;
    }
    if (ck == NIU_T_INCOMPLETE_ARRAY) {
        niu_v_rt(g->nd, "puts");
        niu_buf_printf(code, "%s__niu_vr_puts(o, \"{...}\");\n", ind);
        return;
    }
    if (!g->err_type)
        g->err_type = niu_cxstr(cx.clang_getTypeSpelling(t));
}

/* The function that writes a named struct, union or enum type t
 * (__niu_vp_<key>), generated once per file, after those it uses. Returns
 * 0 for an unnamed type. */
static int niu_gen_named(niu_gen *g, CXType t, char **key)
{
    niu_vctx *vc = g->vc;
    CXType canon = cx.clang_getCanonicalType(cx.clang_getUnqualifiedType(t));
    char *print = NULL, *fn, *ctype;
    int local;
    niu_buf code = {0}, f = {0};
    if (!niu_v_named(vc->c, t, &print, key, &local))
        return 0;
    fn = niu_format("vp_%s", *key);
    if (niu_strlist_contains(&vc->generated, fn)) {
        free(fn);
        free(print);
        return 1;
    }
    niu_strlist_push(&vc->generated, fn);
    ctype = niu_strdup(print);
    if (canon.kind == NIU_T_ENUM) {
        char *prefix = niu_format("(%s)", print);
        niu_gen_enum_body(g, t, prefix, "v", &code, "    ");
        niu_buf_printf(&f, "static inline void __niu_vp_%s(__niu_vr_out *o, %s v)\n{\n", *key, ctype);
        free(prefix);
    } else {
        char *open = niu_format("(%s){", print);
        niu_gen_record_body(g, t, open, "(*v)", &code, "    ");
        niu_buf_printf(&f, "static inline void __niu_vp_%s(__niu_vr_out *o, const %s *v, int prec)\n{\n    (void)prec;\n",
                       *key, ctype);
        free(open);
    }
    niu_buf_puts(&f, niu_buf_str(&code));
    niu_buf_puts(&f, "}\n");
    {
        niu_buf w = {0};
        niu_buf_printf(&w, "#ifndef __niu_vp_%s_defined\n#define __niu_vp_%s_defined\n%s#endif\n", *key, *key,
                       niu_buf_str(&f));
        niu_strlist_push_owned(&g->nd->texts, niu_buf_take(&w));
    }
    niu_buf_free(&code);
    niu_buf_free(&f);
    free(fn);
    free(print);
    free(ctype);
    return 1;
}

/* The type of a variadic argument as written, before the default argument
 * promotions turned a char, bool, enum or float into an int or double. */
static CXType niu_unpromoted_type(niu_node *arg)
{
    CXType t = cx.clang_getCursorType(arg->cur);
    niu_node *n = arg;
    int k = cx.clang_getCanonicalType(t).kind;
    while (n->kind == NIU_K_PAREN_EXPR && n->nkids == 1)
        n = n->kids[0];
    if (n->kind == NIU_K_UNEXPOSED_EXPR && n->nkids == 1 && niu_is_expr(n->kids[0]->kind) &&
        (k == NIU_T_INT || k == NIU_T_UINT || k == NIU_T_DOUBLE)) {
        CXType kt = cx.clang_getCursorType(n->kids[0]->cur);
        int kk = cx.clang_getCanonicalType(kt).kind;
        if (kk != k && (kk == NIU_T_BOOL || kk == NIU_T_CHAR_U || kk == NIU_T_CHAR_S || kk == NIU_T_SCHAR ||
                        kk == NIU_T_UCHAR || kk == NIU_T_SHORT || kk == NIU_T_USHORT || kk == NIU_T_FLOAT ||
                        kk == NIU_T_ENUM))
            return kt;
    }
    return t;
}

/* The generated function for a %v argument of type t: __niu_v_<name>. The
 * value is its parameter v. Returns 0 (and *err) when %v cannot print t. */
static int niu_gen_top(niu_vctx *vc, niu_v_need *nd, CXType t, char **name, char **err)
{
    niu_gen g;
    CXType canon = cx.clang_getCanonicalType(cx.clang_getUnqualifiedType(t));
    int ck = canon.kind;
    char *fn, *param, *key = NULL;
    niu_buf code = {0};
    memset(&g, 0, sizeof g);
    g.vc = vc;
    g.nd = nd;
    *err = NULL;
    if (ck == NIU_T_RECORD || ck == NIU_T_ENUM) {
        if (!niu_gen_named(&g, t, &key)) {
            *err = g.err_type;
            return 0; /* checked before: has a name */
        }
        fn = niu_strdup(key);
        {
            char *print, *k2;
            int local;
            niu_v_named(vc->c, t, &print, &k2, &local);
            param = print;
            free(k2);
        }
        if (ck == NIU_T_RECORD)
            niu_buf_printf(&code, "    __niu_vp_%s(o, &v, prec);\n", key);
        else
            niu_buf_printf(&code, "    __niu_vp_%s(o, v);\n", key);
        free(key);
    } else {
        static const struct {
            int kind;
            const char *fn, *param;
        } names[] = {{NIU_T_BOOL, "bool", "_Bool"},
                     {NIU_T_CHAR_U, "char", "char"},
                     {NIU_T_CHAR_S, "char", "char"},
                     {NIU_T_SCHAR, "signed_char", "signed char"},
                     {NIU_T_UCHAR, "unsigned_char", "unsigned char"},
                     {NIU_T_SHORT, "short", "short"},
                     {NIU_T_INT, "int", "int"},
                     {NIU_T_LONG, "long", "long"},
                     {NIU_T_LONGLONG, "long_long", "long long"},
                     {NIU_T_USHORT, "unsigned_short", "unsigned short"},
                     {NIU_T_UINT, "unsigned_int", "unsigned int"},
                     {NIU_T_ULONG, "unsigned_long", "unsigned long"},
                     {NIU_T_ULONGLONG, "unsigned_long_long", "unsigned long long"},
                     {NIU_T_FLOAT, "float", "float"},
                     {NIU_T_DOUBLE, "double", "double"},
                     {NIU_T_LONGDOUBLE, "long_double", "long double"},
                     {NIU_T_POINTER, "ptr", "unsigned long long"},
                     {0, NULL, NULL}};
        size_t i;
        for (i = 0; names[i].fn && names[i].kind != ck; i++)
            ;
        if (!names[i].fn) {
            *err = niu_cxstr(cx.clang_getTypeSpelling(t));
            return 0;
        }
        fn = niu_strdup(names[i].fn);
        param = niu_strdup(names[i].param);
        if (ck == NIU_T_POINTER) {
            niu_v_rt(nd, "ptr");
            niu_buf_puts(&code, "    __niu_vr_ptr(o, v);\n");
        } else {
            niu_gen_value(&g, canon, "v", 0, &code, "    ");
        }
    }
    if (g.err_type) {
        *err = g.err_type;
        free(fn);
        free(param);
        niu_buf_free(&code);
        return 0;
    }
    *name = fn;
    if (!niu_strlist_contains(&vc->generated, fn)) {
        niu_buf w = {0};
        niu_strlist_push(&vc->generated, fn);
        niu_buf_printf(&w, "#ifndef __niu_v_%s_defined\n#define __niu_v_%s_defined\n", fn, fn);
        niu_buf_printf(&w, "static inline char *__niu_v_%s(char *p, size_t n, int prec, %s v)\n{\n", fn, param);
        niu_buf_puts(&w, "    __niu_vr_out o_, *o = &o_;\n    (void)prec;\n    __niu_vr_begin(o, p, n);\n");
        niu_buf_puts(&w, niu_buf_str(&code));
        niu_buf_puts(&w, "    return __niu_vr_end(o);\n}\n#endif\n");
        niu_strlist_push_owned(&nd->texts, niu_buf_take(&w));
    }
    free(param);
    niu_buf_free(&code);
    return 1;
}

/* Writes the runtime piece `name` and those it needs, once per file. */
static void niu_v_piece_out(niu_buf *b, niu_strlist *emitted, const char *name, const char *eol)
{
    size_t i;
    int k;
    if (niu_strlist_contains(emitted, name))
        return;
    niu_strlist_push(emitted, name);
    for (i = 0; niu_v_pieces[i].name; i++)
        if (strcmp(niu_v_pieces[i].name, name) == 0) {
            niu_buf f = {0};
            for (k = 0; k < 3 && niu_v_pieces[i].deps[k]; k++)
                niu_v_piece_out(b, emitted, niu_v_pieces[i].deps[k], eol);
            niu_buf_printf(&f, "#ifndef __niu_vr_%s_defined\n#define __niu_vr_%s_defined\n", name, name);
            niu_buf_puts(&f, niu_v_pieces[i].text);
            niu_buf_puts(&f, "#endif\n");
            niu_put_eol_text(b, niu_buf_str(&f), eol);
            niu_buf_free(&f);
            return;
        }
}

/* ---- one call ---- */

/* The top-level declaration (function) a node is in. */
static niu_node *niu_top_of(niu_node *n)
{
    while (n->parent)
        n = n->parent;
    return n;
}

/* Puts the statement that contains the call in braces, when the call is a
 * whole statement (spec.md "A call that is a whole statement gets braces"). */
static void niu_v_wrap(niu_vctx *vc, niu_node *call)
{
    niu_dctx *c = vc->c;
    niu_node *s = call, *p;
    size_t i, e;
    int ok;
    while (s->parent && niu_is_expr(s->parent->kind))
        s = s->parent;
    p = s->parent;
    if (!p || p->from_macro)
        return;
    if (s->from_macro) {
        /* a whole macro use, LOG("%v", x); */
        niu_expansion *x = niu_expansion_at(c, s->b);
        if (!x || x->b != s->b || niu_end_of(c, s) != x->e)
            return;
    }
    switch (p->kind) {
    case NIU_K_COMPOUND_STMT:
        /* not the last statement of a statement expression: it is the value */
        ok = !(p->parent && p->parent->kind == NIU_K_STMT_EXPR && s == niu_last_kid(p));
        break;
    case NIU_K_IF_STMT:
        ok = s->index >= 1;
        break;
    case NIU_K_WHILE_STMT:
        ok = s->index == 1;
        break;
    case NIU_K_DO_STMT:
        ok = s->index == 0;
        break;
    case NIU_K_FOR_STMT:
    case NIU_K_LABEL_STMT:
    case NIU_K_CASE_STMT:
    case NIU_K_DEFAULT_STMT:
        ok = s == niu_last_kid(p);
        break;
    default:
        ok = 0; /* a deferred statement, a declaration, a condition... */
        break;
    }
    if (!ok)
        return;
    for (i = 0; i < vc->wrapped.count; i++)
        if (vc->wrapped.items[i] == s)
            return;
    niu_nlist_push(&vc->wrapped, s);
    e = niu_stmt_end(c, s);
    if (e <= s->b || c->src[e - 1] != ';')
        return;
    niu_ledit_text(c, niu_owner_of(s), s->b, s->b, niu_strdup("{ "));
    niu_ledit_text(c, niu_owner_of(s), e, e, niu_strdup(" }"));
}

/* The macro use a node comes from, or NULL. */
static niu_expansion *niu_v_macro_of(niu_dctx *c, niu_node *n)
{
    return n->from_macro ? niu_expansion_at(c, n->b) : NULL;
}

/* Where node n is written in the niu file, [*b, *e): its extent, or, in a
 * call built by a macro, its text as a macro argument in the use. Returns 0
 * when it comes from a macro's definition (or a macro inside the argument),
 * which niu cannot rewrite (constraint 6). */
static int niu_v_spelled(niu_dctx *c, niu_node *n, size_t *b, size_t *e)
{
    niu_expansion *x = niu_v_macro_of(c, n);
    CXFile f = NULL;
    unsigned line, col, off = 0;
    if (!x) {
        *b = n->b;
        *e = niu_end_of(c, n);
        return 1;
    }
    /* an argument is spelled inside the use, after the macro's name (libclang
     * before 19 gives the use's start as the spelling of the body's tokens) */
    if (!n->sb_main || n->sb <= x->b + strlen(x->name) || n->sb >= x->e)
        return 0;
    cx.clang_getSpellingLocation(cx.clang_getRangeEnd(cx.clang_getCursorExtent(n->cur)), &f, &line, &col, &off);
    if (!niu_in_main(c, f))
        return 0;
    *b = n->sb;
    *e = niu_pp_to_src(c->pp, off);
    return *e > *b && *e <= x->e;
}

/* The definition of the macro used at x, or a null cursor. */
static CXCursor niu_v_macro_def(niu_dctx *c, niu_expansion *x)
{
    size_t i;
    for (i = 0; i < c->macro_names.count; i++)
        if (strcmp(c->macro_names.items[i], x->name) == 0)
            return c->macro_defs[i];
    return cx.clang_getCursorReferenced(x->cur);
}

static void niu_v_macro_note(niu_dctx *c, niu_diag *d, niu_expansion *x)
{
    CXCursor def = niu_v_macro_def(c, x);
    size_t off = cx.clang_Cursor_isNull(def) ? (size_t)-1 : niu_cursor_off(c, def);
    if (off != (size_t)-1)
        niu_dnote(d, off, "'%s' is defined here", x->name);
}

/* Does the macro's definition turn an argument into a string (`#x`)? */
static int niu_v_macro_stringifies(niu_dctx *c, niu_expansion *x)
{
    char *body = niu_macro_body(c, niu_v_macro_def(c, x));
    size_t i, n;
    int yes = 0;
    if (!body)
        return 0;
    n = strlen(body);
    for (i = 0; i + 1 < n; i++) {
        if (body[i] == '"' || body[i] == '\'') {
            char q = body[i];
            for (i++; i < n && body[i] != q; i++)
                if (body[i] == '\\')
                    i++;
            continue;
        }
        if (body[i] == '#' && body[i + 1] == '#') {
            i++;
            continue;
        }
        if (body[i] == '#') {
            size_t k = i + 1;
            while (k < n && (body[k] == ' ' || body[k] == '\t'))
                k++;
            if (k < n && niu_is_ident_start(body[k]))
                yes = 1;
        }
    }
    free(body);
    return yes;
}

/* The format string of a literal as text, from [b, e) of the niu file, or
 * from its spelling when it comes from a macro. Returns 0 for a wide
 * literal. */
static int niu_v_literal_chars(niu_dctx *c, niu_node *lit, int spelled, size_t b, size_t e, niu_lchars *lc)
{
    niu_lexer lx;
    if (!spelled) {
        char *s = niu_cxstr(cx.clang_getCursorSpelling(lit->cur));
        int ok = niu_decode_literal(s, 0, strlen(s), lc);
        free(s);
        return ok;
    }
    niu_lex_init(&lx, c->src, e > c->len ? c->len : e, b);
    lx.bol = 0;
    for (;;) {
        niu_tok t = niu_lex_next(&lx);
        if (t.kind == NIU_TOK_EOF)
            break;
        if (t.kind != NIU_TOK_STRING || !niu_decode_literal(c->src, t.b, t.e, lc))
            return 0; /* not a narrow literal */
        /* off[count]: the end of the last character, before the '"' */
        niu_lchars_push(lc, 0, t.e - 1);
        lc->count--;
    }
    return lc->count > 0;
}

/* -Wniu-v-no-format: a %v in a string passed to a variadic function that is
 * not printf-style. */
static void niu_v_no_format(niu_vctx *vc, niu_node *call, CXCursor f)
{
    niu_dctx *c = vc->c;
    CXType ft = cx.clang_getCursorType(f);
    size_t i;
    if (!c->warn_v_no_format || ft.kind != NIU_T_FUNCTION_PROTO || !cx.clang_isFunctionTypeVariadic(ft))
        return;
    for (i = 1; i < call->nkids; i++) {
        niu_node *lit = call->kids[i];
        niu_lchars lc;
        niu_convs cv;
        size_t b = 0, e = 0, k;
        int spelled;
        while (lit && (lit->kind == NIU_K_UNEXPOSED_EXPR || lit->kind == NIU_K_PAREN_EXPR) && lit->nkids == 1)
            lit = lit->kids[0];
        if (!lit || lit->kind != NIU_K_STRING_LITERAL)
            continue;
        memset(&lc, 0, sizeof lc);
        memset(&cv, 0, sizeof cv);
        spelled = niu_v_spelled(c, lit, &b, &e);
        if (niu_v_literal_chars(c, lit, spelled, b, e, &lc))
            niu_parse_format(&lc, 0, &cv);
        for (k = 0; k < cv.count; k++)
            if (cv.items[k].conv == 'v') {
                char *name = niu_cxstr(cx.clang_getCursorSpelling(f));
                size_t fo = niu_cursor_off(c, f);
                niu_diag *d = niu_ddiag(
                    c, "warning", spelled ? cv.items[k].b : lit->b,
                    "'%%v' in a string passed to '%s', which niu does not know as a printf-style function "
                    "[-Wniu-v-no-format]",
                    name);
                if (fo != (size_t)-1)
                    niu_dnote(d, fo, "add '__attribute__((format(printf, %d, %d)))' to '%s' to use '%%v' with it",
                              (int)i, cx.clang_getNumArgTypes(ft) + 1, name);
                free(name);
                break;
            }
        free(lc.ch);
        free(lc.off);
        free(cv.items);
    }
}

/* Reports a %v that niu cannot rewrite because of a macro (constraint 6).
 * Returns 1 when it did. */
static int niu_v_macro_error(niu_vctx *vc, niu_node *n, size_t v_off, int in_def)
{
    niu_dctx *c = vc->c;
    niu_expansion *x = niu_v_macro_of(c, n);
    niu_diag *d;
    if (!x)
        return 0;
    if (in_def) {
        d = niu_ddiag(c, "error", v_off, "'%%v' cannot be used in a macro definition");
    } else {
        d = niu_ddiag(c, "error", v_off, "'%%v' cannot be used here: the macro '%s' turns its argument into a string",
                      x->name);
    }
    niu_v_macro_note(c, d, x);
    return 1;
}

/* Constraint 5: a struct, union or enum argument has a file-scope type with
 * a name. Returns 0 after reporting it. */
static int niu_v_check_named(niu_dctx *c, CXType t, size_t v_off, size_t arg_off)
{
    char *print = NULL, *key = NULL;
    int local = 0, named = niu_v_named(c, t, &print, &key, &local);
    niu_diag *d = NULL;
    if (local) {
        char *ts = niu_cxstr(cx.clang_getTypeSpelling(cx.clang_getUnqualifiedType(t)));
        d = niu_ddiag(c, "error", v_off, "'%%v' cannot print '%s': it is declared inside a function", ts);
        free(ts);
    } else if (!named) {
        d = niu_ddiag(c, "error", v_off, "'%%v' cannot print a value of an unnamed type; give it a typedef name");
    }
    niu_dnote(d, arg_off, "the argument is here");
    free(print);
    free(key);
    return !local && named;
}

static void niu_v_call(niu_vctx *vc, niu_node *call)
{
    niu_dctx *c = vc->c;
    niu_node *callee, *lit;
    CXCursor f;
    int kind, fmt, nargs, has_v = 0, any_positional = 0, lit_spelled;
    niu_lchars lc;
    niu_convs cv;
    size_t i, lb = 0, le = 0;
    char *fname;
    niu_expansion *mx;

    if (call->nkids == 0)
        return;
    callee = niu_strip_expr(call->kids[0]);
    if (!callee || callee->kind != NIU_K_DECL_REF_EXPR)
        return;
    f = cx.clang_getCursorReferenced(callee->cur);
    if (cx.clang_Cursor_isNull(f) || cx.clang_getCursorKind(f) != NIU_K_FUNCTION_DECL)
        return;
    kind = niu_fmt_kind(c, f, &fmt);
    if (kind == NIU_FMT_NONE) {
        niu_v_no_format(vc, call, f);
        return;
    }
    nargs = (int)call->nkids - 1;
    if (fmt >= nargs)
        return;
    lit = call->kids[1 + fmt];
    while (lit && (lit->kind == NIU_K_UNEXPOSED_EXPR || lit->kind == NIU_K_PAREN_EXPR) && lit->nkids == 1)
        lit = lit->kids[0];
    if (!lit || lit->kind != NIU_K_STRING_LITERAL)
        return; /* constraint 2: a format string built at run time */

    memset(&lc, 0, sizeof lc);
    memset(&cv, 0, sizeof cv);
    lit_spelled = niu_v_spelled(c, lit, &lb, &le);
    if (niu_v_literal_chars(c, lit, lit_spelled, lb, le, &lc))
        niu_parse_format(&lc, fmt + 1, &cv);
    for (i = 0; i < cv.count; i++) {
        if (cv.items[i].conv == 'v')
            has_v = 1;
        if (cv.items[i].positional)
            any_positional = 1;
    }
    if (!has_v)
        goto done;
    /* constraint 6: the format string itself */
    mx = niu_v_macro_of(c, call);
    if (!lit_spelled) {
        niu_v_macro_error(vc, lit, lit->b, 1);
        goto done;
    }
    if (mx && niu_v_macro_stringifies(c, mx)) {
        niu_v_macro_error(vc, call, cv.count ? cv.items[0].b : lb, 0);
        goto done;
    }
    /* a macro that uses its argument twice makes two calls of one literal */
    for (i = 0; i < vc->n_lits; i++)
        if (vc->lits[i] == lb) {
            niu_diag *d = NULL;
            size_t k;
            for (k = 0; k < cv.count && cv.items[k].conv != 'v'; k++)
                ;
            if (mx)
                d = niu_ddiag(c, "error", k < cv.count ? cv.items[k].b : lb,
                              "'%%v' cannot be used here: the macro '%s' uses its argument more than once", mx->name);
            if (mx)
                niu_v_macro_note(c, d, mx);
            goto done;
        }
    vc->lits = (size_t *)niu_xrealloc(vc->lits, (vc->n_lits + 1) * sizeof *vc->lits);
    vc->lits[vc->n_lits++] = lb;
    fname = niu_cxstr(cx.clang_getCursorSpelling(f));

    for (i = 0; i < cv.count; i++) {
        niu_conv *v = &cv.items[i];
        niu_node *arg;
        niu_v_need *nd;
        CXType t;
        niu_buf pre = {0}, conv = {0};
        long long bound;
        char *size_expr, *gname = NULL, *err = NULL;
        int owner, ck, is_ptr;
        size_t ab, ae, pb = 0, pe = 0;
        if (v->conv != 'v')
            continue;
        /* constraint 1 */
        if (kind == NIU_FMT_VA_LIST) {
            niu_ddiag(c, "error", v->b, "'%%v' cannot be used with '%s': the types of a 'va_list' are not known",
                      fname);
            continue;
        }
        if (kind == NIU_FMT_SCANF) {
            niu_ddiag(c, "error", v->b, "'%%v' cannot be used in a scanf format string");
            continue;
        }
        /* constraint 7 */
        if (v->bad_flag) {
            niu_ddiag(c, "error", v->b, "'%%v' cannot take the '%c' flag; only a width, '-' and a precision are allowed",
                      v->bad_flag);
            continue;
        }
        if (v->length) {
            niu_ddiag(c, "error", v->b, "'%%v' cannot take a length modifier: the type of the argument is already known");
            continue;
        }
        if (any_positional) {
            niu_ddiag(c, "error", v->b, "'%%v' cannot take a positional argument");
            continue;
        }
        /* constraint 3 */
        if (v->arg_value >= nargs || v->arg_prec >= nargs || v->arg_width >= nargs) {
            niu_ddiag(c, "error", v->b, "'%%v' has no matching argument");
            continue;
        }
        arg = call->kids[1 + v->arg_value];
        /* constraint 6: the arguments */
        if (!niu_v_spelled(c, arg, &ab, &ae) ||
            (v->arg_prec >= 0 && !niu_v_spelled(c, call->kids[1 + v->arg_prec], &pb, &pe))) {
            if (!niu_v_macro_error(vc, arg, v->b, 1))
                niu_ddiag(c, "error", v->b, "'%%v' cannot be used in a macro definition");
            continue;
        }
        t = niu_unpromoted_type(arg);
        ck = cx.clang_getCanonicalType(t).kind;
        /* constraint 5 */
        if ((ck == NIU_T_RECORD || ck == NIU_T_ENUM) && !niu_v_check_named(c, t, v->b, ab))
            continue;
        /* constraint 4, and the nesting the runtime can follow */
        if (niu_v_depth(t) > 30) {
            niu_diag *d = niu_ddiag(c, "error", v->b, "'%%v' cannot print a type nested more than 30 levels deep");
            niu_dnote(d, ab, "the argument is here");
            continue;
        }
        nd = niu_v_need_for(vc, niu_top_of(call), v->b);
        if (!niu_gen_top(vc, nd, t, &gname, &err)) {
            niu_diag *d = niu_ddiag(c, "error", v->b, "'%%v' cannot print a value of type '%s'", err ? err : "?");
            niu_dnote(d, ab, "the argument is here");
            free(err);
            continue;
        }
        is_ptr = ck == NIU_T_POINTER;

        /* the conversion becomes %s, keeping the width and '-' */
        niu_buf_putc(&conv, '%');
        if (v->minus)
            niu_buf_putc(&conv, '-');
        if (v->width_star)
            niu_buf_putc(&conv, '*');
        else if (v->we > v->wb)
            niu_buf_append(&conv, c->src + v->wb, v->we - v->wb);
        niu_buf_putc(&conv, 's');
        owner = niu_owner_of(call);
        niu_ledit_text(c, owner, v->b, v->e, niu_buf_take(&conv));

        /* the buffer: the longest output, or the limit */
        bound = niu_v_bound(c, t, v->prec_star ? -1 : v->has_prec ? v->prec : 2);
        size_expr = bound < 0 || bound >= ((long long)1 << 40)
                        ? niu_strdup("NIU_V_LIMIT")
                        : niu_format("(%lld) < NIU_V_LIMIT ? (%lld) : NIU_V_LIMIT", bound + 1, bound + 1);
        niu_buf_printf(&pre, "__niu_v_%s((char[%s]){0}, %s, ", gname, size_expr, size_expr);
        if (v->prec_star) {
            niu_buf_append(&pre, c->src + pb, pe - pb);
            niu_buf_puts(&pre, ", ");
        } else {
            niu_buf_printf(&pre, "%ld, ", v->has_prec ? v->prec : 2);
        }
        if (is_ptr)
            niu_buf_puts(&pre, "(unsigned long long)(size_t)(");
        /* a '*' precision moves into the call: its text up to the value goes */
        niu_ledit_text(c, owner, v->prec_star ? pb : ab, ab, niu_buf_take(&pre));
        niu_ledit_text(c, owner, ae, ae, niu_strdup(is_ptr ? "))" : ")"));
        free(size_expr);
        free(gname);
        niu_buf_free(&pre);
        niu_v_wrap(vc, call);
    }
    free(fname);
done:
    free(lc.ch);
    free(lc.off);
    free(cv.items);
}

/* ---- where the generated functions go ---- */

typedef struct {
    niu_vctx *vc;
    size_t last_main; /* the offset of the last declaration in the niu file seen */
} niu_stdio_ctx;

static enum CXChildVisitResult niu_stdio_visit(CXCursor cur, CXCursor parent, CXClientData data)
{
    niu_stdio_ctx *sc = (niu_stdio_ctx *)data;
    niu_dctx *c = sc->vc->c;
    size_t off = niu_cursor_off(c, cur);
    int k = cx.clang_getCursorKind(cur);
    (void)parent;
    if (k >= 500 && k < 600)
        return CXChildVisit_Continue; /* preprocessing: not in declaration order */
    if (off != (size_t)-1)
        sc->last_main = off;
    if (cx.clang_getCursorKind(cur) == NIU_K_FUNCTION_DECL && niu_cursor_named(cur, "snprintf")) {
        sc->vc->stdio_from = off != (size_t)-1 ? off : sc->last_main;
        return CXChildVisit_Break;
    }
    return CXChildVisit_Continue;
}

/* Is snprintf declared before offset off (constraint 8)? */
static int niu_v_has_stdio(niu_vctx *vc, size_t off)
{
    if (!vc->stdio_done) {
        niu_stdio_ctx sc;
        vc->stdio_done = 1;
        vc->stdio_from = (size_t)-1;
        sc.vc = vc;
        sc.last_main = 0;
        cx.clang_visitChildren(cx.clang_getTranslationUnitCursor(vc->c->tu), niu_stdio_visit, &sc);
    }
    return vc->stdio_from != (size_t)-1 && vc->stdio_from <= off;
}

static void niu_v_emit(niu_vctx *vc)
{
    niu_dctx *c = vc->c;
    size_t i, k;
    int first = 1;
    for (i = 0; i < vc->n_needs; i++) {
        niu_v_need *nd = &vc->needs[i];
        niu_buf b = {0};
        size_t at = nd->fn->b, ls = niu_line_start(c->src, at);
        unsigned line, col;
        char *fname;
        if (!nd->texts.count)
            continue;
        for (k = ls; k < at && (c->src[k] == ' ' || c->src[k] == '\t'); k++)
            ;
        if (k == at)
            at = ls;
        if (!niu_v_has_stdio(vc, at)) {
            /* constraint 8 */
            size_t fo = niu_cursor_off(c, nd->fn->cur);
            niu_diag *d;
            fname = niu_cxstr(cx.clang_getCursorSpelling(nd->fn->cur));
            d = niu_ddiag(c, "error", nd->first_v, "'%%v' needs <stdio.h>; include it before '%s'", fname);
            niu_dnote(d, fo == (size_t)-1 ? nd->fn->b : fo, "'%s' is here", fname);
            free(fname);
            continue;
        }
        if (at != ls)
            niu_buf_puts(&b, c->eol);
        if (first) {
            niu_put_eol_text(&b, niu_v_runtime_head, c->eol);
            first = 0;
        }
        for (k = 0; k < nd->rts.count; k++)
            niu_v_piece_out(&b, &vc->emitted, nd->rts.items[k], c->eol);
        for (k = 0; k < nd->texts.count; k++)
            niu_put_eol_text(&b, nd->texts.items[k], c->eol);
        niu_dline_col(c, nd->fn->b, &line, &col);
        if (!c->no_line) {
            niu_buf_printf(&b, "#line %u \"", line);
            niu_append_string_literal(&b, niu_basename(c->input));
            niu_buf_printf(&b, "\"%s", c->eol);
        }
        if (at != ls) {
            /* the rest of the line goes on at its column */
            for (k = ls; k < at; k++)
                niu_buf_putc(&b, ' ');
        }
        niu_ledit_text(c, -1, at, at, niu_buf_take(&b));
    }
}

/* Every printf-style call in the niu file. */
static void niu_v_analyze(niu_dctx *c)
{
    niu_vctx vc;
    size_t i;
    memset(&vc, 0, sizeof vc);
    vc.c = c;
    for (i = 0; i < c->all_nodes.count; i++) {
        niu_node *n = c->all_nodes.items[i];
        if (n->kind == NIU_K_CALL_EXPR)
            niu_v_call(&vc, n);
    }
    niu_v_emit(&vc);
    for (i = 0; i < vc.n_needs; i++) {
        niu_strlist_free(&vc.needs[i].rts);
        niu_strlist_free(&vc.needs[i].texts);
    }
    free(vc.needs);
    niu_strlist_free(&vc.generated);
    niu_strlist_free(&vc.emitted);
    free(vc.wrapped.items);
    free(vc.lits);
}
