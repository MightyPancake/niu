/* defer.c - the defer statement (spec.md "Defer statement"): finding the
 * defers and every exit they cover in the AST, checking the constraints, and
 * describing the edits that lower them. lower.c turns those edits into text.
 * Part of niu.c; not compiled on its own.
 *
 * Each function in the niu file is copied out of libclang into a small tree
 * of niu_node (cursor kind, extent, kids), which is easier to walk in all
 * directions than libclang's visitor. Defers are IfStmt nodes at a defer
 * marker of the pre-pass, defer_now is a ParenExpr at a defer_now marker. */

#define NIU_N_DEFER 900
#define NIU_N_DEFER_NOW 901

/* clang cursor kinds used here (enum CXCursorKind) */
#define NIU_K_STRUCT_DECL 2
#define NIU_K_UNION_DECL 3
#define NIU_K_ENUM_DECL 5
#define NIU_K_FIELD_DECL 6
#define NIU_K_ENUM_CONSTANT_DECL 7
#define NIU_K_FUNCTION_DECL 8
#define NIU_K_VAR_DECL 9
#define NIU_K_PARM_DECL 10
#define NIU_K_TYPEDEF_DECL 20
#define NIU_K_TYPE_REF 43
#define NIU_K_LABEL_REF 48
#define NIU_K_UNEXPOSED_EXPR 100
#define NIU_K_DECL_REF_EXPR 101
#define NIU_K_CALL_EXPR 103
#define NIU_K_INTEGER_LITERAL 106
#define NIU_K_PAREN_EXPR 111
#define NIU_K_CSTYLE_CAST_EXPR 117
#define NIU_K_STMT_EXPR 121
#define NIU_K_LABEL_STMT 201
#define NIU_K_COMPOUND_STMT 202
#define NIU_K_CASE_STMT 203
#define NIU_K_DEFAULT_STMT 204
#define NIU_K_IF_STMT 205
#define NIU_K_SWITCH_STMT 206
#define NIU_K_WHILE_STMT 207
#define NIU_K_DO_STMT 208
#define NIU_K_FOR_STMT 209
#define NIU_K_GOTO_STMT 210
#define NIU_K_INDIRECT_GOTO_STMT 211
#define NIU_K_CONTINUE_STMT 212
#define NIU_K_BREAK_STMT 213
#define NIU_K_RETURN_STMT 214
#define NIU_K_NULL_STMT 230
#define NIU_K_DECL_STMT 231
#define NIU_K_MACRO_DEFINITION 501
#define NIU_K_MACRO_EXPANSION 502

#define NIU_CX_TYPE_VOID 2
#define NIU_CX_SC_STATIC 3

/* ---- the tree ---- */

typedef struct niu_node niu_node;
struct niu_node {
    int kind; /* a cursor kind, or NIU_N_DEFER / NIU_N_DEFER_NOW */
    CXCursor cur;
    size_t b, e;    /* extent in the niu file, as expanded (offsets) */
    size_t sb;      /* where the start is spelled */
    int sb_main;    /* ... and whether that is in the niu file */
    int from_macro; /* the start comes from a macro (body or argument) */
    niu_node *parent;
    niu_node **kids;
    size_t nkids, capkids;
    size_t index; /* among the parent's kids */
    int defer;    /* NIU_N_DEFER: index into niu_dctx.defers */
    size_t loc;   /* LabelStmt: offset of the cursor's location */
    niu_marker *marker;
};

typedef struct {
    niu_node **items;
    size_t count, cap;
} niu_nlist;

static void niu_nlist_push(niu_nlist *l, niu_node *n)
{
    if (l->count == l->cap) {
        l->cap = l->cap ? 2 * l->cap : 16;
        l->items = (niu_node **)niu_xrealloc(l->items, l->cap * sizeof *l->items);
    }
    l->items[l->count++] = n;
}

typedef struct {
    int *items;
    size_t count, cap;
} niu_ilist;

static void niu_ilist_push(niu_ilist *l, int v)
{
    if (l->count == l->cap) {
        l->cap = l->cap ? 2 * l->cap : 8;
        l->items = (int *)niu_xrealloc(l->items, l->cap * sizeof *l->items);
    }
    l->items[l->count++] = v;
}

/* A macro use in the niu file. */
typedef struct {
    size_t b, e;
    char *name;
    CXCursor cur;
} niu_expansion;

/* A name the deferred statement uses (constraint 6). */
typedef struct {
    char *name;
    int tag; /* struct/union/enum tag rather than an ordinary name */
    CXCursor decl;
} niu_used_name;

enum { NIU_DS_PLAIN, NIU_DS_KEYWORD_MACRO, NIU_DS_COMPANION };

typedef struct {
    niu_node *node;  /* the NIU_N_DEFER node */
    niu_node *stmt;  /* the deferred statement */
    int shape;       /* NIU_DS_* */
    int owner;       /* the defer whose deferred statement contains this one, or -1 */
    size_t b, e;     /* the text the comment replaces (PLAIN, KEYWORD_MACRO) */
    size_t sb, se;   /* the deferred statement, with its ';' */
    niu_define *def; /* KEYWORD_MACRO, COMPANION */
    size_t use_b, use_e; /* COMPANION: the macro use, e.g. "AUTOFREE(buf)" */
    int use_semi;        /* COMPANION: a ';' follows the use */
    niu_used_name *names;
    size_t n_names;
    niu_strlist macros; /* macros it uses, directly or through other macros */
    niu_strlist macros_reported; /* constraint 11 errors given for these */
    int bad;            /* broke a constraint; not lowered */
    /* lower.c: the copy, made once */
    int copy_done;
    char *copy;
    int copy_block;
    unsigned copy_line;
    char *copy_indent;
} niu_defer;

/* niu's own diagnostics, printed after libclang's. */
typedef struct {
    size_t off;
    const char *sev; /* "error" or "warning" */
    char *msg;
    size_t note_off[2];
    char *note[2];
    int n_notes;
} niu_diag;

/* What lowering has to do at one place. lower.c renders it. */
enum {
    NIU_L_TEXT,       /* replace [b, e) with text */
    NIU_L_END,        /* copies before the '}' at b */
    NIU_L_JUMP,       /* break/continue/goto [b, e); the statement is [xb, xe) */
    NIU_L_RETURN,     /* return [b, e); the value is [xb, xe) */
    NIU_L_DEFER_NOW,  /* defer_now; [b, e) */
    NIU_L_VALUE,      /* last statement [b, e) of a statement expression; its value is [xb, xe) */
    NIU_L_VALUE_AFTER /* copies after the last statement of a statement expression, at b */
};

typedef struct {
    int kind;
    int owner; /* the defer whose deferred statement contains this, or -1 */
    size_t b, e;
    size_t xb, xe;
    char *text;      /* NIU_L_TEXT */
    char *decl;      /* NIU_L_RETURN/VALUE: "int __niu_return_temp", or NULL */
    int void_value;  /* NIU_L_RETURN: a value in a void function (return f();) */
    niu_ilist copies; /* defers to copy, in the order they run */
    size_t seq;      /* creation order, to keep sorting stable */
} niu_ledit;

typedef struct {
    CXTranslationUnit tu;
    CXFile main_file;
    const char *input; /* the file name, for messages */
    const char *src;   /* the original text */
    size_t len;
    const char *eol;
    niu_prepass *pp;
    int no_line;
    int warn_empty_defer_now;

    niu_nlist all_nodes; /* every node, to free them */
    niu_nlist labels;    /* every LabelStmt node */
    niu_nlist functions; /* the FunctionDecl nodes with a body */
    niu_expansion *exps;
    size_t n_exps, cap_exps;
    niu_strlist macro_names; /* every macro defined anywhere ... */
    CXCursor *macro_defs;    /* ... and its (last) definition */

    niu_defer *defers;
    size_t n_defers, cap_defers;

    niu_diag *diags;
    size_t n_diags, cap_diags;
    int errors;

    niu_ledit *ledits;
    size_t n_ledits, cap_ledits;
    niu_ledit **sorted_ledits; /* lower.c: by owner, then position */
    size_t *line_starts;       /* offsets where lines start, made on first use */
    size_t n_lines;
    int internal_error;
    size_t *clang_skip; /* offsets where libclang's errors are left out: niu reports them */
    size_t n_clang_skip;
    size_t n_dollar; /* declarations or macros named '$' (reserved) */

    niu_strlist typedef_names; /* every file-scope typedef ... */
    CXCursor *typedefs;        /* ... and its declaration */
    int typedefs_done;

    /* noreturn functions, by name, cached */
    niu_strlist noreturn_yes, noreturn_no;
} niu_dctx;

/* ---- lines ---- */

/* 1-based line and column of offset off, through a table of line starts. */
static void niu_dline_col(niu_dctx *c, size_t off, unsigned *line, unsigned *col)
{
    size_t lo = 0, hi;
    if (!c->line_starts) {
        size_t i, cap = 64;
        c->line_starts = (size_t *)niu_xmalloc(cap * sizeof *c->line_starts);
        c->line_starts[c->n_lines++] = 0;
        for (i = 0; i < c->len; i++)
            if (c->src[i] == '\n') {
                if (c->n_lines == cap) {
                    cap *= 2;
                    c->line_starts = (size_t *)niu_xrealloc(c->line_starts, cap * sizeof *c->line_starts);
                }
                c->line_starts[c->n_lines++] = i + 1;
            }
    }
    if (off > c->len)
        off = c->len;
    hi = c->n_lines;
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (c->line_starts[mid] <= off)
            lo = mid;
        else
            hi = mid;
    }
    *line = (unsigned)(lo + 1);
    *col = (unsigned)(off - c->line_starts[lo] + 1);
}

/* ---- diagnostics ---- */

static niu_diag *niu_ddiag(niu_dctx *c, const char *sev, size_t off, const char *fmt, ...)
{
    niu_diag *d;
    va_list ap;
    niu_buf b = {0};
    size_t i;
    va_start(ap, fmt);
    niu_buf_vprintf(&b, fmt, ap);
    va_end(ap);
    for (i = 0; i < c->n_diags; i++)
        if (c->diags[i].off == off && strcmp(c->diags[i].msg, niu_buf_str(&b)) == 0) {
            niu_buf_free(&b);
            return NULL; /* reported already */
        }
    if (c->n_diags == c->cap_diags) {
        c->cap_diags = c->cap_diags ? 2 * c->cap_diags : 16;
        c->diags = (niu_diag *)niu_xrealloc(c->diags, c->cap_diags * sizeof *c->diags);
    }
    d = &c->diags[c->n_diags++];
    memset(d, 0, sizeof *d);
    d->off = off;
    d->sev = sev;
    d->msg = niu_buf_take(&b);
    if (strcmp(sev, "error") == 0)
        c->errors++;
    return d;
}

static void niu_dnote(niu_diag *d, size_t off, const char *fmt, ...)
{
    va_list ap;
    niu_buf b = {0};
    if (!d || d->n_notes >= 2)
        return;
    va_start(ap, fmt);
    niu_buf_vprintf(&b, fmt, ap);
    va_end(ap);
    d->note_off[d->n_notes] = off;
    d->note[d->n_notes] = niu_buf_take(&b);
    d->n_notes++;
}

static int niu_diag_cmp(const void *a, const void *b)
{
    const niu_diag *x = (const niu_diag *)a, *y = (const niu_diag *)b;
    if (x->off != y->off)
        return x->off < y->off ? -1 : 1;
    return 0;
}

static void niu_print_ddiag_line(niu_dctx *c, size_t off, const char *sev, const char *msg)
{
    unsigned line, col;
    niu_dline_col(c, off, &line, &col);
    fprintf(stderr, "%s:%u:%u: %s: %s\n", c->input, line, col, sev, msg);
}

static void niu_print_ddiags(niu_dctx *c)
{
    size_t i;
    int k;
    /* stable sort: insertion order breaks ties */
    for (i = 1; i < c->n_diags; i++) {
        size_t j = i;
        while (j > 0 && niu_diag_cmp(&c->diags[j - 1], &c->diags[j]) > 0) {
            niu_diag t = c->diags[j - 1];
            c->diags[j - 1] = c->diags[j];
            c->diags[j] = t;
            j--;
        }
    }
    for (i = 0; i < c->n_diags; i++) {
        niu_diag *d = &c->diags[i];
        niu_print_ddiag_line(c, d->off, d->sev, d->msg);
        for (k = 0; k < d->n_notes; k++)
            niu_print_ddiag_line(c, d->note_off[k], "note", d->note[k]);
    }
}

/* ---- locations ---- */

static int niu_in_main(niu_dctx *c, CXFile f)
{
    return f && cx.clang_File_isEqual(f, c->main_file);
}

static niu_expansion *niu_expansion_at(niu_dctx *c, size_t off);

static void niu_node_locate(niu_dctx *c, niu_node *n)
{
    CXSourceRange r = cx.clang_getCursorExtent(n->cur);
    CXFile f = NULL;
    unsigned line, col, off = 0;
    cx.clang_getExpansionLocation(cx.clang_getRangeStart(r), &f, &line, &col, &off);
    n->b = off;
    cx.clang_getExpansionLocation(cx.clang_getRangeEnd(r), &f, &line, &col, &off);
    n->e = off < n->b ? n->b : off;
    f = NULL;
    off = 0;
    cx.clang_getSpellingLocation(cx.clang_getRangeStart(r), &f, &line, &col, &off);
    n->sb = off;
    n->sb_main = niu_in_main(c, f);
    /* Code from a macro use cannot be edited. (libclang before 19 gives the
     * use, not the #define, as the spelling of a macro's tokens, so the use's
     * extent is what tells.) */
    n->from_macro = niu_expansion_at(c, n->b) != NULL;
    if (n->e > c->len)
        n->e = c->len;
    if (n->b > n->e)
        n->b = n->e;
}

/* Offset of a cursor's location (expansion), or (size_t)-1 outside the file. */
static size_t niu_cursor_off(niu_dctx *c, CXCursor cur)
{
    CXFile f = NULL;
    unsigned line, col, off = 0;
    cx.clang_getExpansionLocation(cx.clang_getCursorLocation(cur), &f, &line, &col, &off);
    return niu_in_main(c, f) ? off : (size_t)-1;
}

/* The macro use whose expansion covers offset off, or NULL. Top-level macro
 * uses are recorded in file order and do not overlap. */
static niu_expansion *niu_expansion_at(niu_dctx *c, size_t off)
{
    size_t lo = 0, hi = c->n_exps;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (c->exps[mid].e <= off)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < c->n_exps && off >= c->exps[lo].b && off < c->exps[lo].e)
        return &c->exps[lo];
    return NULL;
}

/* Is there a macro use starting inside [b, e)? */
static int niu_has_expansion_in(niu_dctx *c, size_t b, size_t e)
{
    size_t lo = 0, hi = c->n_exps;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (c->exps[mid].b < b)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < c->n_exps && c->exps[lo].b < e;
}

/* ---- building the tree ---- */

static niu_node *niu_node_new(niu_dctx *c, CXCursor cur, niu_node *parent)
{
    niu_node *n = (niu_node *)niu_xmalloc(sizeof *n);
    memset(n, 0, sizeof *n);
    n->cur = cur;
    n->kind = cx.clang_getCursorKind(cur);
    n->parent = parent;
    n->defer = -1;
    niu_node_locate(c, n);
    niu_nlist_push(&c->all_nodes, n);
    if (n->kind == NIU_K_LABEL_STMT) {
        n->loc = niu_cursor_off(c, cur);
        niu_nlist_push(&c->labels, n);
    }
    if (parent) {
        if (parent->nkids == parent->capkids) {
            parent->capkids = parent->capkids ? 2 * parent->capkids : 4;
            parent->kids = (niu_node **)niu_xrealloc(parent->kids, parent->capkids * sizeof *parent->kids);
        }
        n->index = parent->nkids;
        parent->kids[parent->nkids++] = n;
    }
    return n;
}

typedef struct {
    niu_dctx *c;
    niu_node *parent;
} niu_build_ctx;

static enum CXChildVisitResult niu_build_visit(CXCursor cur, CXCursor parent, CXClientData data)
{
    niu_build_ctx *bc = (niu_build_ctx *)data;
    niu_build_ctx sub;
    niu_node *n;
    (void)parent;
    if (cx.clang_isAttribute(cx.clang_getCursorKind(cur)))
        return CXChildVisit_Continue;
    n = niu_node_new(bc->c, cur, bc->parent);
    sub.c = bc->c;
    sub.parent = n;
    cx.clang_visitChildren(cur, niu_build_visit, &sub);
    return CXChildVisit_Continue;
}

static int niu_is_expr(int k)
{
    return k >= 100 && k < 200;
}

static int niu_is_loop(int k)
{
    return k == NIU_K_WHILE_STMT || k == NIU_K_DO_STMT || k == NIU_K_FOR_STMT;
}

static niu_node *niu_last_kid(niu_node *n)
{
    return n && n->nkids ? n->kids[n->nkids - 1] : NULL;
}

/* Is a inside b (or b itself)? */
static int niu_within(niu_node *a, niu_node *b)
{
    for (; a; a = a->parent)
        if (a == b)
            return 1;
    return 0;
}

/* The deferred statement that contains n (n inside its statement), or NULL. */
static niu_node *niu_enclosing_defer(niu_node *n)
{
    niu_node *child = n, *p = n->parent;
    for (; p; child = p, p = p->parent)
        if (p->kind == NIU_N_DEFER && p->nkids >= 2 && child == p->kids[1])
            return p;
    return NULL;
}

/* The function body (CompoundStmt) a node is in. */
static niu_node *niu_function_body(niu_node *n)
{
    while (n->parent && n->parent->kind != NIU_K_FUNCTION_DECL)
        n = n->parent;
    return n;
}

/* ---- text helpers ---- */

/* Does the statement end in a ';' that is not part of its extent? */
static int niu_needs_semicolon(niu_node *n)
{
    switch (n->kind) {
    case NIU_K_COMPOUND_STMT:
    case NIU_K_DECL_STMT:
    case NIU_K_NULL_STMT:
        return 0;
    case NIU_K_IF_STMT:
    case NIU_N_DEFER:
    case NIU_K_WHILE_STMT:
    case NIU_K_FOR_STMT:
    case NIU_K_SWITCH_STMT:
    case NIU_K_LABEL_STMT:
    case NIU_K_CASE_STMT:
    case NIU_K_DEFAULT_STMT:
        return niu_last_kid(n) ? niu_needs_semicolon(niu_last_kid(n)) : 1;
    default:
        return 1;
    }
}

/* The end of a statement including its ';'. */
/* The end of a node that ends in a macro use: libclang gives the use's start
 * as the (expansion) end of the macro's last token, so `return ID(v)` would
 * end before `ID`. The node then goes on to the end of the use. */
static size_t niu_end_of(niu_dctx *c, niu_node *n)
{
    niu_expansion *x = niu_expansion_at(c, n->e);
    return x && x->b == n->e ? x->e : n->e;
}

static size_t niu_stmt_end(niu_dctx *c, niu_node *n)
{
    size_t k, e = niu_end_of(c, n);
    if (!niu_needs_semicolon(n))
        return e;
    k = niu_next_code(c->src, c->len, e);
    if (k < c->len && c->src[k] == ';')
        return k + 1;
    return e;
}

/* The offset of a compound statement's '}' (or of the macro use it comes
 * from: the copies go right before that, spec.md "End of block"). */
static size_t niu_close_brace(niu_dctx *c, niu_node *n)
{
    size_t k = n->e > n->b ? n->e - 1 : n->b;
    niu_expansion *x;
    if (k < c->len && c->src[k] == '}' && !niu_expansion_at(c, k))
        return k;
    x = niu_expansion_at(c, k);
    if (x)
        return x->b;
    return k;
}

/* The declarator `<type> <name>`: the name goes before the first ')' of a
 * pointer-to-function or pointer-to-array spelling, else after the type. */
static char *niu_declarator(const char *type, const char *name)
{
    const char *p = strstr(type, "(*");
    if (p) {
        const char *q = strchr(p, ')');
        if (q)
            return niu_format("%.*s%s%s", (int)(q - type), type, name, q);
    }
    return niu_format("%s %s", type, name);
}

/* ---- the type of an expression, as written ---- */

/* Clang 22 and later spell some types with built-in sugar names that no
 * header declares (the type of sizeof is `__size_t`). Does s use one? */
static int niu_has_builtin_sugar(const char *s)
{
    static const char *const names[] = {"__size_t", "__signed_size_t", "__ptrdiff_t"};
    size_t i;
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        const char *p = s;
        size_t n = strlen(names[i]);
        while ((p = strstr(p, names[i])) != NULL) {
            if ((p == s || !niu_is_ident_char(p[-1])) && !niu_is_ident_char(p[n]))
                return 1;
            p += n;
        }
    }
    return 0;
}

static niu_node *niu_strip_expr(niu_node *n);

typedef struct {
    niu_dctx *c;
} niu_tdctx;

static enum CXChildVisitResult niu_typedef_visit(CXCursor cur, CXCursor parent, CXClientData data)
{
    niu_dctx *c = ((niu_tdctx *)data)->c;
    (void)parent;
    if (cx.clang_getCursorKind(cur) == NIU_K_TYPEDEF_DECL) {
        char *name = niu_cxstr(cx.clang_getCursorSpelling(cur));
        niu_strlist_push_owned(&c->typedef_names, name);
        c->typedefs = (CXCursor *)niu_xrealloc(c->typedefs, c->typedef_names.count * sizeof *c->typedefs);
        c->typedefs[c->typedef_names.count - 1] = cur;
    }
    return CXChildVisit_Continue;
}

/* The typedef named `name` at file scope, if its type is t (canonically). */
static int niu_typedef_matches(niu_dctx *c, const char *name, CXType t, CXType *out)
{
    size_t i;
    if (!c->typedefs_done) {
        niu_tdctx td;
        td.c = c;
        c->typedefs_done = 1;
        cx.clang_visitChildren(cx.clang_getTranslationUnitCursor(c->tu), niu_typedef_visit, &td);
    }
    for (i = c->typedef_names.count; i > 0; i--)
        if (strcmp(c->typedef_names.items[i - 1], name) == 0) {
            CXType tt = cx.clang_getCursorType(c->typedefs[i - 1]);
            if (cx.clang_equalTypes(cx.clang_getCanonicalType(tt), cx.clang_getCanonicalType(t))) {
                *out = tt;
                return 1;
            }
            return 0;
        }
    return 0;
}

/* The return type of function f as its declaration spells it. libclang gives
 * library builtins (strlen) their built-in, desugared type; the declaration
 * in the header usually says `size_t strlen(`, so the identifier right before
 * the name (after calling conventions) is tried as a typedef. */
static CXType niu_written_result_type(niu_dctx *c, CXCursor f)
{
    CXType rt = cx.clang_getCursorResultType(f);
    CXFile file = NULL;
    unsigned line, col, off = 0;
    size_t size = 0, k;
    const char *text;
    char *spelled = niu_cxstr(cx.clang_getTypeSpelling(rt));
    char *canon = niu_cxstr(cx.clang_getTypeSpelling(cx.clang_getCanonicalType(rt)));
    int sugared = strcmp(spelled, canon) != 0 && !niu_has_builtin_sugar(spelled);
    free(spelled);
    free(canon);
    if (sugared)
        return rt;
    cx.clang_getSpellingLocation(cx.clang_getCursorLocation(f), &file, &line, &col, &off);
    text = file ? cx.clang_getFileContents(c->tu, file, &size) : NULL;
    if (!text || off > size)
        return rt;
    k = off;
    for (;;) {
        size_t e, b;
        char *word;
        while (k > 0 && (text[k - 1] == ' ' || text[k - 1] == '\t' || text[k - 1] == '\n' || text[k - 1] == '\r'))
            k--;
        e = k;
        while (k > 0 && niu_is_ident_char(text[k - 1]))
            k--;
        b = k;
        if (b == e)
            return rt;
        word = niu_strndup(text + b, e - b);
        if (strcmp(word, "__cdecl") == 0 || strcmp(word, "__stdcall") == 0 || strcmp(word, "__fastcall") == 0 ||
            strcmp(word, "__vectorcall") == 0 || strcmp(word, "_cdecl") == 0 || strcmp(word, "__clrcall") == 0) {
            free(word);
            continue;
        }
        {
            CXType td;
            int ok = niu_typedef_matches(c, word, rt, &td);
            free(word);
            return ok ? td : rt;
        }
    }
}

/* The type of an expression, keeping the typedef it was declared with where
 * there is one (spec.md's example stores strlen(s) in a size_t). */
static CXType niu_expr_type(niu_dctx *c, niu_node *n)
{
    CXType t = cx.clang_getCursorType(n->cur), w = t;
    niu_node *s = niu_strip_expr(n);
    if (s && s->kind == NIU_K_CALL_EXPR && s->nkids) {
        niu_node *callee = niu_strip_expr(s->kids[0]);
        if (callee && callee->kind == NIU_K_DECL_REF_EXPR) {
            CXCursor f = cx.clang_getCursorReferenced(callee->cur);
            if (!cx.clang_Cursor_isNull(f) && cx.clang_getCursorKind(f) == NIU_K_FUNCTION_DECL)
                w = niu_written_result_type(c, f);
        }
    } else if (s && (s->kind == NIU_K_DECL_REF_EXPR || s->kind == 102 /* MemberRefExpr */ ||
                     s->kind == NIU_K_CSTYLE_CAST_EXPR)) {
        w = cx.clang_getCursorType(s->cur);
    }
    /* only if it is really the same type */
    if (cx.clang_equalTypes(cx.clang_getCanonicalType(cx.clang_getUnqualifiedType(w)),
                            cx.clang_getCanonicalType(cx.clang_getUnqualifiedType(t))))
        return w;
    return t;
}

/* The spelling of a type without qualifiers, or NULL (and *unnamed set) when
 * it cannot be written down (an unnamed struct). Sets *is_void. */
static char *niu_type_for_temp(CXType t, int *is_void, int *unnamed)
{
    CXType u = cx.clang_getUnqualifiedType(t);
    char *s = niu_cxstr(cx.clang_getTypeSpelling(u));
    if (niu_has_builtin_sugar(s)) {
        free(s);
        s = niu_cxstr(cx.clang_getTypeSpelling(cx.clang_getCanonicalType(u)));
    }
    *is_void = cx.clang_getCanonicalType(u).kind == NIU_CX_TYPE_VOID;
    *unnamed = strstr(s, "(unnamed") || strstr(s, "(anonymous");
    if (*unnamed) {
        free(s);
        return NULL;
    }
    return s;
}

/* ---- '$' ----
 * The pre-pass writes `if($)` and `((void)$)`, and niu.h declares the $
 * they refer to. A $ of the file's own would clash with it or hide it, so a
 * declaration of $ in the ordinary name space, or an object-like macro $, is
 * an error. */

/* libclang's errors at off are left out: niu reports the problem there. */
static void niu_skip_clang(niu_dctx *c, size_t off)
{
    c->clang_skip = (size_t *)niu_xrealloc(c->clang_skip, (c->n_clang_skip + 1) * sizeof *c->clang_skip);
    c->clang_skip[c->n_clang_skip++] = off;
}

static void niu_dollar_reserved(niu_dctx *c, size_t off)
{
    if (off == (size_t)-1)
        return;
    niu_ddiag(c, "error", off, "'$' is reserved in niu files");
    niu_skip_clang(c, off);
    c->n_dollar++;
}

static void niu_check_dollar(niu_dctx *c, niu_node *n)
{
    size_t i;
    int k = n->kind;
    if (k == NIU_K_VAR_DECL || k == NIU_K_FUNCTION_DECL || k == NIU_K_PARM_DECL || k == NIU_K_TYPEDEF_DECL ||
        k == NIU_K_ENUM_CONSTANT_DECL) {
        char *name = niu_cxstr(cx.clang_getCursorSpelling(n->cur));
        if (strcmp(name, "$") == 0)
            niu_dollar_reserved(c, niu_cursor_off(c, n->cur));
        free(name);
    }
    for (i = 0; i < n->nkids; i++)
        niu_check_dollar(c, n->kids[i]);
}

/* ---- macros ---- */

typedef struct {
    niu_dctx *c;
} niu_mvisit;

static enum CXChildVisitResult niu_macro_visit(CXCursor cur, CXCursor parent, CXClientData data)
{
    niu_dctx *c = ((niu_mvisit *)data)->c;
    int k = cx.clang_getCursorKind(cur);
    (void)parent;
    if (k == NIU_K_MACRO_DEFINITION) {
        char *name = niu_cxstr(cx.clang_getCursorSpelling(cur));
        size_t i;
        /* `#define $(x) ...` is fine: `if($)` has no '(' after the '$' */
        if (strcmp(name, "$") == 0 && !cx.clang_Cursor_isMacroFunctionLike(cur))
            niu_dollar_reserved(c, niu_cursor_off(c, cur));
        for (i = 0; i < c->macro_names.count; i++)
            if (strcmp(c->macro_names.items[i], name) == 0)
                break;
        if (i == c->macro_names.count) {
            niu_strlist_push(&c->macro_names, name);
            c->macro_defs = (CXCursor *)niu_xrealloc(c->macro_defs, c->macro_names.count * sizeof *c->macro_defs);
        }
        c->macro_defs[i] = cur;
        free(name);
    } else if (k == NIU_K_MACRO_EXPANSION) {
        CXSourceRange r = cx.clang_getCursorExtent(cur);
        CXFile f = NULL;
        unsigned line, col, b = 0, e = 0;
        cx.clang_getExpansionLocation(cx.clang_getRangeStart(r), &f, &line, &col, &b);
        if (niu_in_main(c, f)) {
            niu_expansion *x;
            cx.clang_getExpansionLocation(cx.clang_getRangeEnd(r), &f, &line, &col, &e);
            if (c->n_exps == c->cap_exps) {
                c->cap_exps = c->cap_exps ? 2 * c->cap_exps : 32;
                c->exps = (niu_expansion *)niu_xrealloc(c->exps, c->cap_exps * sizeof *c->exps);
            }
            x = &c->exps[c->n_exps++];
            x->b = b;
            x->e = e > b ? e : b;
            x->name = niu_cxstr(cx.clang_getCursorSpelling(cur));
            x->cur = cur;
        }
    }
    return CXChildVisit_Continue;
}

/* The text of a macro definition (name and replacement list). */
static char *niu_macro_body(niu_dctx *c, CXCursor def)
{
    CXSourceRange r;
    CXFile f = NULL, f2 = NULL;
    unsigned line, col, b = 0, e = 0;
    size_t size = 0;
    const char *text;
    if (cx.clang_Cursor_isNull(def))
        return NULL;
    r = cx.clang_getCursorExtent(def);
    cx.clang_getSpellingLocation(cx.clang_getRangeStart(r), &f, &line, &col, &b);
    cx.clang_getSpellingLocation(cx.clang_getRangeEnd(r), &f2, &line, &col, &e);
    if (!f)
        return NULL;
    text = cx.clang_getFileContents(c->tu, f, &size);
    if (!text || e > size || b >= e)
        return NULL;
    return niu_strndup(text + b, e - b);
}

/* Adds the macro names used in text (and, through the definitions in
 * macro_bodies, the ones they use) to out. */
static void niu_macro_closure_text(niu_dctx *c, const char *text, niu_strlist *out, int *counter, int depth)
{
    niu_lexer lx;
    size_t len = strlen(text);
    niu_lex_init(&lx, text, len, 0);
    for (;;) {
        niu_tok t = niu_lex_next(&lx);
        char *name;
        size_t i;
        if (t.kind == NIU_TOK_EOF)
            break;
        if (t.kind != NIU_TOK_IDENT)
            continue;
        name = niu_strndup(text + t.b, t.e - t.b);
        if (strcmp(name, "__COUNTER__") == 0)
            *counter = 1;
        if (!niu_strlist_contains(out, name))
            for (i = 0; i < c->macro_names.count; i++)
                if (strcmp(c->macro_names.items[i], name) == 0) {
                    niu_strlist_push(out, name);
                    if (depth < 32) {
                        char *body = niu_macro_body(c, c->macro_defs[i]);
                        if (body) {
                            niu_macro_closure_text(c, body, out, counter, depth + 1);
                            free(body);
                        }
                    }
                    break;
                }
        free(name);
    }
}

/* ---- noreturn ---- */

typedef struct {
    niu_dctx *c;
    int found;
} niu_attr_ctx;

static enum CXChildVisitResult niu_attr_visit(CXCursor cur, CXCursor parent, CXClientData data)
{
    niu_attr_ctx *ac = (niu_attr_ctx *)data;
    (void)parent;
    if (cx.clang_isAttribute(cx.clang_getCursorKind(cur))) {
        CXSourceRange r = cx.clang_getCursorExtent(cur);
        CXFile f = NULL;
        unsigned line, col, b = 0, e = 0;
        size_t size = 0;
        const char *text;
        cx.clang_getSpellingLocation(cx.clang_getRangeStart(r), &f, &line, &col, &b);
        cx.clang_getSpellingLocation(cx.clang_getRangeEnd(r), &f, &line, &col, &e);
        text = f ? cx.clang_getFileContents(ac->c->tu, f, &size) : NULL;
        if (text && b < e && e <= size) {
            char *s = niu_strndup(text + b, e - b);
            if (strstr(s, "oreturn"))
                ac->found = 1;
            free(s);
        }
    }
    return CXChildVisit_Continue;
}

/* Is the function f declared noreturn (_Noreturn, [[noreturn]],
 * __attribute__((noreturn)), __declspec(noreturn), or a noreturn builtin)? */
static int niu_is_noreturn_fn(niu_dctx *c, CXCursor f)
{
    char *name = niu_cxstr(cx.clang_getCursorSpelling(f));
    char *type;
    int yes = 0;
    if (niu_strlist_contains(&c->noreturn_yes, name) || niu_strlist_contains(&c->noreturn_no, name)) {
        yes = niu_strlist_contains(&c->noreturn_yes, name);
        free(name);
        return yes;
    }
    if (strcmp(name, "__builtin_unreachable") == 0 || strcmp(name, "__builtin_trap") == 0 ||
        strcmp(name, "__builtin_abort") == 0 || strcmp(name, "__builtin_longjmp") == 0)
        yes = 1;
    type = niu_cxstr(cx.clang_getTypeSpelling(cx.clang_getCursorType(f)));
    if (strstr(type, "noreturn"))
        yes = 1;
    free(type);
    if (!yes) {
        niu_attr_ctx ac;
        ac.c = c;
        ac.found = 0;
        cx.clang_visitChildren(f, niu_attr_visit, &ac);
        yes = ac.found;
    }
    niu_strlist_push(yes ? &c->noreturn_yes : &c->noreturn_no, name);
    free(name);
    return yes;
}

/* Strips parentheses, casts and implicit conversions. */
static niu_node *niu_strip_expr(niu_node *n)
{
    while (n && (n->kind == NIU_K_UNEXPOSED_EXPR || n->kind == NIU_K_PAREN_EXPR || n->kind == NIU_K_CSTYLE_CAST_EXPR) &&
           n->nkids > 0)
        n = n->kind == NIU_K_CSTYLE_CAST_EXPR ? niu_last_kid(n) : n->kids[0];
    return n;
}

/* Is the statement a call to a noreturn function? */
static int niu_is_noreturn_call(niu_dctx *c, niu_node *n)
{
    niu_node *callee;
    CXCursor f;
    n = niu_strip_expr(n);
    if (!n || n->kind != NIU_K_CALL_EXPR || n->nkids == 0)
        return 0;
    callee = niu_strip_expr(n->kids[0]);
    if (!callee || callee->kind != NIU_K_DECL_REF_EXPR)
        return 0;
    f = cx.clang_getCursorReferenced(callee->cur);
    if (cx.clang_Cursor_isNull(f) || cx.clang_getCursorKind(f) != NIU_K_FUNCTION_DECL)
        return 0;
    return niu_is_noreturn_fn(c, f);
}

/* ---- jump targets ---- */

/* The loop or switch a break/continue leaves; *crossed is set to the
 * deferred statement it would leave first, if any. */
static niu_node *niu_jump_target(niu_node *x, int is_break, niu_node **crossed)
{
    niu_node *child = x, *p = x->parent;
    *crossed = NULL;
    for (; p && p->kind != NIU_K_FUNCTION_DECL; child = p, p = p->parent) {
        if (p->kind == NIU_N_DEFER && p->nkids >= 2 && child == p->kids[1] && !*crossed)
            *crossed = p;
        if (niu_is_loop(p->kind) || (is_break && p->kind == NIU_K_SWITCH_STMT)) {
            /* a break/continue in the loop condition does not belong to it,
             * but C does not allow that anyway */
            return p;
        }
    }
    return NULL;
}

/* The LabelStmt node a goto jumps to. */
static niu_node *niu_goto_label(niu_dctx *c, niu_node *g)
{
    size_t i;
    CXCursor l;
    size_t off;
    if (g->nkids == 0 || g->kids[0]->kind != NIU_K_LABEL_REF)
        return NULL;
    l = cx.clang_getCursorReferenced(g->kids[0]->cur);
    if (cx.clang_Cursor_isNull(l))
        return NULL;
    off = niu_cursor_off(c, l);
    for (i = 0; i < c->labels.count; i++) {
        niu_node *n = c->labels.items[i];
        if (n->loc == off)
            return n;
    }
    return NULL;
}

/* Appends the defers that are registered in compound p before its kid
 * `before` (exclusive, all when before is NULL), most recent first. */
static void niu_defers_before(niu_node *p, niu_node *before, niu_ilist *out)
{
    size_t end = before ? before->index : p->nkids, i;
    for (i = end; i > 0; i--) {
        niu_node *k = p->kids[i - 1];
        if (k->kind == NIU_N_DEFER && k->defer >= 0)
            niu_ilist_push(out, k->defer);
    }
}

/* The defers that run when control leaves from x up to `stop` (exclusive),
 * innermost first. Returns the deferred statement x would leave on the way,
 * or NULL. */
static niu_node *niu_pending(niu_node *x, niu_node *stop, niu_ilist *out)
{
    niu_node *child = x, *p = x->parent;
    for (; p && p != stop; child = p, p = p->parent) {
        if (p->kind == NIU_N_DEFER && p->nkids >= 2 && child == p->kids[1])
            return p;
        if (p->kind == NIU_K_COMPOUND_STMT)
            niu_defers_before(p, child, out);
    }
    return NULL;
}

/* ---- lowering edits ---- */

static niu_ledit *niu_ledit_new(niu_dctx *c, int kind, int owner, size_t b, size_t e)
{
    niu_ledit *l;
    if (c->n_ledits == c->cap_ledits) {
        c->cap_ledits = c->cap_ledits ? 2 * c->cap_ledits : 32;
        c->ledits = (niu_ledit *)niu_xrealloc(c->ledits, c->cap_ledits * sizeof *c->ledits);
    }
    l = &c->ledits[c->n_ledits];
    memset(l, 0, sizeof *l);
    l->kind = kind;
    l->owner = owner;
    l->b = b;
    l->e = e;
    l->seq = c->n_ledits++;
    return l;
}

static void niu_ledit_text(niu_dctx *c, int owner, size_t b, size_t e, char *text)
{
    niu_ledit *l = niu_ledit_new(c, NIU_L_TEXT, owner, b, e);
    l->text = text;
}

static int niu_owner_of(niu_node *n)
{
    niu_node *d = niu_enclosing_defer(n);
    return d ? d->defer : -1;
}

/* ---- constraint 6: names ---- */

typedef struct {
    niu_dctx *c;
    niu_defer *d;
} niu_names_ctx;

static void niu_add_used_name(niu_defer *d, const char *name, int tag, CXCursor decl)
{
    size_t i;
    for (i = 0; i < d->n_names; i++)
        if (d->names[i].tag == tag && strcmp(d->names[i].name, name) == 0)
            return;
    d->names = (niu_used_name *)niu_xrealloc(d->names, (d->n_names + 1) * sizeof *d->names);
    d->names[d->n_names].name = niu_strdup(name);
    d->names[d->n_names].tag = tag;
    d->names[d->n_names].decl = decl;
    d->n_names++;
}

static int niu_is_tag_kind(int k)
{
    return k == NIU_K_STRUCT_DECL || k == NIU_K_UNION_DECL || k == NIU_K_ENUM_DECL;
}

static void niu_collect_names(niu_dctx *c, niu_defer *d, niu_node *n)
{
    size_t i;
    if (n->kind == NIU_K_DECL_REF_EXPR || n->kind == NIU_K_TYPE_REF) {
        CXCursor r = cx.clang_getCursorReferenced(n->cur);
        if (!cx.clang_Cursor_isNull(r)) {
            size_t off = niu_cursor_off(c, r);
            int rk = cx.clang_getCursorKind(r);
            /* names declared inside the deferred statement itself stay the same */
            if (!(off != (size_t)-1 && off >= d->stmt->b && off < d->stmt->e)) {
                char *name = niu_cxstr(cx.clang_getCursorSpelling(r));
                if (*name && !strchr(name, ' '))
                    niu_add_used_name(d, name, niu_is_tag_kind(rk), r);
                free(name);
            }
        }
    }
    for (i = 0; i < n->nkids; i++)
        niu_collect_names(c, d, n->kids[i]);
}

/* Checks a declaration node against the names d uses; reports at the exit. */
static void niu_check_decl(niu_dctx *c, niu_defer *d, niu_node *decl, size_t exit_off)
{
    size_t i;
    int tag;
    char *name;
    if (decl->kind == NIU_K_DECL_STMT) {
        for (i = 0; i < decl->nkids; i++)
            niu_check_decl(c, d, decl->kids[i], exit_off);
        return;
    }
    if (decl->kind == NIU_K_ENUM_DECL)
        for (i = 0; i < decl->nkids; i++)
            if (decl->kids[i]->kind == NIU_K_ENUM_CONSTANT_DECL)
                niu_check_decl(c, d, decl->kids[i], exit_off);
    if (decl->kind != NIU_K_VAR_DECL && decl->kind != NIU_K_TYPEDEF_DECL && decl->kind != NIU_K_FUNCTION_DECL &&
        decl->kind != NIU_K_ENUM_CONSTANT_DECL && !niu_is_tag_kind(decl->kind))
        return;
    tag = niu_is_tag_kind(decl->kind);
    name = niu_cxstr(cx.clang_getCursorSpelling(decl->cur));
    for (i = 0; i < d->n_names; i++)
        if (d->names[i].tag == tag && strcmp(d->names[i].name, name) == 0 &&
            !cx.clang_equalCursors(d->names[i].decl, decl->cur)) {
            niu_diag *g = niu_ddiag(c, "error", exit_off,
                                    "'%s' in this deferred statement would refer to a different declaration at "
                                    "this exit",
                                    name);
            niu_dnote(g, niu_cursor_off(c, decl->cur) == (size_t)-1 ? decl->b : niu_cursor_off(c, decl->cur),
                      "'%s' is declared here", name);
            niu_dnote(g, d->node->b, "the 'defer' is here");
            d->bad = 1;
        }
    free(name);
}

/* Constraint 6 for the copy of d at an exit x (inside d's block). For the
 * end of d's own block, x is that block. */
static void niu_check_names(niu_dctx *c, niu_defer *d, niu_node *x, size_t exit_off)
{
    niu_node *blk = d->node->parent, *child, *p;
    size_t i;
    if (!d->n_names || !blk)
        return;
    if (x == blk) {
        for (i = d->node->index + 1; i < blk->nkids; i++)
            niu_check_decl(c, d, blk->kids[i], exit_off);
        return;
    }
    /* from the exit up to d's block */
    child = x;
    for (p = x->parent; p && p != blk; child = p, p = p->parent) {
        if (p->kind == NIU_K_COMPOUND_STMT || p->kind == NIU_K_FOR_STMT)
            for (i = 0; i < child->index; i++)
                niu_check_decl(c, d, p->kids[i], exit_off);
    }
    if (p == blk)
        for (i = d->node->index + 1; i < child->index; i++)
            niu_check_decl(c, d, blk->kids[i], exit_off);
}

/* Constraint 11 for the copy of d at an exit at exit_off. */
static void niu_check_macros(niu_dctx *c, niu_defer *d, size_t exit_off)
{
    size_t i;
    for (i = 0; i < c->pp->n_directives; i++) {
        niu_directive *dir = &c->pp->directives[i];
        if (dir->off > d->node->b && dir->off < exit_off && niu_strlist_contains(&d->macros, dir->name) &&
            !niu_strlist_contains(&d->macros_reported, dir->name)) {
            niu_diag *g = niu_ddiag(c, "error", dir->off,
                                    "'%s' is redefined while a deferred statement that uses it is still pending",
                                    dir->name);
            niu_dnote(g, d->node->b, "the 'defer' is here");
            niu_strlist_push(&d->macros_reported, dir->name);
            d->bad = 1;
        }
    }
}

/* Every check a copy needs; returns 0 when the exit cannot be lowered. */
static int niu_check_exit(niu_dctx *c, niu_node *x, niu_node *name_x, size_t exit_off, const niu_ilist *copies,
                          const char *what)
{
    size_t i;
    if (!copies->count)
        return 1;
    if (x->from_macro && x->kind != NIU_K_COMPOUND_STMT) {
        niu_expansion *m = niu_expansion_at(c, x->b);
        niu_diag *g = niu_ddiag(c, "error", x->b,
                                "'%s' comes from the macro '%s' and leaves the scope of a 'defer'; niu cannot add "
                                "the cleanup there",
                                what, m ? m->name : "?");
        niu_dnote(g, c->defers[copies->items[0]].node->b, "the 'defer' is here");
        return 0;
    }
    for (i = 0; i < copies->count; i++) {
        niu_defer *d = &c->defers[copies->items[i]];
        niu_check_names(c, d, name_x, exit_off);
        niu_check_macros(c, d, exit_off);
    }
    return 1;
}

/* ---- constraint 9: what happens after defer_now ---- */

typedef struct {
    niu_dctx *c;
    niu_node *now;  /* the defer_now */
    niu_node *cmin; /* the innermost block with a defer it ran */
    niu_nlist visited;
    int reported;
} niu_dn_ctx;

static niu_node *niu_dn_label(niu_dn_ctx *d, niu_node *g)
{
    return niu_goto_label(d->c, g);
}

/* Is the loop statement an endless one (for (;;) or while (1))? */
static int niu_endless_loop(niu_dctx *c, niu_node *n)
{
    niu_lexer lx;
    niu_tok t;
    niu_lex_init(&lx, c->src, n->e, n->b);
    t = niu_lex_next(&lx);
    if (n->kind == NIU_K_FOR_STMT && niu_tok_is(c->src, &t, "for")) {
        t = niu_lex_next(&lx);
        if (!niu_tok_is(c->src, &t, "("))
            return 0;
        t = niu_lex_next(&lx);
        if (!niu_tok_is(c->src, &t, ";"))
            return 0;
        t = niu_lex_next(&lx);
        return niu_tok_is(c->src, &t, ";");
    }
    if (n->kind == NIU_K_WHILE_STMT && niu_tok_is(c->src, &t, "while")) {
        t = niu_lex_next(&lx);
        if (!niu_tok_is(c->src, &t, "("))
            return 0;
        t = niu_lex_next(&lx);
        if (!niu_tok_is(c->src, &t, "1"))
            return 0;
        t = niu_lex_next(&lx);
        return niu_tok_is(c->src, &t, ")");
    }
    return 0;
}

/* Can s complete normally? Jumps that leave s are added to esc. A call to a
 * noreturn function ends a path. */
static int niu_flow(niu_dn_ctx *d, niu_node *s, niu_nlist *esc)
{
    size_t i;
    niu_node *crossed;
    if (!s)
        return 1;
    switch (s->kind) {
    case NIU_K_RETURN_STMT:
        niu_nlist_push(esc, s);
        return 0;
    case NIU_K_BREAK_STMT:
    case NIU_K_CONTINUE_STMT: {
        niu_node *t = niu_jump_target(s, s->kind == NIU_K_BREAK_STMT, &crossed);
        if (!t || !niu_within(t, s))
            niu_nlist_push(esc, s);
        return 0;
    }
    case NIU_K_GOTO_STMT: {
        niu_node *l = niu_dn_label(d, s);
        if (!l || !niu_within(l, s))
            niu_nlist_push(esc, s);
        return 0;
    }
    case NIU_K_INDIRECT_GOTO_STMT:
        return 0;
    case NIU_K_COMPOUND_STMT: {
        int normal = 1;
        for (i = 0; i < s->nkids; i++) {
            if (s->kids[i]->kind == NIU_K_LABEL_STMT)
                normal = 1;
            if (!normal)
                continue;
            normal = niu_flow(d, s->kids[i], esc);
        }
        return normal;
    }
    case NIU_K_IF_STMT: {
        int a = s->nkids >= 2 ? niu_flow(d, s->kids[1], esc) : 1;
        int b = s->nkids >= 3 ? niu_flow(d, s->kids[2], esc) : 1;
        return a || b;
    }
    case NIU_K_WHILE_STMT:
    case NIU_K_FOR_STMT:
    case NIU_K_DO_STMT:
    case NIU_K_SWITCH_STMT: {
        niu_nlist tmp = {0};
        int breaks = 0;
        niu_flow(d, niu_last_kid(s), &tmp);
        for (i = 0; i < tmp.count; i++) {
            niu_node *e = tmp.items[i];
            if (e->kind == NIU_K_BREAK_STMT || e->kind == NIU_K_CONTINUE_STMT) {
                niu_node *t = niu_jump_target(e, e->kind == NIU_K_BREAK_STMT, &crossed);
                if (t == s) {
                    if (e->kind == NIU_K_BREAK_STMT)
                        breaks = 1;
                    continue;
                }
            }
            niu_nlist_push(esc, e);
        }
        free(tmp.items);
        if (s->kind != NIU_K_SWITCH_STMT && niu_endless_loop(d->c, s) && !breaks)
            return 0;
        return 1;
    }
    case NIU_K_LABEL_STMT:
    case NIU_K_CASE_STMT:
    case NIU_K_DEFAULT_STMT:
        return niu_flow(d, niu_last_kid(s), esc);
    default:
        if (niu_is_noreturn_call(d->c, s))
            return 0;
        return 1;
    }
}

static void niu_dn_violation(niu_dn_ctx *d, size_t off)
{
    niu_diag *g;
    if (d->reported)
        return;
    d->reported = 1;
    g = niu_ddiag(d->c, "error", off,
                  "control can leave the scope of a 'defer' after 'defer_now'; its cleanup would run twice");
    niu_dnote(g, d->now->b, "'defer_now' is here");
}

static void niu_dn_after(niu_dn_ctx *d, niu_node *x, size_t fall_off);

static void niu_dn_escapes(niu_dn_ctx *d, niu_nlist *esc, size_t fall_off)
{
    size_t i;
    niu_node *crossed;
    for (i = 0; i < esc->count && !d->reported; i++) {
        niu_node *e = esc->items[i];
        size_t at = fall_off != (size_t)-1 ? fall_off : e->b;
        size_t follow = at; /* where control first got away, for the message */
        if (e->kind == NIU_K_RETURN_STMT) {
            niu_dn_violation(d, at);
        } else if (e->kind == NIU_K_BREAK_STMT || e->kind == NIU_K_CONTINUE_STMT) {
            niu_node *t = niu_jump_target(e, e->kind == NIU_K_BREAK_STMT, &crossed);
            if (!t || !niu_within(t, d->cmin) || t == d->cmin) {
                niu_dn_violation(d, at);
            } else if (e->kind == NIU_K_BREAK_STMT) {
                niu_dn_after(d, t, follow);
            } else {
                niu_nlist more = {0};
                niu_flow(d, niu_last_kid(t), &more);
                niu_dn_escapes(d, &more, follow);
                free(more.items);
                niu_dn_after(d, t, follow);
            }
        } else if (e->kind == NIU_K_GOTO_STMT) {
            niu_node *l = niu_dn_label(d, e);
            if (!l || !niu_within(l, d->cmin)) {
                niu_dn_violation(d, at);
            } else {
                niu_nlist more = {0};
                size_t k;
                int seen = 0;
                for (k = 0; k < d->visited.count; k++)
                    if (d->visited.items[k] == l)
                        seen = 1;
                if (!seen) {
                    niu_nlist_push(&d->visited, l);
                    if (niu_flow(d, l, &more)) {
                        niu_dn_escapes(d, &more, follow);
                        niu_dn_after(d, l, follow);
                    } else {
                        niu_dn_escapes(d, &more, follow);
                    }
                }
                free(more.items);
            }
        }
    }
}

/* x has just completed normally; follow where control goes next. */
static void niu_dn_after(niu_dn_ctx *d, niu_node *x, size_t fall_off)
{
    niu_node *p = x->parent;
    size_t i;
    for (i = 0; i < d->visited.count; i++)
        if (d->visited.items[i] == x && x != d->now)
            return;
    niu_nlist_push(&d->visited, x);
    if (d->reported)
        return;
    if (x == d->cmin || !p || p->kind == NIU_K_FUNCTION_DECL) {
        niu_dn_violation(d, fall_off != (size_t)-1 ? fall_off : niu_close_brace(d->c, x));
        return;
    }
    switch (p->kind) {
    case NIU_K_COMPOUND_STMT: {
        niu_nlist esc = {0};
        int normal = 1;
        for (i = x->index + 1; i < p->nkids; i++) {
            if (p->kids[i]->kind == NIU_K_LABEL_STMT)
                normal = 1;
            if (!normal)
                continue;
            normal = niu_flow(d, p->kids[i], &esc);
        }
        niu_dn_escapes(d, &esc, fall_off);
        free(esc.items);
        if (normal && !d->reported) {
            if (fall_off == (size_t)-1)
                fall_off = niu_close_brace(d->c, p);
            niu_dn_after(d, p, fall_off);
        }
        return;
    }
    case NIU_K_WHILE_STMT:
    case NIU_K_FOR_STMT:
    case NIU_K_DO_STMT:
        if (x == niu_last_kid(p)) {
            /* the next iteration runs the whole body again */
            niu_nlist esc = {0};
            niu_flow(d, x, &esc);
            niu_dn_escapes(d, &esc, fall_off);
            free(esc.items);
        }
        if (!d->reported && !niu_endless_loop(d->c, p))
            niu_dn_after(d, p, fall_off);
        return;
    case NIU_N_DEFER:
        return;
    default:
        niu_dn_after(d, p, fall_off);
        return;
    }
}

/* ---- the analysis ---- */

static niu_defer *niu_defer_new(niu_dctx *c)
{
    niu_defer *d;
    if (c->n_defers == c->cap_defers) {
        c->cap_defers = c->cap_defers ? 2 * c->cap_defers : 16;
        c->defers = (niu_defer *)niu_xrealloc(c->defers, c->cap_defers * sizeof *c->defers);
    }
    d = &c->defers[c->n_defers++];
    memset(d, 0, sizeof *d);
    d->owner = -1;
    return d;
}

/* Does the expression refer to niu.h's `$` (what the pre-pass writes)? */
static int niu_is_dollar(niu_node *e)
{
    CXCursor r;
    char *name;
    int yes;
    e = niu_strip_expr(e);
    if (!e || e->kind != NIU_K_DECL_REF_EXPR)
        return 0;
    r = cx.clang_getCursorReferenced(e->cur);
    if (cx.clang_Cursor_isNull(r) || cx.clang_getCursorKind(r) != NIU_K_ENUM_CONSTANT_DECL)
        return 0;
    name = niu_cxstr(cx.clang_getCursorSpelling(r));
    yes = strcmp(name, "$") == 0;
    free(name);
    return yes;
}

/* The #define with a niu keyword whose body a node comes from, found by the
 * macro used at the node's start (directly, or as the only defer-macro named
 * in that macro's body). *direct is set when it is the macro used there. */
static niu_define *niu_define_for(niu_dctx *c, niu_node *n, int kind, int *direct, niu_expansion **use)
{
    niu_expansion *x = niu_expansion_at(c, n->b);
    size_t i;
    *direct = 0;
    *use = x;
    if (!x)
        return NULL;
    for (i = 0; i < c->pp->n_defines; i++) {
        niu_define *def = &c->pp->defines[i];
        if (strcmp(def->name, x->name) == 0 && c->pp->markers[def->first_marker].kind == kind) {
            *direct = 1;
            return def;
        }
    }
    /* used from another macro: name the one with the keyword if we can */
    {
        CXCursor dc = cx.clang_getCursorReferenced(x->cur);
        char *body = cx.clang_Cursor_isNull(dc) ? NULL : niu_macro_body(c, dc);
        niu_define *found = NULL;
        if (body) {
            for (i = 0; i < c->pp->n_defines; i++) {
                niu_define *def = &c->pp->defines[i];
                niu_lexer lx;
                niu_lex_init(&lx, body, strlen(body), 0);
                for (;;) {
                    niu_tok t = niu_lex_next(&lx);
                    if (t.kind == NIU_TOK_EOF)
                        break;
                    if (t.kind == NIU_TOK_IDENT && niu_tok_is(body, &t, def->name)) {
                        found = def;
                        break;
                    }
                }
                if (found)
                    break;
            }
            free(body);
        }
        return found;
    }
}

/* Marks defer and defer_now nodes, in every function: an if whose condition
 * is `$`, and `((void)$)`. The marker is found by offset when the node is
 * spelled in the file; for a macro's tokens, through the macro used there. */
static void niu_mark_nodes(niu_dctx *c)
{
    size_t i;
    for (i = 0; i < c->all_nodes.count; i++) {
        niu_node *n = c->all_nodes.items[i];
        niu_marker *m;
        int kind, direct = 0;
        if (n->kind == NIU_K_IF_STMT && n->nkids >= 1 && niu_is_dollar(n->kids[0]))
            kind = NIU_MARK_DEFER;
        else if (n->kind == NIU_K_PAREN_EXPR && n->nkids == 1 && n->kids[0]->kind == NIU_K_CSTYLE_CAST_EXPR &&
                 niu_is_dollar(niu_last_kid(n->kids[0])))
            kind = NIU_MARK_DEFER_NOW;
        else
            continue;
        m = n->sb_main ? niu_marker_at(c->pp, n->sb) : NULL;
        if (!m || m->kind != kind) {
            niu_expansion *use = NULL;
            niu_define *def = niu_define_for(c, n, kind, &direct, &use);
            if (def && direct) {
                m = &c->pp->markers[def->first_marker];
            } else {
                niu_diag *g;
                if (def) {
                    g = niu_ddiag(c, "error", n->b,
                                  "the macro '%s' contains a 'defer', so it must be used directly, not from another "
                                  "macro",
                                  def->name);
                    niu_dnote(g, def->line_off, "'%s' is defined here", def->name);
                } else {
                    niu_ddiag(c, "error", n->b, "this 'defer' comes from a macro niu cannot find");
                }
                n->kind = kind == NIU_MARK_DEFER ? NIU_N_DEFER : NIU_N_DEFER_NOW;
                n->marker = NULL;
                continue;
            }
        }
        n->kind = kind == NIU_MARK_DEFER ? NIU_N_DEFER : NIU_N_DEFER_NOW;
        n->marker = m;
        m->matched = 1;
    }
}

/* Creates the niu_defer of every defer node (shape, ranges, names). */
static void niu_make_defers(niu_dctx *c)
{
    size_t i;
    for (i = 0; i < c->all_nodes.count; i++) {
        niu_node *n = c->all_nodes.items[i];
        niu_defer *d;
        if (n->kind != NIU_N_DEFER || !n->marker)
            continue;
        if (n->nkids < 2) {
            niu_ddiag(c, "error", n->b, "expected a statement after '%s'", n->marker->spelling);
            continue;
        }
        d = niu_defer_new(c);
        n->defer = (int)(d - c->defers);
        d->node = n;
        d->stmt = n->kids[1];
        if (n->nkids > 2) {
            niu_ddiag(c, "error", n->kids[2]->b, "'else' without a previous 'if'");
            d->bad = 1;
        }
        if (n->marker->define < 0) {
            d->shape = NIU_DS_PLAIN;
            d->b = n->b;
            d->sb = d->stmt->b;
            d->se = niu_stmt_end(c, d->stmt);
            d->e = d->se;
        } else {
            niu_define *def = &c->pp->defines[n->marker->define];
            niu_marker *m = n->marker;
            d->def = def;
            if (m->off == def->body_b && m->off + m->len == def->body_e) {
                /* #define DEFER defer: the statement is written at the use */
                d->shape = NIU_DS_KEYWORD_MACRO;
                if (d->stmt->from_macro) {
                    niu_ddiag(c, "error", d->stmt->b, "the statement after '%s' must be written out, not come from a macro", def->name);
                    d->bad = 1;
                }
                d->b = n->b;
                d->sb = d->stmt->b;
                d->se = niu_stmt_end(c, d->stmt);
                d->e = d->se;
            } else {
                niu_expansion *x = niu_expansion_at(c, n->b);
                size_t k;
                d->shape = NIU_DS_COMPANION;
                if (!x || x->b != n->b || strcmp(x->name, def->name) != 0) {
                    niu_diag *g = niu_ddiag(c, "error", n->b,
                                            "the macro '%s' contains a 'defer', so it must be used directly, not "
                                            "from another macro",
                                            def->name);
                    niu_dnote(g, def->line_off, "'%s' is defined here", def->name);
                    d->bad = 1;
                    continue;
                }
                d->use_b = x->b;
                d->use_e = x->e;
                k = niu_next_code(c->src, c->len, x->e);
                d->use_semi = k < c->len && c->src[k] == ';';
                d->sb = x->b;
                d->se = d->use_semi ? k + 1 : x->e;
                /* the defer must be the last statement of the macro */
                if (n->parent)
                    for (k = n->index + 1; k < n->parent->nkids; k++) {
                        niu_node *after = n->parent->kids[k];
                        if (after->from_macro && after->b == n->b) {
                            niu_diag *g = niu_ddiag(c, "error", n->b,
                                                    "the 'defer' must be the last statement in the macro '%s'",
                                                    def->name);
                            niu_dnote(g, def->line_off, "'%s' is defined here", def->name);
                            d->bad = 1;
                            break;
                        }
                    }
            }
        }
    }
    /* owners and names, after every defer exists */
    for (i = 0; i < c->n_defers; i++) {
        niu_defer *d = &c->defers[i];
        niu_node *o = niu_enclosing_defer(d->node);
        int counter = 0;
        d->owner = o ? o->defer : -1;
        niu_collect_names(c, d, d->stmt);
        /* the macros it uses (constraint 11) */
        if (d->shape == NIU_DS_COMPANION) {
            char *body = niu_strndup(c->src + d->def->body_b, d->def->body_e - d->def->body_b);
            niu_macro_closure_text(c, body, &d->macros, &counter, 0);
            free(body);
        } else {
            char *text = niu_strndup(c->src + d->sb, d->se - d->sb);
            niu_macro_closure_text(c, text, &d->macros, &counter, 0);
            free(text);
        }
        if (counter) {
            niu_ddiag(c, "error", d->node->b, "'__COUNTER__' cannot be used in a deferred statement");
            d->bad = 1;
        }
        if (d->owner >= 0 && c->defers[d->owner].shape == NIU_DS_COMPANION) {
            niu_ddiag(c, "error", d->node->b, "a 'defer' in a macro cannot contain another 'defer'");
            d->bad = 1;
        }
    }
}

/* Checks inside a deferred statement: constraints 2 (partly), 4, 5, 10. */
static void niu_check_inside(niu_dctx *c, niu_node *n, niu_defer *d)
{
    size_t i;
    if (n->kind == NIU_K_LABEL_STMT) {
        niu_diag *g = niu_ddiag(c, "error", n->b, "labels are not allowed in a deferred statement");
        niu_dnote(g, d->node->b, "the 'defer' is here");
        d->bad = 1;
    } else if (n->kind == NIU_K_VAR_DECL) {
        if (cx.clang_Cursor_getStorageClass(n->cur) == NIU_CX_SC_STATIC) {
            niu_diag *g = niu_ddiag(c, "error", n->b, "'static' variables are not allowed in a deferred statement");
            niu_dnote(g, d->node->b, "the 'defer' is here");
            d->bad = 1;
        } else if (cx.clang_getCursorTLSKind(n->cur) != 0) {
            niu_diag *g =
                niu_ddiag(c, "error", n->b, "'_Thread_local' variables are not allowed in a deferred statement");
            niu_dnote(g, d->node->b, "the 'defer' is here");
            d->bad = 1;
        }
    } else if (n->kind == NIU_N_DEFER_NOW) {
        niu_diag *g = niu_ddiag(c, "error", n->b, "'defer_now' cannot appear inside a deferred statement");
        niu_dnote(g, d->node->b, "the 'defer' is here");
        d->bad = 1;
    }
    for (i = 0; i < n->nkids; i++) {
        /* a nested defer is checked as its own defer */
        if (n->kids[i]->kind == NIU_N_DEFER)
            continue;
        niu_check_inside(c, n->kids[i], d);
    }
}

static const char *niu_jump_word(int kind)
{
    switch (kind) {
    case NIU_K_BREAK_STMT:
        return "break";
    case NIU_K_CONTINUE_STMT:
        return "continue";
    case NIU_K_GOTO_STMT:
        return "goto";
    default:
        return "return";
    }
}

/* Constraint 3 for a case/default label or the target of a goto: defers
 * that are registered on the way down from `top` (exclusive) to label n. */
static void niu_check_jump_in(niu_dctx *c, niu_node *n, niu_node *top, size_t report_off, const char *msg)
{
    niu_node *child = n, *p;
    for (p = n->parent; p && p != top; child = p, p = p->parent) {
        if (p->kind == NIU_K_COMPOUND_STMT) {
            size_t i;
            for (i = 0; i < child->index; i++)
                if (p->kids[i]->kind == NIU_N_DEFER) {
                    niu_diag *g = niu_ddiag(c, "error", report_off, "%s", msg);
                    niu_dnote(g, p->kids[i]->b, "the 'defer' is here");
                    return;
                }
        }
    }
}

/* The value of a return statement, or NULL. */
static niu_node *niu_return_value(niu_node *r)
{
    return r->nkids ? r->kids[0] : NULL;
}

static niu_node *niu_function_of(niu_node *n)
{
    while (n && n->kind != NIU_K_FUNCTION_DECL)
        n = n->parent;
    return n;
}

static void niu_analyze_node(niu_dctx *c, niu_node *n)
{
    niu_ilist copies = {0};
    niu_node *crossed = NULL;
    size_t i;
    switch (n->kind) {
    case NIU_N_DEFER: {
        niu_defer *d = n->defer >= 0 ? &c->defers[n->defer] : NULL;
        if (!n->marker)
            break; /* reported already */
        /* constraint 1 */
        if (!n->parent || n->parent->kind != NIU_K_COMPOUND_STMT) {
            niu_ddiag(c, "error", n->b, "'%s' must appear directly inside a block '{ }'", n->marker->spelling);
            if (d)
                d->bad = 1;
        }
        if (d) {
            niu_check_inside(c, d->stmt, d);
            if (d->shape != NIU_DS_COMPANION)
                niu_ledit_text(c, d->owner, d->b, d->e, niu_comment_out(c->src + d->b, d->e - d->b));
        }
        break;
    }
    case NIU_K_COMPOUND_STMT: {
        niu_node *last = niu_last_kid(n);
        niu_defers_before(n, NULL, &copies);
        if (!copies.count)
            break;
        if (n->parent && n->parent->kind == NIU_K_STMT_EXPR) {
            /* a statement expression: spec.md "Statement expressions" */
            int is_void = 1, unnamed = 0;
            char *type = NULL;
            if (last && niu_is_expr(last->kind) && last->kind != NIU_N_DEFER_NOW) {
                type = niu_type_for_temp(niu_expr_type(c, last), &is_void, &unnamed);
                if (unnamed) {
                    niu_ddiag(c, "error", last->b,
                              "the value of this statement expression has a type without a name, so niu cannot "
                              "store it while the defers run");
                    break;
                }
            }
            if (!niu_check_exit(c, last, n, niu_close_brace(c, n), &copies, "}"))
                break;
            if (type && !is_void) {
                niu_ledit *l = niu_ledit_new(c, NIU_L_VALUE, niu_owner_of(n), last->b, niu_stmt_end(c, last));
                l->xb = last->b;
                l->xe = niu_end_of(c, last);
                l->decl = niu_declarator(type, "__niu_value_temp");
                l->copies = copies;
                copies.items = NULL;
            } else {
                size_t at = niu_stmt_end(c, last);
                niu_ledit *l = niu_ledit_new(c, NIU_L_VALUE_AFTER, niu_owner_of(n), at, at);
                l->copies = copies;
                copies.items = NULL;
            }
            free(type);
            break;
        }
        /* control cannot reach the '}' after these */
        if (last && (last->kind == NIU_K_RETURN_STMT || last->kind == NIU_K_BREAK_STMT ||
                     last->kind == NIU_K_CONTINUE_STMT || last->kind == NIU_K_GOTO_STMT))
            break;
        {
            size_t at = niu_close_brace(c, n);
            if (niu_check_exit(c, n, n, at, &copies, "}")) {
                niu_ledit *l = niu_ledit_new(c, NIU_L_END, niu_owner_of(n), at, at);
                l->copies = copies;
                copies.items = NULL;
            }
        }
        break;
    }
    case NIU_K_RETURN_STMT: {
        niu_node *body = niu_function_body(n);
        crossed = niu_pending(n, body->parent, &copies);
        if (crossed) {
            niu_diag *g = niu_ddiag(c, "error", n->b, "'return' cannot leave a deferred statement");
            niu_dnote(g, crossed->b, "the 'defer' is here");
            if (crossed->defer >= 0)
                c->defers[crossed->defer].bad = 1;
            break;
        }
        if (!copies.count)
            break;
        if (!niu_check_exit(c, n, n, n->b, &copies, "return"))
            break;
        {
            niu_node *fn = niu_function_of(n);
            niu_node *val = niu_return_value(n);
            int is_void = 1, unnamed = 0;
            char *type = fn ? niu_type_for_temp(cx.clang_getCursorResultType(fn->cur), &is_void, &unnamed) : NULL;
            niu_ledit *l;
            if (unnamed) {
                char *fname = fn ? niu_cxstr(cx.clang_getCursorSpelling(fn->cur)) : niu_strdup("?");
                niu_ddiag(c, "error", n->b,
                          "the return type of '%s' has no name, so niu cannot store the returned value while the "
                          "defers run",
                          fname);
                free(fname);
                break;
            }
            l = niu_ledit_new(c, NIU_L_RETURN, niu_owner_of(n), n->b, niu_stmt_end(c, n));
            if (val) {
                l->xb = val->b;
                l->xe = niu_end_of(c, val);
                if (is_void)
                    l->void_value = 1;
                else
                    l->decl = niu_declarator(type, "__niu_return_temp");
            }
            l->copies = copies;
            copies.items = NULL;
            free(type);
        }
        break;
    }
    case NIU_K_BREAK_STMT:
    case NIU_K_CONTINUE_STMT: {
        niu_node *t = niu_jump_target(n, n->kind == NIU_K_BREAK_STMT, &crossed);
        if (!t)
            break;
        if (crossed && !niu_within(t, crossed->kids[1])) {
            niu_diag *g = niu_ddiag(c, "error", n->b, "'%s' cannot leave a deferred statement", niu_jump_word(n->kind));
            niu_dnote(g, crossed->b, "the 'defer' is here");
            if (crossed->defer >= 0)
                c->defers[crossed->defer].bad = 1;
            break;
        }
        niu_pending(n, t, &copies);
        if (!copies.count)
            break;
        if (niu_check_exit(c, n, n, n->b, &copies, niu_jump_word(n->kind))) {
            niu_ledit *l = niu_ledit_new(c, NIU_L_JUMP, niu_owner_of(n), n->b, niu_stmt_end(c, n));
            l->xb = n->b;
            l->xe = niu_end_of(c, n);
            l->copies = copies;
            copies.items = NULL;
        }
        break;
    }
    case NIU_K_GOTO_STMT: {
        niu_node *l = niu_goto_label(c, n), *a, *dx, *dl;
        if (!l)
            break;
        dx = niu_enclosing_defer(n);
        dl = niu_enclosing_defer(l);
        if (dx && dx != dl && !niu_within(l, dx)) {
            niu_diag *g = niu_ddiag(c, "error", n->b, "'goto' cannot leave a deferred statement");
            niu_dnote(g, dx->b, "the 'defer' is here");
            if (dx->defer >= 0)
                c->defers[dx->defer].bad = 1;
            break;
        }
        /* the lowest common ancestor */
        for (a = n->parent; a && !niu_within(l, a); a = a->parent)
            ;
        if (!a)
            break;
        niu_pending(n, a, &copies);
        if (a->kind == NIU_K_COMPOUND_STMT) {
            niu_node *cx_ = n, *cl = l;
            while (cx_->parent != a)
                cx_ = cx_->parent;
            while (cl != a && cl->parent != a)
                cl = cl->parent;
            if (cl != a && cl->index < cx_->index) {
                /* backwards: leaves the scope of the defers in between */
                for (i = cx_->index; i > cl->index + 1; i--)
                    if (a->kids[i - 1]->kind == NIU_N_DEFER && a->kids[i - 1]->defer >= 0)
                        niu_ilist_push(&copies, a->kids[i - 1]->defer);
            } else if (cl != a && cl->index > cx_->index) {
                for (i = cx_->index + 1; i < cl->index; i++)
                    if (a->kids[i]->kind == NIU_N_DEFER) {
                        niu_diag *g = niu_ddiag(c, "error", n->b, "'goto' jumps into the scope of a 'defer'");
                        niu_dnote(g, a->kids[i]->b, "the 'defer' is here");
                        break;
                    }
            }
        }
        niu_check_jump_in(c, l, a, n->b, "'goto' jumps into the scope of a 'defer'");
        if (!copies.count)
            break;
        if (niu_check_exit(c, n, n, n->b, &copies, "goto")) {
            niu_ledit *le = niu_ledit_new(c, NIU_L_JUMP, niu_owner_of(n), n->b, niu_stmt_end(c, n));
            le->xb = n->b;
            le->xe = niu_end_of(c, n);
            le->copies = copies;
            copies.items = NULL;
        }
        break;
    }
    case NIU_K_INDIRECT_GOTO_STMT: {
        niu_node *body = niu_function_body(n);
        niu_pending(n, body->parent, &copies);
        if (copies.count) {
            niu_diag *g = niu_ddiag(c, "error", n->b, "computed 'goto' is not allowed in the scope of a 'defer'");
            niu_dnote(g, c->defers[copies.items[0]].node->b, "the 'defer' is here");
        }
        break;
    }
    case NIU_K_CASE_STMT:
    case NIU_K_DEFAULT_STMT: {
        niu_node *s = n->parent;
        while (s && s->kind != NIU_K_SWITCH_STMT && s->kind != NIU_K_FUNCTION_DECL)
            s = s->parent;
        if (s && s->kind == NIU_K_SWITCH_STMT)
            niu_check_jump_in(c, n, s, n->b,
                              n->kind == NIU_K_CASE_STMT ? "'case' label is inside the scope of a 'defer'"
                                                         : "'default' label is inside the scope of a 'defer'");
        break;
    }
    case NIU_N_DEFER_NOW: {
        niu_node *body = niu_function_body(n);
        size_t k = niu_next_code(c->src, c->len, n->e);
        if (!n->marker)
            break; /* reported already */
        if (n->parent && niu_is_expr(n->parent->kind)) {
            niu_ddiag(c, "error", n->b, "'%s' must be a statement of its own", n->marker->spelling);
            break;
        }
        if (k >= c->len || c->src[k] != ';') {
            niu_ddiag(c, "error", n->e, "expected ';' after '%s'", n->marker->spelling);
            break;
        }
        crossed = niu_pending(n, body->parent, &copies);
        if (crossed)
            break; /* constraint 10, reported by niu_check_inside */
        if (!copies.count) {
            if (c->warn_empty_defer_now)
                niu_ddiag(c, "warning", n->b, "'%s' has no pending defers to run [-Wniu-empty-defer-now]",
                          n->marker->spelling);
            niu_ledit_text(c, niu_owner_of(n), n->b, k + 1, niu_comment_out(c->src + n->b, k + 1 - n->b));
            break;
        }
        if (!niu_check_exit(c, n, n, n->b, &copies, n->marker->spelling))
            break;
        /* constraint 9 */
        {
            niu_dn_ctx dn;
            niu_node *child = n, *p;
            memset(&dn, 0, sizeof dn);
            dn.c = c;
            dn.now = n;
            for (p = n->parent; p; child = p, p = p->parent) {
                if (p->kind == NIU_K_COMPOUND_STMT) {
                    for (i = 0; i < child->index; i++)
                        if (p->kids[i]->kind == NIU_N_DEFER)
                            break;
                    if (i < child->index) {
                        dn.cmin = p;
                        break;
                    }
                }
            }
            if (dn.cmin) {
                /* defer_now is an expression statement: what follows it */
                niu_node *stmt = n;
                while (stmt->parent && niu_is_expr(stmt->parent->kind))
                    stmt = stmt->parent;
                niu_dn_after(&dn, stmt, (size_t)-1);
            }
            free(dn.visited.items);
        }
        {
            niu_ledit *l = niu_ledit_new(c, NIU_L_DEFER_NOW, niu_owner_of(n), n->b, k + 1);
            l->copies = copies;
            copies.items = NULL;
        }
        break;
    }
    default:
        break;
    }
    free(copies.items);
}

/* Constraint 8, for every declaration in the niu file. */
static void niu_check_reserved(niu_dctx *c, niu_node *n)
{
    size_t i;
    int k = n->kind;
    if ((k == NIU_K_VAR_DECL || k == NIU_K_FUNCTION_DECL || k == NIU_K_PARM_DECL || k == NIU_K_TYPEDEF_DECL ||
         k == NIU_K_FIELD_DECL || k == NIU_K_ENUM_CONSTANT_DECL || niu_is_tag_kind(k) || k == NIU_K_LABEL_STMT) &&
        n->sb_main && !n->from_macro) {
        char *name = niu_cxstr(cx.clang_getCursorSpelling(n->cur));
        if (niu_starts_with(name, "__niu")) {
            size_t off = niu_cursor_off(c, n->cur);
            niu_ddiag(c, "error", off == (size_t)-1 ? n->b : off, "identifiers starting with '__niu' are reserved");
        }
        free(name);
    }
    for (i = 0; i < n->nkids; i++)
        niu_check_reserved(c, n->kids[i]);
}

typedef struct {
    niu_dctx *c;
} niu_top_ctx;

static enum CXChildVisitResult niu_top_visit(CXCursor cur, CXCursor parent, CXClientData data)
{
    niu_dctx *c = ((niu_top_ctx *)data)->c;
    CXFile f = NULL;
    unsigned line, col, off;
    int k = cx.clang_getCursorKind(cur);
    (void)parent;
    if (k >= 500 && k < 600)
        return CXChildVisit_Continue; /* preprocessing */
    cx.clang_getExpansionLocation(cx.clang_getCursorLocation(cur), &f, &line, &col, &off);
    if (!niu_in_main(c, f))
        return CXChildVisit_Continue;
    {
        niu_build_ctx bc;
        niu_node *n = niu_node_new(c, cur, NULL);
        bc.c = c;
        bc.parent = n;
        cx.clang_visitChildren(cur, niu_build_visit, &bc);
        niu_nlist_push(&c->functions, n); /* every top-level declaration */
    }
    return CXChildVisit_Continue;
}

/* Reports keywords that did not become a defer or defer_now: used as names,
 * or at file scope. Returns their count. */
static void niu_check_unmatched(niu_dctx *c)
{
    size_t i, j;
    for (i = 0; i < c->pp->n_markers; i++) {
        niu_marker *m = &c->pp->markers[i];
        int in_function = 0;
        if (m->matched || m->define >= 0)
            continue;
        for (j = 0; j < c->functions.count; j++) {
            niu_node *f = c->functions.items[j];
            niu_node *body = niu_last_kid(f);
            if (f->kind == NIU_K_FUNCTION_DECL && body && body->kind == NIU_K_COMPOUND_STMT && m->off > body->b &&
                m->off < body->e)
                in_function = 1;
        }
        {
            /* used as a name if a type or declarator comes right before it */
            size_t k = m->off, after;
            int as_name = 0;
            while (k > 0 && (c->src[k - 1] == ' ' || c->src[k - 1] == '\t' || c->src[k - 1] == '\n' ||
                             c->src[k - 1] == '\r'))
                k--;
            if (k > 0 && (niu_is_ident_char(c->src[k - 1]) || c->src[k - 1] == '*' || c->src[k - 1] == ','))
                as_name = 1;
            after = niu_next_code(c->src, c->len, m->off + m->len);
            /* `defer }`: libclang leaves the broken statement out of the AST */
            if (in_function && !as_name && m->kind != NIU_MARK_DEFER_NOW && (after >= c->len || c->src[after] == '}')) {
                niu_ddiag(c, "error", m->off, "expected a statement after '%s'", m->spelling);
                niu_skip_clang(c, after);
            } else if (in_function || as_name || m->kind == NIU_MARK_DEFER_NOW)
                niu_ddiag(c, "error", m->off, "'%s' is a keyword in niu files and cannot be used as a name",
                          m->spelling);
            else
                niu_ddiag(c, "error", m->off, "'%s' must appear directly inside a block '{ }'", m->spelling);
        }
    }
}

/* Every macro with a defer in its body: the define is rewritten whether or
 * not it is used (see milestone notes: the companion macro). */
static void niu_rewrite_defines(niu_dctx *c)
{
    size_t i;
    for (i = 0; i < c->pp->n_defines; i++) {
        niu_define *def = &c->pp->defines[i];
        niu_marker *first = &c->pp->markers[def->first_marker];
        int k, defers = 0, nows = 0;
        for (k = 0; k < def->n_markers; k++) {
            if (c->pp->markers[def->first_marker + k].kind == NIU_MARK_DEFER)
                defers++;
            else
                nows++;
        }
        if (nows) {
            for (k = 0; k < def->n_markers; k++) {
                niu_marker *m = &c->pp->markers[def->first_marker + k];
                if (m->kind == NIU_MARK_DEFER_NOW)
                    niu_ddiag(c, "error", m->off, "'%s' cannot be used in a macro", m->spelling);
            }
            continue;
        }
        if (defers > 1) {
            niu_ddiag(c, "error", c->pp->markers[def->first_marker + 1].off,
                      "a macro can contain only one 'defer', as its last statement");
            continue;
        }
        if (first->off == def->body_b && first->off + first->len == def->body_e) {
            niu_ledit_text(c, -1, first->off, first->off + first->len, niu_comment_out(c->src + first->off, first->len));
        } else {
            size_t stmt_b = niu_next_code(c->src, def->body_e, first->off + first->len);
            unsigned line, col;
            niu_buf b = {0};
            niu_dline_col(c, def->line_off, &line, &col);
            niu_buf_printf(&b, "#define __niu_defer_%s", def->name);
            niu_buf_append(&b, c->src + def->params_b, def->params_e - def->params_b);
            niu_buf_putc(&b, ' ');
            niu_buf_append(&b, c->src + stmt_b, def->body_e - stmt_b);
            niu_buf_puts(&b, c->eol);
            if (!c->no_line) {
                niu_buf_printf(&b, "#line %u \"", line);
                niu_append_string_literal(&b, niu_basename(c->input));
                niu_buf_printf(&b, "\"%s", c->eol);
            }
            niu_ledit_text(c, -1, def->line_off, def->line_off, niu_buf_take(&b));
            niu_ledit_text(c, -1, first->off, def->body_e, niu_comment_out(c->src + first->off, def->body_e - first->off));
        }
    }
}

/* Runs the whole defer analysis. */
static void niu_defer_analyze(niu_dctx *c)
{
    niu_mvisit mv;
    niu_top_ctx tc;
    size_t i;
    mv.c = c;
    cx.clang_visitChildren(cx.clang_getTranslationUnitCursor(c->tu), niu_macro_visit, &mv);
    tc.c = c;
    cx.clang_visitChildren(cx.clang_getTranslationUnitCursor(c->tu), niu_top_visit, &tc);
    for (i = 0; i < c->functions.count; i++)
        niu_check_dollar(c, c->functions.items[i]);
    /* With a $ of the file's own the markers cannot be trusted: everything
     * else would only be errors that follow from this one. */
    if (c->n_dollar)
        return;
    niu_mark_nodes(c);
    niu_check_unmatched(c);
    niu_make_defers(c);
    niu_rewrite_defines(c);
    for (i = 0; i < c->pp->n_directives; i++)
        if (niu_starts_with(c->pp->directives[i].name, "__niu"))
            niu_ddiag(c, "error", c->pp->directives[i].off, "identifiers starting with '__niu' are reserved");
    for (i = 0; i < c->functions.count; i++)
        niu_check_reserved(c, c->functions.items[i]);
    for (i = 0; i < c->all_nodes.count; i++)
        niu_analyze_node(c, c->all_nodes.items[i]);
}

static void niu_dctx_free(niu_dctx *c)
{
    size_t i;
    for (i = 0; i < c->all_nodes.count; i++) {
        free(c->all_nodes.items[i]->kids);
        free(c->all_nodes.items[i]);
    }
    free(c->all_nodes.items);
    free(c->labels.items);
    free(c->functions.items);
    free(c->sorted_ledits);
    free(c->line_starts);
    for (i = 0; i < c->n_exps; i++)
        free(c->exps[i].name);
    free(c->exps);
    niu_strlist_free(&c->macro_names);
    free(c->clang_skip);
    free(c->macro_defs);
    for (i = 0; i < c->n_defers; i++) {
        size_t k;
        for (k = 0; k < c->defers[i].n_names; k++)
            free(c->defers[i].names[k].name);
        free(c->defers[i].names);
        niu_strlist_free(&c->defers[i].macros);
        niu_strlist_free(&c->defers[i].macros_reported);
        free(c->defers[i].copy);
        free(c->defers[i].copy_indent);
    }
    free(c->defers);
    for (i = 0; i < c->n_diags; i++) {
        int k;
        free(c->diags[i].msg);
        for (k = 0; k < c->diags[i].n_notes; k++)
            free(c->diags[i].note[k]);
    }
    free(c->diags);
    for (i = 0; i < c->n_ledits; i++) {
        free(c->ledits[i].text);
        free(c->ledits[i].decl);
        free(c->ledits[i].copies.items);
    }
    free(c->ledits);
    niu_strlist_free(&c->noreturn_yes);
    niu_strlist_free(&c->noreturn_no);
    niu_strlist_free(&c->typedef_names);
    free(c->typedefs);
}
