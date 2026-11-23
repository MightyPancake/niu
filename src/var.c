/* var.c - var and let (spec.md "var and let"): checking each declaration,
 * working out the type to write, and the edit that writes it. Part of
 * niu.c; not compiled on its own.
 *
 * The pre-pass turns `var` into `__auto_type` and `let` into
 * `__auto_type const`, so clang deduces the type. The syntax rules
 * (constraints 1-4 and 6-8) are checked on the original tokens, since clang
 * accepts more than niu does (`__auto_type *p`) and recovers from the rest in
 * ways that differ between versions. The type comes from the AST: the type
 * of the initializer as a value, which keeps typedef names. niu writes it
 * out itself (niu_tp), so that each name in it can be checked against the
 * scope of the declaration (constraint 9). */

/* clang type kinds (enum CXTypeKind) */
#define NIU_T_UNEXPOSED 1
#define NIU_T_POINTER 101
#define NIU_T_RECORD 105
#define NIU_T_ENUM 106
#define NIU_T_TYPEDEF 107
#define NIU_T_FUNCTION_NOPROTO 110
#define NIU_T_FUNCTION_PROTO 111
#define NIU_T_CONSTANT_ARRAY 112
#define NIU_T_INCOMPLETE_ARRAY 114
#define NIU_T_VARIABLE_ARRAY 115
#define NIU_T_DEPENDENT_ARRAY 116
#define NIU_T_AUTO 118
#define NIU_T_ELABORATED 119
#define NIU_T_ATTRIBUTED 163
#define NIU_T_ATOMIC 177

#define NIU_K_TRANSLATION_UNIT 350

enum { NIU_Q_CONST = 1, NIU_Q_VOLATILE = 2, NIU_Q_RESTRICT = 4 };

/* ---- tokens ---- */

static int niu_punct_is(niu_dctx *c, const niu_tok *t, char ch)
{
    return t->kind == NIU_TOK_PUNCT && t->e == t->b + 1 && c->src[t->b] == ch;
}

static int niu_word_in(niu_dctx *c, const niu_tok *t, const char *const *words)
{
    size_t i;
    if (t->kind != NIU_TOK_IDENT)
        return 0;
    for (i = 0; words[i]; i++)
        if (niu_tok_is(c->src, t, words[i]))
            return 1;
    return 0;
}

/* Consumes a balanced group whose opening '(' or '[' is `open`; returns
 * the offset after its closing bracket. */
static size_t niu_skip_balanced(niu_dctx *c, niu_lexer *lx, const niu_tok *open)
{
    int depth = 1;
    (void)open; /* for the reader: the group it closes */
    for (;;) {
        niu_tok t = niu_lex_next(lx);
        if (t.kind == NIU_TOK_EOF)
            return t.b;
        if (niu_punct_is(c, &t, '(') || niu_punct_is(c, &t, '[') || niu_punct_is(c, &t, '{'))
            depth++;
        else if (niu_punct_is(c, &t, ')') || niu_punct_is(c, &t, ']') || niu_punct_is(c, &t, '}')) {
            if (--depth == 0)
                return t.e;
        }
    }
}

static const char *const niu_attr_words[] = {"__attribute__", "__attribute", "__declspec", NULL};
static const char *const niu_paren_spec_words[] = {"_Alignas", "alignas", "typeof", "__typeof__", "__typeof",
                                                   "typeof_unqual", "__typeof_unqual__", "_Atomic", "_BitInt",
                                                   NULL};

/* If t starts an attribute (`[[...]]`, `__attribute__((...))`,
 * `__declspec(...)`), consumes it and returns 1. */
static int niu_skip_attribute(niu_dctx *c, niu_lexer *lx, const niu_tok *t)
{
    niu_lexer save = *lx;
    niu_tok n;
    if (niu_punct_is(c, t, '[')) {
        n = niu_lex_next(lx);
        if (niu_punct_is(c, &n, '[')) {
            niu_skip_balanced(c, lx, &n);
            niu_lex_next(lx); /* the second ']' */
            return 1;
        }
        *lx = save;
        return 0;
    }
    if (niu_word_in(c, t, niu_attr_words)) {
        n = niu_lex_next(lx);
        if (niu_punct_is(c, &n, '(')) {
            niu_skip_balanced(c, lx, &n);
            return 1;
        }
        *lx = save;
    }
    return 0;
}

/* The end of the declaration that starts at off: after the ';' at depth 0,
 * or at a closing bracket of an enclosing level (also at a ',' when
 * stop_at_comma). */
static size_t niu_decl_end(niu_dctx *c, size_t off, int stop_at_comma)
{
    niu_lexer lx;
    int depth = 0;
    niu_lex_init(&lx, c->src, c->len, off);
    lx.bol = 0;
    for (;;) {
        niu_tok t = niu_lex_next(&lx);
        if (t.kind == NIU_TOK_EOF)
            return t.b;
        if (niu_punct_is(c, &t, '(') || niu_punct_is(c, &t, '[') || niu_punct_is(c, &t, '{'))
            depth++;
        else if (niu_punct_is(c, &t, ')') || niu_punct_is(c, &t, ']') || niu_punct_is(c, &t, '}')) {
            if (depth-- == 0)
                return t.b;
        } else if (depth == 0 && niu_punct_is(c, &t, ';'))
            return t.e;
        else if (depth == 0 && stop_at_comma && niu_punct_is(c, &t, ','))
            return t.b;
    }
}

static void niu_skip_range(niu_dctx *c, size_t b, size_t e)
{
    c->skip_ranges = (size_t *)niu_xrealloc(c->skip_ranges, (2 * c->n_skip_ranges + 2) * sizeof *c->skip_ranges);
    c->skip_ranges[2 * c->n_skip_ranges] = b;
    c->skip_ranges[2 * c->n_skip_ranges + 1] = e > b ? e : b + 1;
    c->n_skip_ranges++;
}

/* ---- the scope of a name ---- */

static int niu_cursor_named(CXCursor cur, const char *name)
{
    char *s = niu_cxstr(cx.clang_getCursorSpelling(cur));
    int yes = strcmp(s, name) == 0;
    free(s);
    return yes;
}

static int niu_same_entity(CXCursor a, CXCursor b)
{
    if (cx.clang_equalCursors(cx.clang_getCanonicalCursor(a), cx.clang_getCanonicalCursor(b)))
        return 1;
    /* two typedefs of the same name and type are the same name */
    if (cx.clang_getCursorKind(a) == NIU_K_TYPEDEF_DECL && cx.clang_getCursorKind(b) == NIU_K_TYPEDEF_DECL &&
        cx.clang_equalTypes(cx.clang_getCanonicalType(cx.clang_getCursorType(a)),
                            cx.clang_getCanonicalType(cx.clang_getCursorType(b)))) {
        char *x = niu_cxstr(cx.clang_getCursorSpelling(a));
        int same = niu_cursor_named(b, x);
        free(x);
        return same;
    }
    return 0;
}

/* Does declaration node n (a kid of a scope) declare `name` in the tag or
 * the ordinary name space? The latest such declaration is put in *found. */
static int niu_node_declares(niu_node *n, const char *name, int tag, CXCursor *found)
{
    size_t i, k;
    if (n->kind == NIU_K_DECL_STMT) {
        for (i = n->nkids; i > 0; i--)
            if (niu_node_declares(n->kids[i - 1], name, tag, found))
                return 1;
        return 0;
    }
    if (tag) {
        if (niu_is_tag_kind(n->kind) && niu_cursor_named(n->cur, name)) {
            *found = n->cur;
            return 1;
        }
        if (n->kind == NIU_K_VAR_DECL || n->kind == NIU_K_TYPEDEF_DECL)
            for (k = 0; k < n->nkids; k++)
                if (niu_is_tag_kind(n->kids[k]->kind) && niu_node_declares(n->kids[k], name, tag, found))
                    return 1;
        return 0;
    }
    if (n->kind == NIU_K_ENUM_DECL) {
        for (k = n->nkids; k > 0; k--)
            if (n->kids[k - 1]->kind == NIU_K_ENUM_CONSTANT_DECL && niu_cursor_named(n->kids[k - 1]->cur, name)) {
                *found = n->kids[k - 1]->cur;
                return 1;
            }
        return 0;
    }
    if (n->kind == NIU_K_VAR_DECL || n->kind == NIU_K_TYPEDEF_DECL || n->kind == NIU_K_FUNCTION_DECL ||
        n->kind == NIU_K_PARM_DECL) {
        if (niu_cursor_named(n->cur, name)) {
            *found = n->cur;
            return 1;
        }
        for (k = 0; k < n->nkids; k++)
            if (n->kids[k]->kind == NIU_K_ENUM_DECL && niu_node_declares(n->kids[k], name, tag, found))
                return 1;
    }
    return 0;
}

/* Is the entity e visible as `name` at `point` (a node in the niu file)?
 * When another declaration hides it, that one is put in *hider. */
static int niu_name_visible(niu_node *point, CXCursor e, int tag, const char *name, CXCursor *hider,
                            int *has_hider)
{
    niu_node *child = point, *p;
    CXCursor found;
    size_t i;
    CXCursor sp;
    *has_hider = 0;
    for (p = point->parent; p; child = p, p = p->parent) {
        if (p->kind != NIU_K_COMPOUND_STMT && p->kind != NIU_K_FOR_STMT && p->kind != NIU_K_FUNCTION_DECL)
            continue;
        for (i = child->index; i > 0; i--) {
            niu_node *k = p->kids[i - 1];
            if (p->kind == NIU_K_FUNCTION_DECL && k->kind != NIU_K_PARM_DECL)
                continue;
            if (niu_node_declares(k, name, tag, &found)) {
                if (niu_same_entity(found, e))
                    return 1;
                *hider = found;
                *has_hider = 1;
                return 0;
            }
        }
    }
    /* file scope: names there cannot be hidden by other file-scope names */
    sp = cx.clang_getCursorSemanticParent(e);
    return !cx.clang_Cursor_isNull(sp) && cx.clang_getCursorKind(sp) == NIU_K_TRANSLATION_UNIT;
}

static void niu_load_typedefs(niu_dctx *c)
{
    if (!c->typedefs_done) {
        niu_tdctx td;
        td.c = c;
        c->typedefs_done = 1;
        cx.clang_visitChildren(cx.clang_getTranslationUnitCursor(c->tu), niu_typedef_visit, &td);
    }
}

/* ---- writing a type ---- */

enum { NIU_TP_OK, NIU_TP_UNNAMED, NIU_TP_VM, NIU_TP_HIDDEN };

typedef struct {
    niu_dctx *c;
    niu_node *point; /* where the type is written */
    int err;         /* NIU_TP_* (the first problem) */
    char *err_name;  /* NIU_TP_HIDDEN: the name that cannot be used */
    CXCursor hider;
    int has_hider;
    niu_used_name *names; /* the names written, for defer constraint 6 */
    size_t n_names;
} niu_tp_ctx;

static void niu_tp_name(niu_tp_ctx *tp, const char *name, int tag, CXCursor decl)
{
    tp->names = (niu_used_name *)niu_xrealloc(tp->names, (tp->n_names + 1) * sizeof *tp->names);
    tp->names[tp->n_names].name = niu_strdup(name);
    tp->names[tp->n_names].tag = tag;
    tp->names[tp->n_names].decl = decl;
    tp->n_names++;
}

static void niu_tp_fail(niu_tp_ctx *tp, int err, const char *name, const CXCursor *hider)
{
    if (tp->err)
        return;
    tp->err = err;
    tp->err_name = name ? niu_strdup(name) : NULL;
    if (hider) {
        tp->hider = *hider;
        tp->has_hider = 1;
    }
}

static int niu_quals_of(CXType t)
{
    return (cx.clang_isConstQualifiedType(t) ? NIU_Q_CONST : 0) |
           (cx.clang_isVolatileQualifiedType(t) ? NIU_Q_VOLATILE : 0) |
           (cx.clang_isRestrictQualifiedType(t) ? NIU_Q_RESTRICT : 0);
}

static void niu_put_quals(niu_buf *b, int q, const char *sep)
{
    const char *s = "";
    if (q & NIU_Q_CONST) {
        niu_buf_printf(b, "%sconst", s);
        s = " ";
    }
    if (q & NIU_Q_VOLATILE) {
        niu_buf_printf(b, "%svolatile", s);
        s = " ";
    }
    if (q & NIU_Q_RESTRICT)
        niu_buf_printf(b, "%srestrict", s);
    if (q)
        niu_buf_puts(b, sep);
}

/* The type niu wrote for the variables whose deduced type is the AutoType
 * a, when they all agree (clang shares one AutoType between variables
 * deduced to the same type). */
static int niu_auto_written(niu_dctx *c, CXType a, CXType *out)
{
    size_t i;
    int found = 0;
    if (!c)
        return 0;
    for (i = 0; i < c->n_var_types; i++)
        if (cx.clang_equalTypes(c->var_auto[i], a)) {
            if (found && !cx.clang_equalTypes(*out, c->var_written[i]))
                return 0;
            *out = c->var_written[i];
            found = 1;
        }
    return found;
}

/* Strips the sugar niu does not write (attributes, typeof, clang's
 * built-in names such as __size_t, and the elaborated wrapper of a written
 * type), collecting its qualifiers in *q. An __auto_type becomes the type
 * niu wrote for that var or let, so that typedef names carry on through
 * later uses (with c NULL, its canonical type). */
static CXType niu_tp_unwrap_in(niu_dctx *c, CXType t, int *q)
{
    int guard;
    for (guard = 0; guard < 64; guard++) {
        CXType w;
        *q |= niu_quals_of(t);
        if (t.kind == NIU_T_ATTRIBUTED) {
            t = cx.clang_Type_getModifiedType(t);
        } else if (t.kind == NIU_T_ELABORATED) {
            t = cx.clang_Type_getNamedType(t);
        } else if (t.kind == NIU_T_AUTO && niu_auto_written(c, cx.clang_getUnqualifiedType(t), &w)) {
            t = w;
        } else if (t.kind == NIU_T_AUTO || t.kind == NIU_T_UNEXPOSED) {
            CXType k = cx.clang_getCanonicalType(t);
            if (k.kind == t.kind)
                return t;
            t = k;
        } else {
            return t;
        }
    }
    return t;
}

static CXType niu_tp_unwrap(CXType t, int *q)
{
    return niu_tp_unwrap_in(NULL, t, q);
}

static int niu_is_array_or_function(int k)
{
    return k == NIU_T_CONSTANT_ARRAY || k == NIU_T_INCOMPLETE_ARRAY || k == NIU_T_VARIABLE_ARRAY ||
           k == NIU_T_DEPENDENT_ARRAY || k == NIU_T_FUNCTION_PROTO || k == NIU_T_FUNCTION_NOPROTO;
}

static int niu_is_unnamed_spelling(const char *s)
{
    return !*s || strstr(s, "(unnamed") || strstr(s, "(anonymous");
}

/* The tag of the struct, union or enum declared by r, or NULL when it has
 * none. (libclang names an unnamed struct after its typedef, `typedef
 * struct { ... } A;`, so the cursor's spelling does not tell; the type's
 * does: "struct N" for a tag, "A" without one.) */
static char *niu_record_tag(CXCursor r)
{
    char *s = niu_cxstr(cx.clang_getTypeSpelling(cx.clang_getCanonicalType(cx.clang_getCursorType(r))));
    const char *p = s, *words[3];
    size_t i;
    words[0] = "struct ";
    words[1] = "union ";
    words[2] = "enum ";
    if (strncmp(p, "const ", 6) == 0)
        p += 6;
    for (i = 0; i < 3; i++)
        if (strncmp(p, words[i], strlen(words[i])) == 0 && !niu_is_unnamed_spelling(p + strlen(words[i]))) {
            char *tag = niu_strdup(p + strlen(words[i]));
            free(s);
            return tag;
        }
    free(s);
    return NULL;
}

static const char *niu_tag_word(int cursor_kind)
{
    return cursor_kind == NIU_K_UNION_DECL ? "union" : cursor_kind == NIU_K_ENUM_DECL ? "enum" : "struct";
}

/* Another typedef name for type t (exactly, qualifiers included) that is
 * visible at the point, or NULL: the innermost local one, else the first
 * at file scope. */
static char *niu_tp_other_typedef(niu_tp_ctx *tp, CXType t, const CXCursor *exclude)
{
    niu_dctx *c = tp->c;
    CXType want = cx.clang_getCanonicalType(t);
    niu_node *child = tp->point, *p;
    CXCursor h;
    int hh;
    size_t i, k;
    for (p = tp->point->parent; p; child = p, p = p->parent) {
        if (p->kind != NIU_K_COMPOUND_STMT && p->kind != NIU_K_FOR_STMT)
            continue;
        for (i = child->index; i > 0; i--) {
            niu_node *d = p->kids[i - 1];
            if (d->kind != NIU_K_DECL_STMT)
                continue;
            for (k = d->nkids; k > 0; k--) {
                niu_node *td = d->kids[k - 1];
                char *name;
                if (td->kind != NIU_K_TYPEDEF_DECL)
                    continue;
                if (exclude && niu_same_entity(td->cur, *exclude))
                    continue;
                if (!cx.clang_equalTypes(cx.clang_getCanonicalType(cx.clang_getCursorType(td->cur)), want))
                    continue;
                name = niu_cxstr(cx.clang_getCursorSpelling(td->cur));
                if (niu_name_visible(tp->point, td->cur, 0, name, &h, &hh)) {
                    niu_tp_name(tp, name, 0, td->cur);
                    return name;
                }
                free(name);
            }
        }
    }
    niu_load_typedefs(c);
    for (i = 0; i < c->typedef_names.count; i++) {
        CXCursor td = c->typedefs[i];
        if (exclude && niu_same_entity(td, *exclude))
            continue;
        if (!cx.clang_equalTypes(cx.clang_getCanonicalType(cx.clang_getCursorType(td)), want))
            continue;
        if (niu_name_visible(tp->point, td, 0, c->typedef_names.items[i], &h, &hh)) {
            niu_tp_name(tp, c->typedef_names.items[i], 0, td);
            return niu_strdup(c->typedef_names.items[i]);
        }
    }
    return NULL;
}

static char *niu_tp(niu_tp_ctx *tp, CXType t, const char *inner, int q);

/* `base inner`, with the qualifiers in front of base. */
static char *niu_tp_base(const char *base, const char *inner, int q)
{
    niu_buf b = {0};
    niu_put_quals(&b, q, " ");
    niu_buf_puts(&b, base);
    if (*inner) {
        niu_buf_putc(&b, ' ');
        niu_buf_puts(&b, inner);
    }
    return niu_buf_take(&b);
}

/* A struct, union or enum without typedef sugar. */
static char *niu_tp_record(niu_tp_ctx *tp, CXType t, const char *inner, int q)
{
    CXCursor r = cx.clang_getTypeDeclaration(t);
    char *tag = niu_record_tag(r), *base = NULL, *res;
    CXCursor h;
    int hh = 0;
    CXType unq = cx.clang_getUnqualifiedType(t);
    if (!tag) {
        base = niu_tp_other_typedef(tp, unq, NULL);
        if (!base) {
            niu_tp_fail(tp, NIU_TP_UNNAMED, NULL, NULL);
            base = niu_strdup("int");
        }
    } else {
        char *full = niu_format("%s %s", niu_tag_word(cx.clang_getCursorKind(r)), tag);
        if (niu_name_visible(tp->point, r, 1, tag, &h, &hh)) {
            niu_tp_name(tp, tag, 1, r);
            base = niu_strdup(full);
        } else {
            base = niu_tp_other_typedef(tp, unq, NULL);
            if (!base) {
                niu_tp_fail(tp, NIU_TP_HIDDEN, full, hh ? &h : NULL);
                base = niu_strdup(full);
            }
        }
        free(full);
    }
    res = niu_tp_base(base, inner, q);
    free(base);
    free(tag);
    return res;
}

/* A typedef name: spec.md "Writing the type". */
static char *niu_tp_typedef(niu_tp_ctx *tp, CXType t, const char *inner, int q)
{
    CXCursor e = cx.clang_getTypeDeclaration(t);
    char *name = niu_cxstr(cx.clang_getCursorSpelling(e)), *base = NULL, *res;
    CXCursor h;
    int hh = 0;
    CXType canon = cx.clang_getCanonicalType(cx.clang_getUnqualifiedType(t));
    int canon_q = 0;
    canon = niu_tp_unwrap(canon, &canon_q);
    /* 1. the typedef name */
    if (niu_name_visible(tp->point, e, 0, name, &h, &hh)) {
        niu_tp_name(tp, name, 0, e);
        res = niu_tp_base(name, inner, q);
        free(name);
        return res;
    }
    /* 2. its tag */
    if (canon.kind == NIU_T_RECORD || canon.kind == NIU_T_ENUM) {
        CXCursor r = cx.clang_getTypeDeclaration(canon), h2;
        char *tag = niu_record_tag(r);
        int hh2;
        if (tag && niu_name_visible(tp->point, r, 1, tag, &h2, &hh2)) {
            niu_tp_name(tp, tag, 1, r);
            base = niu_format("%s %s", niu_tag_word(cx.clang_getCursorKind(r)), tag);
            q |= canon_q; /* typedef const struct S CS; */
        }
        free(tag);
    }
    /* 3. another typedef name (only for a struct, union or enum: any other
     * type has step 4, and would otherwise get whichever typedef of, say,
     * unsigned long the headers happen to declare first) */
    if (!base && (canon.kind == NIU_T_RECORD || canon.kind == NIU_T_ENUM))
        base = niu_tp_other_typedef(tp, cx.clang_getUnqualifiedType(t), &e);
    /* 4. the underlying type, for what is not a struct, union or enum */
    if (!base && canon.kind != NIU_T_RECORD && canon.kind != NIU_T_ENUM) {
        free(name);
        return niu_tp(tp, cx.clang_getTypedefDeclUnderlyingType(e), inner, q);
    }
    if (!base) {
        niu_tp_fail(tp, NIU_TP_HIDDEN, name, hh ? &h : NULL);
        base = niu_strdup(name);
    }
    res = niu_tp_base(base, inner, q);
    free(base);
    free(name);
    return res;
}

/* The declaration of type t around the declarator `inner` (which may be
 * empty, for an abstract declarator), with the qualifiers q added to t. */
static char *niu_tp(niu_tp_ctx *tp, CXType t, const char *inner, int q)
{
    niu_buf b = {0};
    char *res;
    t = niu_tp_unwrap_in(tp->c, t, &q);
    switch (t.kind) {
    case NIU_T_POINTER: {
        CXType p = cx.clang_getPointeeType(t);
        int pq = 0;
        CXType pu = niu_tp_unwrap_in(tp->c, p, &pq);
        if (niu_is_array_or_function(pu.kind)) {
            static const char *const cc_names[] = {NULL, NULL, "__stdcall", "__fastcall", "__thiscall", NULL,
                                                   NULL, NULL, NULL, NULL, NULL, NULL, "__vectorcall"};
            int cc = pu.kind == NIU_T_FUNCTION_PROTO || pu.kind == NIU_T_FUNCTION_NOPROTO
                         ? cx.clang_getFunctionTypeCallingConv(pu)
                         : 0;
            niu_buf_putc(&b, '(');
            if (cc > 0 && cc < 13 && cc_names[cc])
                niu_buf_printf(&b, "%s ", cc_names[cc]);
        }
        niu_buf_putc(&b, '*');
        niu_put_quals(&b, q, *inner ? " " : "");
        niu_buf_puts(&b, inner);
        if (niu_is_array_or_function(pu.kind))
            niu_buf_putc(&b, ')');
        res = niu_tp(tp, p, niu_buf_str(&b), 0);
        niu_buf_free(&b);
        return res;
    }
    case NIU_T_CONSTANT_ARRAY:
        niu_buf_printf(&b, "%s[%lld]", inner, cx.clang_getArraySize(t));
        res = niu_tp(tp, cx.clang_getArrayElementType(t), niu_buf_str(&b), q);
        niu_buf_free(&b);
        return res;
    case NIU_T_INCOMPLETE_ARRAY:
        niu_buf_printf(&b, "%s[]", inner);
        res = niu_tp(tp, cx.clang_getArrayElementType(t), niu_buf_str(&b), q);
        niu_buf_free(&b);
        return res;
    case NIU_T_VARIABLE_ARRAY:
    case NIU_T_DEPENDENT_ARRAY:
        niu_tp_fail(tp, NIU_TP_VM, NULL, NULL);
        return niu_strdup("int");
    case NIU_T_FUNCTION_PROTO:
    case NIU_T_FUNCTION_NOPROTO: {
        int i, n = t.kind == NIU_T_FUNCTION_PROTO ? cx.clang_getNumArgTypes(t) : 0;
        niu_buf_printf(&b, "%s(", inner);
        for (i = 0; i < n; i++) {
            char *a = niu_tp(tp, cx.clang_getArgType(t, (unsigned)i), "", 0);
            if (i)
                niu_buf_puts(&b, ", ");
            niu_buf_puts(&b, a);
            free(a);
        }
        if (t.kind == NIU_T_FUNCTION_PROTO && cx.clang_isFunctionTypeVariadic(t))
            niu_buf_puts(&b, n ? ", ..." : "...");
        else if (t.kind == NIU_T_FUNCTION_PROTO && n == 0)
            niu_buf_puts(&b, "void");
        niu_buf_putc(&b, ')');
        res = niu_tp(tp, cx.clang_getResultType(t), niu_buf_str(&b), 0);
        niu_buf_free(&b);
        return res;
    }
    case NIU_T_TYPEDEF:
        return niu_tp_typedef(tp, t, inner, q);
    case NIU_T_RECORD:
    case NIU_T_ENUM:
        return niu_tp_record(tp, t, inner, q);
    case NIU_T_ATOMIC: {
        char *v = niu_tp(tp, cx.clang_Type_getValueType(t), "", 0);
        char *base = niu_format("_Atomic(%s)", v);
        res = niu_tp_base(base, inner, q);
        free(base);
        free(v);
        return res;
    }
    default: {
        char *s = niu_cxstr(cx.clang_getTypeSpelling(cx.clang_getUnqualifiedType(t)));
        if (niu_has_builtin_sugar(s)) {
            free(s);
            s = niu_cxstr(cx.clang_getTypeSpelling(cx.clang_getCanonicalType(cx.clang_getUnqualifiedType(t))));
        }
        if (niu_is_unnamed_spelling(s))
            niu_tp_fail(tp, NIU_TP_UNNAMED, NULL, NULL);
        res = niu_tp_base(s, inner, q);
        free(s);
        return res;
    }
    }
}

/* ---- one declaration ---- */

static const char *const niu_storage_words[] = {"static", "_Thread_local", "thread_local", "__thread", "register",
                                                NULL};
/* words that start or belong to a statement or an expression */
static const char *const niu_stmt_words[] = {"return", "if", "else", "while", "do", "for", "switch", "case",
                                             "default", "goto", "break", "continue", "sizeof", "_Alignof",
                                             "alignof", "__alignof__", "__alignof", "_Static_assert",
                                             "static_assert", "_Generic", "asm", "__asm__", "__asm", NULL};
static const char *const niu_sizeof_words[] = {"sizeof", "_Alignof", "alignof", "__alignof__", "__alignof",
                                               "typeof", "__typeof__", "__typeof", "typeof_unqual",
                                               "__typeof_unqual__", "_Alignas", "alignas", "_Atomic", NULL};

/* Can the token start an operand (for `(var)x`, a cast)? */
static int niu_starts_operand(niu_dctx *c, const niu_tok *t)
{
    if (t->kind == NIU_TOK_IDENT || t->kind == NIU_TOK_NUMBER || t->kind == NIU_TOK_STRING ||
        t->kind == NIU_TOK_CHAR)
        return 1;
    if (t->kind != NIU_TOK_PUNCT)
        return 0;
    return strchr("({~!-+*&", c->src[t->b]) != NULL;
}

/* Adds the names a var's written type uses to every defer whose deferred
 * statement contains it (defer constraint 6), except names declared inside
 * that deferred statement. */
static void niu_var_names_to_defers(niu_dctx *c, niu_node *n, const niu_tp_ctx *tp)
{
    niu_node *d;
    for (d = niu_enclosing_defer(n); d; d = niu_enclosing_defer(d)) {
        niu_defer *df;
        size_t i;
        if (d->defer < 0)
            continue;
        df = &c->defers[d->defer];
        for (i = 0; i < tp->n_names; i++) {
            size_t off = niu_cursor_off(c, tp->names[i].decl);
            if (off != (size_t)-1 && off >= df->stmt->b && off < df->stmt->e)
                continue;
            niu_add_used_name(df, tp->names[i].name, tp->names[i].tag, tp->names[i].decl);
        }
    }
}

/* The VarDecl node whose name is spelled at off, or NULL. */
static niu_node *niu_var_decl_at(niu_dctx *c, size_t off)
{
    size_t i;
    for (i = 0; i < c->all_nodes.count; i++) {
        niu_node *n = c->all_nodes.items[i];
        if (n->kind == NIU_K_VAR_DECL && niu_cursor_spelling_off(c, n->cur) == off)
            return n;
    }
    return NULL;
}

/* Are a and b the same type with the same typedef name (an elaborated
 * wrapper or not)? */
static int niu_same_sugar(CXType a, CXType b)
{
    a = cx.clang_getUnqualifiedType(a);
    b = cx.clang_getUnqualifiedType(b);
    if (a.kind == NIU_T_ELABORATED)
        a = cx.clang_Type_getNamedType(a);
    if (b.kind == NIU_T_ELABORATED)
        b = cx.clang_Type_getNamedType(b);
    if (a.kind == NIU_T_TYPEDEF && b.kind == NIU_T_TYPEDEF)
        return cx.clang_equalCursors(cx.clang_getTypeDeclaration(a), cx.clang_getTypeDeclaration(b));
    return cx.clang_equalTypes(a, b);
}

/* The typedef name the value of expression n has in the lowered C, where
 * every var and let has the type niu wrote for it; 0 when it has none.
 * Like clang, an operator whose operands have the same typedef name has
 * that name too (two size_t operands make a size_t, spec.md "Typedef names
 * are kept"). */
static int niu_node_sugar(niu_dctx *c, niu_node *n, CXType *out, int depth)
{
    CXType t;
    int q = 0;
    /* parentheses, and implicit conversions that keep the type */
    while (n && (n->kind == NIU_K_UNEXPOSED_EXPR || n->kind == NIU_K_PAREN_EXPR) && n->nkids == 1 &&
           cx.clang_equalTypes(cx.clang_getCanonicalType(cx.clang_getCursorType(n->kids[0]->cur)),
                               cx.clang_getCanonicalType(cx.clang_getCursorType(n->cur))))
        n = n->kids[0];
    if (!n || depth > 64)
        return 0;
    if (n->kind == NIU_K_DECL_REF_EXPR) {
        CXCursor r = cx.clang_getCursorReferenced(n->cur);
        size_t i;
        for (i = 0; !cx.clang_Cursor_isNull(r) && i < c->n_var_types; i++)
            if (cx.clang_equalCursors(c->var_decls[i], r)) {
                *out = cx.clang_getUnqualifiedType(c->var_written[i]);
                return niu_tp_unwrap(*out, &q).kind == NIU_T_TYPEDEF;
            }
    }
    if ((n->kind == 114 /* BinaryOperator */ && n->nkids == 2) ||
        (n->kind == 116 /* ConditionalOperator */ && n->nkids == 3)) {
        CXType a, b;
        if (niu_node_sugar(c, n->kids[n->nkids - 2], &a, depth + 1) &&
            niu_node_sugar(c, n->kids[n->nkids - 1], &b, depth + 1) &&
            niu_same_sugar(a, b) &&
            cx.clang_equalTypes(cx.clang_getCanonicalType(cx.clang_getUnqualifiedType(a)),
                                cx.clang_getCanonicalType(cx.clang_getUnqualifiedType(cx.clang_getCursorType(n->cur))))) {
            *out = cx.clang_getUnqualifiedType(a);
            return 1;
        }
        return 0;
    }
    t = cx.clang_getCursorType(n->cur);
    if (niu_tp_unwrap(t, &q).kind == NIU_T_TYPEDEF ||
        (t.kind == NIU_T_ELABORATED && cx.clang_Type_getNamedType(t).kind == NIU_T_TYPEDEF)) {
        *out = cx.clang_getUnqualifiedType(t);
        return 1;
    }
    return 0;
}

/* The type of the initializer as a value (spec.md "Semantics"). */
static CXType niu_var_value_type(niu_dctx *c, niu_node *vd, niu_node *init)
{
    CXType declared = cx.clang_getCanonicalType(cx.clang_getUnqualifiedType(cx.clang_getCursorType(vd->cur)));
    CXType t = niu_expr_type(c, init);
    int q = 0;
    if (declared.kind == NIU_T_ATOMIC)
        declared = cx.clang_getCanonicalType(cx.clang_Type_getValueType(declared));
    declared = cx.clang_getUnqualifiedType(declared);
    {
        /* an _Atomic value: the type it holds, keeping its typedef name */
        CXType u = niu_tp_unwrap_in(c, t, &q);
        if (u.kind == NIU_T_ATOMIC)
            t = cx.clang_Type_getValueType(u);
        else if (cx.clang_getCanonicalType(u).kind == NIU_T_ATOMIC)
            t = cx.clang_Type_getValueType(cx.clang_getCanonicalType(u));
    }
    t = cx.clang_getUnqualifiedType(t);
    if (t.kind == NIU_T_AUTO) {
        /* the value of a var or let itself: the type written for it */
        niu_node *s = niu_strip_expr(init);
        if (s && s->kind == NIU_K_DECL_REF_EXPR) {
            CXCursor r = cx.clang_getCursorReferenced(s->cur);
            size_t i;
            for (i = 0; !cx.clang_Cursor_isNull(r) && i < c->n_var_types; i++)
                if (cx.clang_equalCursors(c->var_decls[i], r)) {
                    t = cx.clang_getUnqualifiedType(c->var_written[i]);
                    break;
                }
        }
    }
    if (t.kind == NIU_T_AUTO) {
        /* the value of another var or let: the type niu wrote for it */
        CXType w;
        t = niu_auto_written(c, t, &w) ? cx.clang_getUnqualifiedType(w) : cx.clang_getCanonicalType(t);
    }
    {
        /* no typedef name: maybe it has one in the lowered C */
        CXType w;
        int wq = 0;
        if (niu_tp_unwrap(t, &wq).kind != NIU_T_TYPEDEF && niu_node_sugar(c, init, &w, 0) &&
            cx.clang_equalTypes(cx.clang_getUnqualifiedType(cx.clang_getCanonicalType(w)), declared))
            t = w;
    }
    if (!cx.clang_equalTypes(cx.clang_getUnqualifiedType(cx.clang_getCanonicalType(t)), declared))
        t = declared;
    return t;
}

static void niu_var_one(niu_dctx *c, niu_marker *m)
{
    const char *kw = m->spelling;
    niu_lexer lx;
    niu_tok n, t;
    niu_diag *g = NULL;
    size_t name_b, name_e, attr_e, decl_e = 0;
    int bad = 0, in_macro = 0, n_attrs = 0;
    size_t attrs[8][2]; /* the attributes after the name */
    niu_node *vd, *init = NULL;

    /* In a macro argument (`TRY(var x = f());`) the brackets around it are
     * the macro's: the declaration starts at the argument, and whether it is
     * one is left to the AST (the VarDecl must exist). */
    {
        niu_expansion *x = niu_expansion_at(c, m->off);
        if (x) {
            niu_lexer al;
            int depth = 0;
            size_t arg_b = m->off;
            niu_lex_init(&al, c->src, m->off, x->b);
            al.bol = 0;
            for (;;) {
                niu_tok a = niu_lex_next(&al);
                if (a.kind == NIU_TOK_EOF)
                    break;
                if (niu_punct_is(c, &a, '(') || niu_punct_is(c, &a, '[') || niu_punct_is(c, &a, '{'))
                    depth++;
                else if (niu_punct_is(c, &a, ')') || niu_punct_is(c, &a, ']') || niu_punct_is(c, &a, '}'))
                    depth--;
                if (depth == 1 && (niu_punct_is(c, &a, '(') || niu_punct_is(c, &a, ',') ||
                                   niu_punct_is(c, &a, ';') || niu_punct_is(c, &a, '{') || niu_punct_is(c, &a, '}')))
                    arg_b = a.e;
                else if (depth == 2 && niu_punct_is(c, &a, '{'))
                    arg_b = a.e;
            }
            in_macro = 1;
            if (arg_b > m->decl_b || m->decl_b > m->off)
                m->decl_b = niu_next_code(c->src, m->off, arg_b);
        }
    }

    niu_lex_init(&lx, c->src, c->len, m->off + m->len);
    lx.bol = 0;
    n = niu_lex_next(&lx);

    if (!(n.kind == NIU_TOK_IDENT || niu_punct_is(c, &n, '*') || niu_punct_is(c, &n, '(') ||
          niu_punct_is(c, &n, '['))) {
        /* `(var)x`, `sizeof(var)`: var as a type name (constraint 8) */
        int type_name = 0;
        if (niu_punct_is(c, &n, ')') && m->ctx == NIU_CTX_PAREN && m->open_off != (size_t)-1 &&
            m->prev_off == m->open_off) {
            niu_tok before, after;
            niu_lexer l2;
            int have_before = m->open_prev != (size_t)-1;
            memset(&before, 0, sizeof before);
            if (have_before) {
                niu_lex_init(&l2, c->src, c->len, m->open_prev);
                l2.bol = 0;
                before = niu_lex_next(&l2);
            }
            after = niu_lex_next(&lx);
            if (have_before && niu_word_in(c, &before, niu_sizeof_words))
                type_name = 1;
            else if ((!have_before ||
                      !((before.kind == NIU_TOK_IDENT && !niu_word_in(c, &before, niu_stmt_words)) ||
                        niu_punct_is(c, &before, ')') || niu_punct_is(c, &before, ']'))) &&
                     niu_starts_operand(c, &after))
                type_name = 1;
        }
        if (type_name)
            niu_ddiag(c, "error", m->off, "'%s' can only declare a variable", kw);
        else
            niu_ddiag(c, "error", m->off, "'%s' is a keyword in niu files and cannot be used as a name", kw);
        /* the C the pre-pass wrote there breaks the rest of the statement */
        niu_skip_range(c, m->decl_b, niu_decl_end(c, m->off, 0));
        return;
    }

    /* constraint 8: where a declaration of a variable can be */
    if (!in_macro && m->ctx != NIU_CTX_FILE && m->ctx != NIU_CTX_BLOCK && m->ctx != NIU_CTX_FOR_INIT) {
        niu_ddiag(c, "error", m->off, "'%s' can only declare a variable", kw);
        niu_skip_range(c, m->decl_b, niu_decl_end(c, m->off, 1));
        return;
    }

    /* the specifiers before the keyword: constraints 8 and 6 */
    {
        niu_lexer pl;
        int pass;
        for (pass = 0; pass < 2 && !bad; pass++) {
            niu_lex_init(&pl, c->src, m->off, m->decl_b);
            pl.bol = 0;
            for (;;) {
                niu_tok s = niu_lex_next(&pl);
                if (s.kind == NIU_TOK_EOF)
                    break;
                if (niu_skip_attribute(c, &pl, &s))
                    continue;
                if (pass == 0) {
                    /* anything but specifiers: not a declaration */
                    if (s.kind != NIU_TOK_IDENT || niu_word_in(c, &s, niu_stmt_words)) {
                        g = niu_ddiag(c, "error", m->off, "'%s' can only declare a variable", kw);
                        bad = 1;
                        break;
                    }
                    if (niu_word_in(c, &s, niu_paren_spec_words)) {
                        niu_lexer save = pl;
                        niu_tok o = niu_lex_next(&pl);
                        if (niu_punct_is(c, &o, '('))
                            niu_skip_balanced(c, &pl, &o);
                        else
                            pl = save;
                    }
                    continue;
                }
                if (niu_word_in(c, &s, niu_storage_words))
                    continue;
                if (niu_tok_is(c->src, &s, "extern") || niu_tok_is(c->src, &s, "typedef") ||
                    niu_tok_is(c->src, &s, "auto")) {
                    char *w = niu_strndup(c->src + s.b, s.e - s.b);
                    g = niu_ddiag(c, "error", s.b, "'%s' cannot be used with '%s'", kw, w);
                    free(w);
                } else if (niu_tok_is(c->src, &s, "_Alignas") || niu_tok_is(c->src, &s, "alignas")) {
                    char *w = niu_strndup(c->src + s.b, s.e - s.b);
                    g = niu_ddiag(c, "error", s.b, "'%s' cannot be used with '%s'; write the type out instead", kw,
                                  w);
                    free(w);
                } else {
                    g = niu_ddiag(c, "error", s.b,
                                  m->kind == NIU_MARK_LET
                                      ? "'%s' takes the place of the type; 'let' already makes the variable 'const'"
                                      : "'%s' takes the place of the type; use 'let' for a 'const' variable",
                                  kw);
                }
                bad = 1;
                break;
            }
        }
    }
    (void)g;

    /* the declarator: constraints 3, 1, 2 and 4 */
    name_b = n.b;
    name_e = n.e;
    attr_e = n.e;
    if (!bad && n.kind != NIU_TOK_IDENT) {
        niu_ddiag(c, "error", n.b, "the name after '%s' must be a plain identifier; its type comes from the initializer",
                  kw);
        bad = 1;
    }
    if (!bad) {
        for (;;) {
            niu_lexer save = lx;
            t = niu_lex_next(&lx);
            if (niu_skip_attribute(c, &lx, &t)) {
                if (n_attrs < 8) {
                    attrs[n_attrs][0] = t.b;
                    attrs[n_attrs][1] = lx.pos;
                    n_attrs++;
                }
                attr_e = lx.pos;
                continue;
            }
            lx = save;
            break;
        }
        t = niu_lex_next(&lx);
        if (niu_punct_is(c, &t, '=')) {
            niu_tok v = niu_lex_next(&lx);
            if (niu_punct_is(c, &v, '{')) {
                niu_ddiag(c, "error", v.b, "'%s' cannot take a brace initializer; write the type instead", kw);
                bad = 1;
            } else {
                /* the initializer, to its end */
                int depth = 0;
                niu_tok prev = t;
                for (; v.kind != NIU_TOK_EOF; prev = v, v = niu_lex_next(&lx)) {
                    if (niu_punct_is(c, &v, '(') || niu_punct_is(c, &v, '[') || niu_punct_is(c, &v, '{')) {
                        depth++;
                    } else if (niu_punct_is(c, &v, ')') || niu_punct_is(c, &v, ']') || niu_punct_is(c, &v, '}')) {
                        if (depth-- == 0) {
                            decl_e = v.b;
                            break;
                        }
                    } else if (depth == 0 && niu_punct_is(c, &v, ';')) {
                        decl_e = v.e;
                        break;
                    } else if (depth == 0 && niu_punct_is(c, &v, ',') && !bad) {
                        niu_lexer save = lx;
                        niu_tok w = niu_lex_next(&lx);
                        lx = save;
                        niu_ddiag(c, "error", w.kind == NIU_TOK_EOF ? v.b : w.b, "'%s' declares a single variable",
                                  kw);
                        bad = 1;
                    } else if (!bad && v.kind == NIU_TOK_IDENT && v.e - v.b == name_e - name_b &&
                               memcmp(c->src + v.b, c->src + name_b, name_e - name_b) == 0 &&
                               !niu_is_member_op(c->src, &prev)) {
                        char *nm = niu_strndup(c->src + name_b, name_e - name_b);
                        niu_ddiag(c, "error", v.b, "'%s' cannot appear in its own initializer", nm);
                        free(nm);
                        bad = 1;
                    }
                }
                if (v.kind == NIU_TOK_EOF)
                    decl_e = v.b;
            }
        } else if (niu_punct_is(c, &t, '(')) {
            /* var f(void) = ... (constraint 3), or a function (constraint 8) */
            niu_tok a;
            niu_skip_balanced(c, &lx, &t);
            a = niu_lex_next(&lx);
            if (niu_punct_is(c, &a, '='))
                niu_ddiag(c, "error", t.b,
                          "the name after '%s' must be a plain identifier; its type comes from the initializer", kw);
            else
                niu_ddiag(c, "error", m->off, "'%s' can only declare a variable", kw);
            bad = 1;
        } else if (niu_punct_is(c, &t, '[')) {
            niu_ddiag(c, "error", t.b,
                      "the name after '%s' must be a plain identifier; its type comes from the initializer", kw);
            bad = 1;
        } else if (niu_punct_is(c, &t, ';') || niu_punct_is(c, &t, ',') || niu_punct_is(c, &t, ')')) {
            /* in a macro argument it may not be a variable at all */
            if (in_macro && !niu_var_decl_at(c, name_b))
                niu_ddiag(c, "error", m->off, "'%s' can only declare a variable", kw);
            else
                niu_ddiag(c, "error", m->off, "'%s' requires an initializer", kw);
            bad = 1;
        } else {
            return; /* not C: libclang reports it */
        }
    }
    if (!decl_e)
        decl_e = niu_decl_end(c, m->off, 0);
    if (bad) {
        niu_skip_range(c, m->decl_b, decl_e);
        return;
    }

    /* the declaration in the AST */
    vd = niu_var_decl_at(c, name_b);
    if (!vd) {
        niu_ddiag(c, "error", m->off, "'%s' can only declare a variable", kw);
        niu_skip_range(c, m->off, m->off + m->len);
        return;
    }
    {
        size_t i;
        for (i = vd->nkids; i > 0; i--)
            if (niu_is_expr(vd->kids[i - 1]->kind)) {
                init = vd->kids[i - 1];
                break;
            }
    }
    if (!init || cx.clang_getCanonicalType(cx.clang_getCursorType(vd->cur)).kind == NIU_T_AUTO)
        return; /* clang could not deduce it, and says why */

    /* the type, and the edit that writes it */
    {
        niu_tp_ctx tp;
        CXType vt = niu_var_value_type(c, vd, init);
        char *decl, *name = niu_strndup(c->src + name_b, name_e - name_b), *mark;
        size_t i;
        memset(&tp, 0, sizeof tp);
        tp.c = c;
        tp.point = vd;
        decl = niu_tp(&tp, vt, "\001", m->kind == NIU_MARK_LET ? NIU_Q_CONST : 0);
        if (tp.err == NIU_TP_UNNAMED) {
            niu_ddiag(c, "error", m->off, "the type of '%s' has no name, so niu cannot write it out", name);
        } else if (tp.err == NIU_TP_VM) {
            niu_ddiag(c, "error", m->off, "the type of '%s' is a variably modified type, which niu cannot write out",
                      name);
        } else if (tp.err == NIU_TP_HIDDEN) {
            niu_diag *d = niu_ddiag(c, "error", m->off,
                                    "the type of '%s' cannot be written here: '%s' means a different type in this "
                                    "scope",
                                    name, tp.err_name);
            if (tp.has_hider) {
                size_t off = niu_cursor_off(c, tp.hider);
                if (off != (size_t)-1)
                    niu_dnote(d, off, "'%s' is declared here", tp.err_name);
            }
        } else {
            niu_buf b = {0};
            size_t end = name_e;
            mark = strchr(decl, '\001');
            niu_buf_append(&b, decl, (size_t)(mark - decl));
            niu_buf_puts(&b, name);
            if (mark[1] && n_attrs) {
                /* `void (*cb)(int)`: an attribute after the name belongs to
                 * the name, so a [[...]] goes inside the declarator, next to
                 * it (after the ')' it would apply to the type); a GNU
                 * __attribute__ cannot go there and stays after it */
                int k;
                for (k = 0; k < n_attrs; k++)
                    if (c->src[attrs[k][0]] == '[') {
                        niu_buf_putc(&b, ' ');
                        niu_buf_append(&b, c->src + attrs[k][0], attrs[k][1] - attrs[k][0]);
                    }
                niu_buf_puts(&b, mark + 1);
                for (k = 0; k < n_attrs; k++)
                    if (c->src[attrs[k][0]] != '[') {
                        niu_buf_putc(&b, ' ');
                        niu_buf_append(&b, c->src + attrs[k][0], attrs[k][1] - attrs[k][0]);
                    }
                end = attr_e;
            } else {
                niu_buf_puts(&b, mark + 1);
            }
            niu_ledit_text(c, niu_owner_of(vd), m->off, end, niu_buf_take(&b));
            niu_var_names_to_defers(c, vd, &tp);
            c->var_auto = (CXType *)niu_xrealloc(c->var_auto, (c->n_var_types + 1) * sizeof *c->var_auto);
            c->var_written = (CXType *)niu_xrealloc(c->var_written, (c->n_var_types + 1) * sizeof *c->var_written);
            c->var_auto[c->n_var_types] = cx.clang_getUnqualifiedType(cx.clang_getCursorType(vd->cur));
            c->var_written[c->n_var_types] = vt;
            c->var_decls = (CXCursor *)niu_xrealloc(c->var_decls, (c->n_var_types + 1) * sizeof *c->var_decls);
            c->var_decls[c->n_var_types] = vd->cur;
            c->n_var_types++;
        }
        for (i = 0; i < tp.n_names; i++)
            free(tp.names[i].name);
        free(tp.names);
        free(tp.err_name);
        free(decl);
        free(name);
    }
}

/* Every var and let in the niu file. Those in a #define are reported by
 * niu_rewrite_defines (constraint 7). */
static void niu_var_analyze(niu_dctx *c)
{
    size_t i;
    for (i = 0; i < c->pp->n_markers; i++) {
        niu_marker *m = &c->pp->markers[i];
        if ((m->kind == NIU_MARK_VAR || m->kind == NIU_MARK_LET) && m->define < 0 && !m->inactive)
            niu_var_one(c, m);
    }
}
