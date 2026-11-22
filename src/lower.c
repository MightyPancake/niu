/* lower.c - turns the edits defer.c describes into text (spec.md "Lowering").
 * Part of niu.c; not compiled on its own.
 *
 * Edits belong to a region: the file itself (owner -1) or the deferred
 * statement of one defer. A region's text is the original text with its own
 * edits applied; the copies of a defer are the text of its region. So a defer
 * inside a deferred statement is lowered in every copy of the outer one. */

static void niu_render_range(niu_dctx *c, int owner, size_t b, size_t e, niu_buf *out);

/* The copy of a defer: its deferred statement as written (lowered), without
 * comments. Block copies (several lines, or macros) go on their own lines
 * between #line directives. */
static niu_defer *niu_copy(niu_dctx *c, int id)
{
    niu_defer *d = &c->defers[id];
    unsigned line, col;
    if (d->copy_done)
        return d;
    d->copy_done = 1;
    if (d->shape == NIU_DS_COMPANION) {
        niu_buf b = {0};
        size_t name_len = strlen(d->def->name);
        niu_buf_printf(&b, "__niu_defer_%s", d->def->name);
        if (d->use_e > d->use_b + name_len)
            niu_buf_append(&b, c->src + d->use_b + name_len, d->use_e - d->use_b - name_len);
        if (d->use_semi)
            niu_buf_putc(&b, ';');
        d->copy = niu_buf_take(&b);
        d->copy_block = 1;
        niu_dline_col(c, d->use_b, &line, &col);
        d->copy_line = line;
        d->copy_indent = niu_line_indent(c->src, c->len, d->use_b);
        return d;
    }
    {
        niu_buf raw = {0};
        niu_render_range(c, id, d->sb, d->se, &raw);
        d->copy = niu_strip_comments(raw.data ? raw.data : "", raw.len);
        niu_buf_free(&raw);
    }
    d->copy_block = strchr(d->copy, '\n') != NULL || niu_has_expansion_in(c, d->sb, d->se);
    niu_dline_col(c, d->sb, &line, &col);
    d->copy_line = line;
    d->copy_indent = niu_line_indent(c->src, c->len, d->sb);
    return d;
}

static void niu_put_line_directive(niu_dctx *c, niu_buf *out, unsigned line)
{
    if (c->no_line)
        return;
    niu_buf_printf(out, "#line %u \"", line);
    niu_append_string_literal(out, niu_basename(c->input));
    niu_buf_printf(out, "\"%s", c->eol);
}

/* Do the copies need block form? */
static int niu_copies_block(niu_dctx *c, const niu_ilist *copies)
{
    size_t i;
    for (i = 0; i < copies->count; i++)
        if (niu_copy(c, copies->items[i])->copy_block)
            return 1;
    return 0;
}

/* Inline form: the copies on one line, separated by spaces. */
static void niu_put_copies_inline(niu_dctx *c, niu_buf *out, const niu_ilist *copies)
{
    size_t i;
    for (i = 0; i < copies->count; i++) {
        if (i)
            niu_buf_putc(out, ' ');
        niu_buf_puts(out, niu_copy(c, copies->items[i])->copy);
    }
}

/* Block form: a line break, then each copy on its own lines after a #line
 * naming its defer, then a #line back to the line of `cont` and that line's
 * indentation, so the code after the copies goes on where it was. */
static void niu_put_copies_block(niu_dctx *c, niu_buf *out, const niu_ilist *copies, size_t cont)
{
    size_t i;
    unsigned line, col;
    char *indent;
    niu_buf_puts(out, c->eol);
    for (i = 0; i < copies->count; i++) {
        niu_defer *d = niu_copy(c, copies->items[i]);
        niu_put_line_directive(c, out, d->copy_line);
        niu_buf_puts(out, d->copy_indent);
        niu_buf_puts(out, d->copy);
        niu_buf_puts(out, c->eol);
    }
    niu_dline_col(c, cont, &line, &col);
    niu_put_line_directive(c, out, line);
    indent = niu_line_indent(c->src, c->len, cont);
    niu_buf_puts(out, indent);
    free(indent);
}

static void niu_render_ledit(niu_dctx *c, const niu_ledit *l, niu_buf *out)
{
    int block = l->kind != NIU_L_TEXT && niu_copies_block(c, &l->copies);
    niu_buf x = {0};
    switch (l->kind) {
    case NIU_L_TEXT:
        niu_buf_puts(out, l->text);
        break;
    case NIU_L_END:
        /* "free(ptr); }" */
        if (block) {
            niu_put_copies_block(c, out, &l->copies, l->b);
        } else {
            niu_put_copies_inline(c, out, &l->copies);
            niu_buf_putc(out, ' ');
        }
        break;
    case NIU_L_JUMP:
        /* "{ free(ptr); break; }" */
        niu_render_range(c, l->owner, l->xb, l->xe, &x);
        niu_buf_putc(out, '{');
        if (block) {
            niu_put_copies_block(c, out, &l->copies, l->b);
        } else {
            niu_buf_putc(out, ' ');
            niu_put_copies_inline(c, out, &l->copies);
            niu_buf_putc(out, ' ');
        }
        niu_buf_append(out, x.data ? x.data : "", x.len);
        niu_buf_puts(out, "; }");
        break;
    case NIU_L_RETURN:
        /* "{ int __niu_return_temp = v; free(p); return __niu_return_temp; }" */
        niu_buf_putc(out, '{');
        if (l->decl || l->void_value) {
            niu_render_range(c, l->owner, l->xb, l->xe, &x);
            niu_buf_putc(out, ' ');
            if (l->decl)
                niu_buf_printf(out, "%s = ", l->decl);
            niu_buf_append(out, x.data ? x.data : "", x.len);
            niu_buf_putc(out, ';');
        }
        if (block) {
            niu_put_copies_block(c, out, &l->copies, l->e > l->b ? l->e - 1 : l->b);
        } else {
            niu_buf_putc(out, ' ');
            niu_put_copies_inline(c, out, &l->copies);
            niu_buf_putc(out, ' ');
        }
        niu_buf_puts(out, l->decl ? "return __niu_return_temp; }" : "return; }");
        break;
    case NIU_L_DEFER_NOW: {
        /* "/" "* defer_now; *" "/ { free(buf); fclose(f); }" */
        char *comment = niu_comment_out(c->src + l->b, l->e - l->b);
        niu_buf_puts(out, comment);
        free(comment);
        niu_buf_puts(out, " {");
        if (block) {
            niu_put_copies_block(c, out, &l->copies, l->e > l->b ? l->e - 1 : l->b);
        } else {
            niu_buf_putc(out, ' ');
            niu_put_copies_inline(c, out, &l->copies);
            niu_buf_putc(out, ' ');
        }
        niu_buf_putc(out, '}');
        break;
    }
    case NIU_L_VALUE:
        /* "size_t __niu_value_temp = strlen(s); free(s); __niu_value_temp;" */
        niu_render_range(c, l->owner, l->xb, l->xe, &x);
        niu_buf_printf(out, "%s = ", l->decl);
        niu_buf_append(out, x.data ? x.data : "", x.len);
        niu_buf_putc(out, ';');
        if (block) {
            niu_put_copies_block(c, out, &l->copies, l->e > l->b ? l->e - 1 : l->b);
        } else {
            niu_buf_putc(out, ' ');
            niu_put_copies_inline(c, out, &l->copies);
            niu_buf_putc(out, ' ');
        }
        niu_buf_puts(out, "__niu_value_temp;");
        break;
    case NIU_L_VALUE_AFTER:
        if (block) {
            niu_put_copies_block(c, out, &l->copies, l->b > 0 ? l->b - 1 : 0);
        } else {
            niu_buf_putc(out, ' ');
            niu_put_copies_inline(c, out, &l->copies);
        }
        break;
    }
    niu_buf_free(&x);
}

static int niu_ledit_cmp(const void *a, const void *b)
{
    const niu_ledit *x = *(const niu_ledit *const *)a, *y = *(const niu_ledit *const *)b;
    if (x->owner != y->owner)
        return x->owner < y->owner ? -1 : 1;
    if (x->b != y->b)
        return x->b < y->b ? -1 : 1;
    /* the larger edit first: it contains the other */
    if (x->e != y->e)
        return x->e > y->e ? -1 : 1;
    return x->seq < y->seq ? -1 : (x->seq > y->seq ? 1 : 0);
}

/* Appends the region's text for [b, e): the original with the region's
 * edits applied. Edits inside another edit are applied by that edit, which
 * renders the parts of the original it keeps through this function. */
static void niu_render_range(niu_dctx *c, int owner, size_t b, size_t e, niu_buf *out)
{
    size_t lo = 0, hi = c->n_ledits, i, pos = b;
    if (!c->sorted_ledits) {
        c->sorted_ledits = (niu_ledit **)niu_xmalloc((c->n_ledits + 1) * sizeof *c->sorted_ledits);
        for (i = 0; i < c->n_ledits; i++)
            c->sorted_ledits[i] = &c->ledits[i];
        qsort(c->sorted_ledits, c->n_ledits, sizeof *c->sorted_ledits, niu_ledit_cmp);
    }
    /* the first edit of this owner at or after b */
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const niu_ledit *m = c->sorted_ledits[mid];
        if (m->owner < owner || (m->owner == owner && m->b < b))
            lo = mid + 1;
        else
            hi = mid;
    }
    for (i = lo; i < c->n_ledits; i++) {
        niu_ledit *l = c->sorted_ledits[i];
        if (l->owner != owner || l->b > e || (l->b == e && b != e))
            break;
        if (l->e > e)
            continue;
        if (l->b < pos) {
            if (l->e > pos)
                c->internal_error = 1; /* overlapping edits: a bug in niu */
            continue;
        }
        niu_buf_append(out, c->src + pos, l->b - pos);
        niu_render_ledit(c, l, out);
        pos = l->e;
    }
    if (e > pos)
        niu_buf_append(out, c->src + pos, e - pos);
}

/* The lowered text of the whole file (without niu's header lines). */
static char *niu_lower_file(niu_dctx *c)
{
    niu_buf out = {0};
    niu_render_range(c, -1, 0, c->len, &out);
    return niu_buf_take(&out);
}
