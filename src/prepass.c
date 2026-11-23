/* prepass.c - the token pre-pass. niu keywords are not C, so before
 * libclang parses a niu file, each keyword after `#include <niu.h>` is
 * replaced by C:
 *
 *   defer       ->  if($)          _Defer      ->  if ($)
 *   defer_now   ->  ((void)$)      _Defer_now  ->  ( (void)$)
 *   var         ->  __auto_type    let         ->  __auto_type const
 *
 * `$` is a constant niu.h declares (enum { $ }; only while niu parses). The
 * deferred statement then parses as the body of an if that never runs:
 * clang checks it like any other code, and niu finds it in the AST as an if
 * whose condition is niu's `$`, also when it comes from a macro. The defer
 * replacements have the same length as the keyword. var and let cannot: so
 * the pre-pass keeps an offset map (niu_pp_to_src), and every offset
 * libclang reports in the niu file goes through it before niu uses it on
 * the original text, which is the text niu edits. Lines never change, only
 * the columns after a var or let on the same line. Part of niu.c; not
 * compiled on its own. */

enum { NIU_MARK_DEFER, NIU_MARK_DEFER_NOW, NIU_MARK_VAR, NIU_MARK_LET };

/* Where a var or let is (spec.md "var and let", constraint 8). */
enum {
    NIU_CTX_FILE,     /* file scope */
    NIU_CTX_BLOCK,    /* directly in a block { } */
    NIU_CTX_FOR_INIT, /* the first clause of a for ( */
    NIU_CTX_PAREN,    /* inside other parentheses */
    NIU_CTX_MEMBER,   /* in the braces of a struct, union or enum */
    NIU_CTX_OTHER     /* in [ ] or an initializer's braces */
};

typedef struct {
    int kind;      /* NIU_MARK_* */
    size_t off;    /* offset of the keyword */
    size_t len;    /* its length */
    const char *spelling; /* "defer", "_Defer", ... */
    int define;    /* index into niu_prepass.defines, or -1 */
    int matched;   /* found in the AST */
    size_t pp_off; /* where the replacement is in niu_prepass.text */
    size_t pp_len; /* its length */
    /* var and let only: */
    int ctx;          /* NIU_CTX_* */
    size_t decl_b;    /* first token after the last ';', '{', '}' or ':' (or the for's '(') */
    size_t prev_off;  /* the token before the keyword, or (size_t)-1 */
    size_t open_off;  /* the innermost open bracket, or (size_t)-1 */
    size_t open_prev; /* the token before that bracket, or (size_t)-1 */
} niu_marker;

/* A #define whose body contains a niu keyword. */
typedef struct {
    char *name;
    size_t line_off;              /* start of the line with the '#' */
    size_t name_b, name_e;        /* the macro name */
    size_t params_b, params_e;    /* "(a, b)", or empty for an object-like macro */
    size_t body_b, body_e;        /* the replacement list, without trailing space */
    int first_marker, n_markers;  /* its keywords in niu_prepass.markers */
} niu_define;

/* A #define or #undef in the niu part of the file (for constraint 11). */
typedef struct {
    char *name;
    size_t off; /* the '#' */
    int undef;
} niu_directive;

typedef struct {
    size_t niu_h_off;  /* offset of the '#' of `#include <niu.h>`, or (size_t)-1 */
    char *text;        /* the text for libclang: the source with the keywords replaced */
    size_t len;        /* its length */
    niu_marker *markers;
    size_t n_markers, cap_markers;
    niu_define *defines;
    size_t n_defines, cap_defines;
    niu_directive *directives;
    size_t n_directives, cap_directives;
} niu_prepass;

static const struct {
    const char *word;
    const char *repl;
    int kind;
} niu_keywords[] = {
    {"defer", "if($)", NIU_MARK_DEFER},
    {"_Defer", "if ($)", NIU_MARK_DEFER},
    {"defer_now", "((void)$)", NIU_MARK_DEFER_NOW},
    {"_Defer_now", "( (void)$)", NIU_MARK_DEFER_NOW},
    {"var", "__auto_type", NIU_MARK_VAR},
    {"let", "__auto_type const", NIU_MARK_LET},
};

#define NIU_N_KEYWORDS (sizeof niu_keywords / sizeof niu_keywords[0])

/* The keyword the token spells, or -1. */
static int niu_keyword_index(const char *s, const niu_tok *t)
{
    size_t i;
    if (t->kind != NIU_TOK_IDENT)
        return -1;
    for (i = 0; i < NIU_N_KEYWORDS; i++)
        if (niu_tok_is(s, t, niu_keywords[i].word))
            return (int)i;
    return -1;
}

/* Is prev the '.' or '->' of a member access? A keyword after one is a
 * member name (s.defer, p->defer, .defer = 1). */
static int niu_is_member_op(const char *s, const niu_tok *prev)
{
    size_t k, dashes = 0;
    if (!prev || prev->kind != NIU_TOK_PUNCT)
        return 0;
    if (niu_tok_is(s, prev, "."))
        return 1;
    if (!niu_tok_is(s, prev, ">"))
        return 0;
    /* '->', not the '>' of '-->': an odd run of '-' right before the '>' */
    for (k = prev->b; k > 0 && s[k - 1] == '-'; k--)
        dashes++;
    return dashes % 2 == 1;
}

static niu_marker *niu_prepass_mark(niu_prepass *p, const niu_tok *t, int kw, int define)
{
    niu_marker *m;
    if (p->n_markers == p->cap_markers) {
        p->cap_markers = p->cap_markers ? 2 * p->cap_markers : 16;
        p->markers = (niu_marker *)niu_xrealloc(p->markers, p->cap_markers * sizeof *p->markers);
    }
    m = &p->markers[p->n_markers++];
    m->kind = niu_keywords[kw].kind;
    m->off = t->b;
    m->len = t->e - t->b;
    m->spelling = niu_keywords[kw].word;
    m->define = define;
    m->matched = 0;
    m->pp_off = 0;
    m->pp_len = strlen(niu_keywords[kw].repl);
    m->ctx = NIU_CTX_OTHER;
    m->decl_b = t->b;
    m->prev_off = m->open_off = m->open_prev = (size_t)-1;
    return m;
}

/* Builds p->text from src: the source with every marker's replacement. */
static void niu_prepass_build(niu_prepass *p, const char *src, size_t len)
{
    niu_buf b = {0};
    size_t i, pos = 0;
    for (i = 0; i < p->n_markers; i++) {
        niu_marker *m = &p->markers[i];
        size_t k;
        niu_buf_append(&b, src + pos, m->off - pos);
        m->pp_off = b.len;
        for (k = 0; k < NIU_N_KEYWORDS; k++)
            if (strcmp(niu_keywords[k].word, m->spelling) == 0)
                niu_buf_puts(&b, niu_keywords[k].repl);
        pos = m->off + m->len;
    }
    niu_buf_append(&b, src + pos, len - pos);
    p->len = b.len;
    p->text = niu_buf_take(&b);
}

/* The offset in the original text of offset off in p->text. An offset
 * inside a replacement maps to the start of its keyword. */
static size_t niu_pp_to_src(const niu_prepass *p, size_t off)
{
    size_t lo = 0, hi = p->n_markers;
    const niu_marker *m;
    if (!p->n_markers)
        return off;
    /* the last marker whose replacement starts at or before off */
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (p->markers[mid].pp_off <= off)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return off;
    m = &p->markers[lo - 1];
    if (off < m->pp_off + m->pp_len)
        return off == m->pp_off || m->pp_len == m->len ? m->off + (off - m->pp_off) : m->off;
    return off - (m->pp_off + m->pp_len) + m->off + m->len;
}

/* The bracket context the pre-pass tracks for var and let. */
typedef struct {
    char kind;       /* '(', '[' or '{' */
    int ctx;         /* NIU_CTX_* for code directly inside */
    size_t off;      /* the bracket */
    size_t prev;     /* the token before it, or (size_t)-1 */
    int for_semis;   /* a for's '(': the ';' seen so far */
} niu_bracket;

typedef struct {
    niu_bracket *items;
    size_t count, cap;
    size_t decl_b;     /* see niu_marker.decl_b */
    int have_decl_b;
    int tag_pending;   /* after struct/union/enum: a '{' opens its members */
    size_t tag_depth;
    niu_tok prev;      /* the previous token */
    int have_prev;
} niu_brackets;

static int niu_brackets_ctx(const niu_brackets *bs)
{
    return bs->count ? bs->items[bs->count - 1].ctx : NIU_CTX_FILE;
}

/* Follows one token outside directives. */
static void niu_brackets_step(niu_brackets *bs, const char *s, const niu_tok *t)
{
    size_t depth = bs->count;
    int punct = t->kind == NIU_TOK_PUNCT;
    char ch = punct ? s[t->b] : 0;
    const niu_tok *pv = bs->have_prev ? &bs->prev : NULL;
    if (bs->tag_pending && depth == bs->tag_depth && !(t->kind == NIU_TOK_IDENT || ch == '(' || ch == '[' || ch == '{'))
        bs->tag_pending = 0;
    if (punct && (ch == '(' || ch == '[' || ch == '{')) {
        niu_bracket br;
        int top = niu_brackets_ctx(bs);
        br.kind = ch;
        br.off = t->b;
        br.prev = pv ? pv->b : (size_t)-1;
        br.for_semis = 0;
        if (ch == '(')
            br.ctx = pv && niu_tok_is(s, pv, "for") ? NIU_CTX_FOR_INIT : NIU_CTX_PAREN;
        else if (ch == '[')
            br.ctx = NIU_CTX_OTHER;
        else if (bs->tag_pending && depth == bs->tag_depth)
            br.ctx = NIU_CTX_MEMBER;
        else if (pv && (niu_tok_is(s, pv, "=") || (top == NIU_CTX_OTHER && (niu_tok_is(s, pv, ",") || niu_tok_is(s, pv, "{")))))
            br.ctx = NIU_CTX_OTHER;
        else
            br.ctx = NIU_CTX_BLOCK;
        if (ch == '{')
            bs->tag_pending = 0;
        if (bs->count == bs->cap) {
            bs->cap = bs->cap ? 2 * bs->cap : 16;
            bs->items = (niu_bracket *)niu_xrealloc(bs->items, bs->cap * sizeof *bs->items);
        }
        bs->items[bs->count++] = br;
        if (ch == '{' || br.ctx == NIU_CTX_FOR_INIT)
            bs->have_decl_b = 0;
    } else if (punct && (ch == ')' || ch == ']' || ch == '}')) {
        if (bs->count)
            bs->count--;
        if (ch == '}')
            bs->have_decl_b = 0;
    } else if (punct && (ch == ';' || ch == ':')) {
        if (ch == ';' && bs->count && bs->items[bs->count - 1].ctx == NIU_CTX_FOR_INIT) {
            bs->items[bs->count - 1].for_semis++;
            bs->items[bs->count - 1].ctx = NIU_CTX_PAREN;
        }
        bs->have_decl_b = 0;
    } else {
        if (!bs->have_decl_b) {
            bs->decl_b = t->b;
            bs->have_decl_b = 1;
        }
        if (t->kind == NIU_TOK_IDENT &&
            (niu_tok_is(s, t, "struct") || niu_tok_is(s, t, "union") || niu_tok_is(s, t, "enum"))) {
            bs->tag_pending = 1;
            bs->tag_depth = depth;
        }
    }
    if (punct && (ch == '(' || ch == '[') && !bs->have_decl_b) {
        /* `(var)x`: the declaration would start here */
        bs->decl_b = t->b;
        bs->have_decl_b = bs->items[bs->count - 1].ctx != NIU_CTX_FOR_INIT;
        if (!bs->have_decl_b)
            bs->decl_b = t->e;
    }
    bs->prev = *t;
    bs->have_prev = 1;
}

/* Records where a var or let marker is. Called before the keyword's token
 * is passed to niu_brackets_step. */
static void niu_brackets_note(const niu_brackets *bs, niu_marker *m)
{
    m->ctx = niu_brackets_ctx(bs);
    m->decl_b = bs->have_decl_b ? bs->decl_b : m->off;
    m->prev_off = bs->have_prev ? bs->prev.b : (size_t)-1;
    if (bs->count) {
        m->open_off = bs->items[bs->count - 1].off;
        m->open_prev = bs->items[bs->count - 1].prev;
    }
}

/* The tokens of a directive line after its '#', up to the end of the line. */
typedef struct {
    niu_tok *items;
    size_t count, cap;
} niu_toks;

static void niu_directive_tokens(niu_lexer *lx, niu_toks *out)
{
    out->count = 0;
    for (;;) {
        size_t save = lx->pos;
        int save_bol = lx->bol;
        niu_tok t = niu_lex_next(lx);
        if (t.kind == NIU_TOK_EOF || t.bol) {
            lx->pos = save;
            lx->bol = save_bol;
            return;
        }
        if (out->count == out->cap) {
            out->cap = out->cap ? 2 * out->cap : 32;
            out->items = (niu_tok *)niu_xrealloc(out->items, out->cap * sizeof *out->items);
        }
        out->items[out->count++] = t;
    }
}

/* Is this `#include <niu.h>` (after the '#')? */
static int niu_is_niu_include(const char *s, const niu_tok *toks, size_t n)
{
    return n >= 6 && niu_tok_is(s, &toks[0], "include") && niu_tok_is(s, &toks[1], "<") &&
           niu_tok_is(s, &toks[2], "niu") && niu_tok_is(s, &toks[3], ".") && niu_tok_is(s, &toks[4], "h") &&
           niu_tok_is(s, &toks[5], ">");
}

/* Finds the first `#include <niu.h>` in the text (before preprocessing, so a
 * disabled one inside #if 0 counts too; niu_prepass_run is told the real one
 * after the parse when they differ). */
static size_t niu_find_niu_include(const char *s, size_t len)
{
    niu_lexer lx;
    niu_toks toks = {0};
    size_t found = (size_t)-1;
    niu_lex_init(&lx, s, len, 0);
    for (;;) {
        niu_tok t = niu_lex_next(&lx);
        if (t.kind == NIU_TOK_EOF)
            break;
        if (t.bol && niu_tok_is(s, &t, "#")) {
            niu_directive_tokens(&lx, &toks);
            if (niu_is_niu_include(s, toks.items, toks.count)) {
                found = t.b;
                break;
            }
        }
    }
    free(toks.items);
    return found;
}

static void niu_prepass_free(niu_prepass *p)
{
    size_t i;
    for (i = 0; i < p->n_defines; i++)
        free(p->defines[i].name);
    for (i = 0; i < p->n_directives; i++)
        free(p->directives[i].name);
    free(p->defines);
    free(p->directives);
    free(p->markers);
    free(p->text);
    memset(p, 0, sizeof *p);
}

/* Runs the pre-pass on src, replacing keywords after the offset niu_h_off
 * (the `#include <niu.h>`; (size_t)-1 for a plain file, which is copied
 * unchanged). */
static void niu_prepass_run(niu_prepass *p, const char *src, size_t len, size_t niu_h_off)
{
    niu_lexer lx;
    niu_toks dt = {0};
    niu_tok prev = {0};
    niu_brackets bs;
    int have_prev = 0;
    memset(p, 0, sizeof *p);
    memset(&bs, 0, sizeof bs);
    p->niu_h_off = niu_h_off;
    if (niu_h_off == (size_t)-1) {
        niu_prepass_build(p, src, len);
        return;
    }
    niu_lex_init(&lx, src, len, niu_h_off);
    for (;;) {
        niu_tok t = niu_lex_next(&lx);
        int kw;
        if (t.kind == NIU_TOK_EOF)
            break;
        if (!(t.bol && niu_tok_is(src, &t, "#"))) {
            kw = niu_keyword_index(src, &t);
            if (kw >= 0 && !niu_is_member_op(src, have_prev ? &prev : NULL)) {
                niu_marker *m = niu_prepass_mark(p, &t, kw, -1);
                if (m->kind == NIU_MARK_VAR || m->kind == NIU_MARK_LET)
                    niu_brackets_note(&bs, m);
            }
            niu_brackets_step(&bs, src, &t);
            prev = t;
            have_prev = 1;
            continue;
        }
        have_prev = 0;
        bs.have_prev = 0;
        bs.have_decl_b = 0;
        if (t.bol && niu_tok_is(src, &t, "#")) {
            /* A directive. Keywords are replaced only in #define bodies. */
            niu_tok *toks;
            size_t k, nk;
            niu_directive_tokens(&lx, &dt);
            toks = dt.items;
            nk = dt.count;
            if (nk >= 2 && toks[1].kind == NIU_TOK_IDENT &&
                (niu_tok_is(src, &toks[0], "define") || niu_tok_is(src, &toks[0], "undef"))) {
                niu_directive *d;
                if (p->n_directives == p->cap_directives) {
                    p->cap_directives = p->cap_directives ? 2 * p->cap_directives : 16;
                    p->directives =
                        (niu_directive *)niu_xrealloc(p->directives, p->cap_directives * sizeof *p->directives);
                }
                d = &p->directives[p->n_directives++];
                d->name = niu_strndup(src + toks[1].b, toks[1].e - toks[1].b);
                d->off = t.b;
                d->undef = niu_tok_is(src, &toks[0], "undef");
            }
            if (nk >= 2 && niu_tok_is(src, &toks[0], "define") && toks[1].kind == NIU_TOK_IDENT) {
                size_t body = 2, first = p->n_markers;
                niu_define def;
                def.params_b = def.params_e = toks[1].e;
                /* function-like: '(' right after the name */
                if (nk > 2 && niu_tok_is(src, &toks[2], "(") && toks[2].b == toks[1].e) {
                    for (k = 3; k < nk; k++)
                        if (niu_tok_is(src, &toks[k], ")"))
                            break;
                    def.params_b = toks[2].b;
                    def.params_e = k < nk ? toks[k].e : toks[nk - 1].e;
                    body = k + 1;
                }
                for (k = body; k < nk; k++) {
                    kw = niu_keyword_index(src, &toks[k]);
                    if (kw >= 0 && !(k > body && niu_is_member_op(src, &toks[k - 1])))
                        niu_prepass_mark(p, &toks[k], kw, (int)p->n_defines);
                }
                if (p->n_markers > first) {
                    def.name = niu_strndup(src + toks[1].b, toks[1].e - toks[1].b);
                    def.line_off = niu_line_start(src, t.b);
                    def.name_b = toks[1].b;
                    def.name_e = toks[1].e;
                    def.body_b = body < nk ? toks[body].b : toks[nk - 1].e;
                    def.body_e = toks[nk - 1].e;
                    def.first_marker = (int)first;
                    def.n_markers = (int)(p->n_markers - first);
                    if (p->n_defines == p->cap_defines) {
                        p->cap_defines = p->cap_defines ? 2 * p->cap_defines : 8;
                        p->defines = (niu_define *)niu_xrealloc(p->defines, p->cap_defines * sizeof *p->defines);
                    }
                    p->defines[p->n_defines++] = def;
                }
            }
            continue;
        }
    }
    free(dt.items);
    free(bs.items);
    niu_prepass_build(p, src, len);
}

/* The marker at offset off, or NULL. */
static niu_marker *niu_marker_at(niu_prepass *p, size_t off)
{
    size_t lo = 0, hi = p->n_markers;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (p->markers[mid].off < off)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < p->n_markers && p->markers[lo].off == off ? &p->markers[lo] : NULL;
}

/* The marker whose keyword covers offset off, or NULL. */
static niu_marker *niu_marker_covering(niu_prepass *p, size_t off)
{
    size_t i;
    for (i = 0; i < p->n_markers; i++)
        if (off >= p->markers[i].off && off < p->markers[i].off + p->markers[i].len)
            return &p->markers[i];
    return NULL;
}
