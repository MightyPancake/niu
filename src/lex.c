/* lex.c - a small C lexer over raw source text. It knows comments, string
 * and character literals, line continuations and preprocessor directives,
 * which is all niu's text passes need: the token pre-pass, finding the ';'
 * after a statement, and leaving comments out of copies. Part of niu.c; not
 * compiled on its own. */

enum { NIU_TOK_EOF, NIU_TOK_IDENT, NIU_TOK_NUMBER, NIU_TOK_STRING, NIU_TOK_CHAR, NIU_TOK_PUNCT };

typedef struct {
    int kind;
    size_t b, e; /* the token is s[b, e) */
    int bol;     /* first token on its (logical) line */
    int spliced; /* contains a line continuation */
} niu_tok;

typedef struct {
    const char *s;
    size_t len;
    size_t pos;
    int bol; /* the next token is the first on its line */
} niu_lexer;

static void niu_lex_init(niu_lexer *lx, const char *s, size_t len, size_t pos)
{
    lx->s = s;
    lx->len = len;
    lx->pos = pos;
    lx->bol = 1;
}

/* Length of a line continuation (backslash, optional CR, LF) at i, or 0. */
static size_t niu_splice_at(const char *s, size_t len, size_t i)
{
    if (i < len && s[i] == '\\') {
        if (i + 1 < len && s[i + 1] == '\n')
            return 2;
        if (i + 2 < len && s[i + 1] == '\r' && s[i + 2] == '\n')
            return 3;
    }
    return 0;
}

/* Skips one comment at i; returns its end, or i when there is none. *nl is
 * set when a line comment ends at a newline (which is not consumed). */
static size_t niu_skip_comment(const char *s, size_t len, size_t i)
{
    size_t k;
    if (i + 1 >= len || s[i] != '/')
        return i;
    if (s[i + 1] == '*') {
        for (k = i + 2; k + 1 < len; k++)
            if (s[k] == '*' && s[k + 1] == '/')
                return k + 2;
        return len;
    }
    if (s[i + 1] == '/') {
        for (k = i + 2; k < len; k++) {
            size_t sp = niu_splice_at(s, len, k);
            if (sp) {
                k += sp - 1;
                continue;
            }
            if (s[k] == '\n' || (s[k] == '\r' && k + 1 < len && s[k + 1] == '\n'))
                return k;
        }
        return len;
    }
    return i;
}

/* Skips spaces, comments and line continuations from i. Newlines are
 * skipped only when skip_newlines is set; *saw_newline reports them. */
static size_t niu_skip_space(const char *s, size_t len, size_t i, int skip_newlines, int *saw_newline)
{
    for (;;) {
        size_t sp, c;
        if (i >= len)
            return i;
        if (s[i] == ' ' || s[i] == '\t' || s[i] == '\f' || s[i] == '\v') {
            i++;
            continue;
        }
        if (s[i] == '\r' && i + 1 < len && s[i + 1] == '\n') {
            if (!skip_newlines)
                return i;
            if (saw_newline)
                *saw_newline = 1;
            i += 2;
            continue;
        }
        if (s[i] == '\n') {
            if (!skip_newlines)
                return i;
            if (saw_newline)
                *saw_newline = 1;
            i++;
            continue;
        }
        sp = niu_splice_at(s, len, i);
        if (sp) {
            i += sp;
            continue;
        }
        c = niu_skip_comment(s, len, i);
        if (c != i) {
            i = c;
            continue;
        }
        return i;
    }
}

static int niu_is_ident_start(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || (unsigned char)c >= 0x80;
}

static int niu_is_ident_char(int c)
{
    return niu_is_ident_start(c) || (c >= '0' && c <= '9');
}

/* The next token, or NIU_TOK_EOF. Newlines are skipped (they set bol). */
static niu_tok niu_lex_next(niu_lexer *lx)
{
    const char *s = lx->s;
    size_t len = lx->len, i;
    int nl = 0;
    niu_tok t;
    i = niu_skip_space(s, len, lx->pos, 1, &nl);
    if (nl)
        lx->bol = 1;
    t.b = i;
    t.bol = lx->bol;
    t.spliced = 0;
    lx->bol = 0;
    if (i >= len) {
        t.kind = NIU_TOK_EOF;
        t.e = i;
        lx->pos = i;
        return t;
    }
    /* string and character literals, with their prefixes */
    {
        size_t q = i;
        if (s[q] == 'u' && q + 1 < len && s[q + 1] == '8')
            q += 2;
        else if (s[q] == 'u' || s[q] == 'U' || s[q] == 'L')
            q += 1;
        if (q < len && (s[q] == '"' || s[q] == '\'')) {
            char quote = s[q];
            size_t k = q + 1;
            while (k < len && s[k] != quote && s[k] != '\n') {
                if (s[k] == '\\' && k + 1 < len) {
                    size_t sp = niu_splice_at(s, len, k);
                    k += sp ? sp : 2;
                    continue;
                }
                k++;
            }
            if (k < len && s[k] == quote)
                k++;
            t.kind = quote == '"' ? NIU_TOK_STRING : NIU_TOK_CHAR;
            t.e = k;
            lx->pos = k;
            return t;
        }
    }
    if (niu_is_ident_start(s[i])) {
        size_t k = i;
        for (;;) {
            size_t sp = niu_splice_at(s, len, k);
            if (sp && k + sp < len && niu_is_ident_char(s[k + sp])) {
                t.spliced = 1;
                k += sp;
                continue;
            }
            if (k < len && niu_is_ident_char(s[k])) {
                k++;
                continue;
            }
            break;
        }
        t.kind = NIU_TOK_IDENT;
        t.e = k;
        lx->pos = k;
        return t;
    }
    if ((s[i] >= '0' && s[i] <= '9') || (s[i] == '.' && i + 1 < len && s[i + 1] >= '0' && s[i + 1] <= '9')) {
        size_t k = i + 1;
        while (k < len) {
            char c = s[k];
            if ((c == '+' || c == '-') &&
                (s[k - 1] == 'e' || s[k - 1] == 'E' || s[k - 1] == 'p' || s[k - 1] == 'P')) {
                k++;
                continue;
            }
            if (niu_is_ident_char(c) || c == '.' || (c == '\'' && k + 1 < len && niu_is_ident_char(s[k + 1]))) {
                k++;
                continue;
            }
            break;
        }
        t.kind = NIU_TOK_NUMBER;
        t.e = k;
        lx->pos = k;
        return t;
    }
    t.kind = NIU_TOK_PUNCT;
    t.e = i + 1;
    if (s[i] == '#' && i + 1 < len && s[i + 1] == '#')
        t.e = i + 2;
    lx->pos = t.e;
    return t;
}

/* Does the token spell exactly `word` (without line continuations)? */
static int niu_tok_is(const char *s, const niu_tok *t, const char *word)
{
    size_t n = strlen(word);
    return !t->spliced && t->e - t->b == n && memcmp(s + t->b, word, n) == 0;
}

/* The offset of the first character after s[i..] that is not a space or a
 * comment (newlines included). */
static size_t niu_next_code(const char *s, size_t len, size_t i)
{
    return niu_skip_space(s, len, i, 1, NULL);
}

/* The start of the line containing offset i. */
static size_t niu_line_start(const char *s, size_t i)
{
    while (i > 0 && s[i - 1] != '\n')
        i--;
    return i;
}

/* The spaces and tabs at the start of the line containing offset i. */
static char *niu_line_indent(const char *s, size_t len, size_t i)
{
    size_t b = niu_line_start(s, i), e = b;
    while (e < len && (s[e] == ' ' || s[e] == '\t'))
        e++;
    return niu_strndup(s + b, e - b);
}

/* text with its comments left out: a block comment becomes one space (none
 * next to other spaces), a line comment disappears up to its newline.
 * Strings and character literals are kept as they are. Trailing spaces are
 * trimmed. */
static char *niu_strip_comments(const char *s, size_t len)
{
    niu_buf out = {0};
    size_t i = 0;
    while (i < len) {
        size_t c = niu_skip_comment(s, len, i);
        if (c != i) {
            int block = s[i + 1] == '*';
            i = c;
            if (block && out.len > 0 && out.data[out.len - 1] != ' ' && out.data[out.len - 1] != '\n' &&
                i < len && s[i] != ' ' && s[i] != '\n' && s[i] != '\r')
                niu_buf_putc(&out, ' ');
            continue;
        }
        if (s[i] == '"' || s[i] == '\'') {
            char quote = s[i];
            size_t k = i + 1;
            while (k < len && s[k] != quote && s[k] != '\n') {
                if (s[k] == '\\' && k + 1 < len) {
                    k += 2;
                    continue;
                }
                k++;
            }
            if (k < len && s[k] == quote)
                k++;
            niu_buf_append(&out, s + i, k - i);
            i = k;
            continue;
        }
        niu_buf_putc(&out, s[i]);
        i++;
    }
    /* trim trailing spaces on every line and at the end */
    {
        niu_buf t = {0};
        size_t k = 0;
        while (k < out.len) {
            size_t e = k;
            while (e < out.len && out.data[e] != '\n')
                e++;
            {
                size_t te = e;
                while (te > k && (out.data[te - 1] == ' ' || out.data[te - 1] == '\t' || out.data[te - 1] == '\r'))
                    te--;
                niu_buf_append(&t, out.data + k, te - k);
                /* keep a CR that belongs to a CRLF line ending */
                if (e < out.len && e > k && out.data[e - 1] == '\r')
                    niu_buf_putc(&t, '\r');
            }
            if (e < out.len)
                niu_buf_putc(&t, '\n');
            k = e + 1;
        }
        niu_buf_free(&out);
        return niu_buf_take(&t);
    }
}
