/* frontend.c - parsing a file with libclang, reporting its diagnostics, and
 * walking the result to build the edit list. Part of niu.c; not compiled on
 * its own. */

typedef struct {
    const char *input;      /* the file to process */
    niu_strlist user_args;  /* -I/-D/-U/-std= etc., passed to libclang */
    const char *target;     /* --target=, or NULL for the default */
    int no_line;            /* --no-line */
    int no_query_cc;        /* --no-query-cc */
    int warn_file_name;     /* -Wniu-file-name (on by default) */
    int warn_plain_file;    /* -Wniu-plain-file (on by default) */
    int warn_empty_defer_now; /* -Wniu-empty-defer-now (on by default) */
    int verbose;            /* -v */
} niu_options;

/* The target libclang parses for. On Windows the default follows the
 * compiler niu itself was built with, whichever default target the libclang
 * build has: a niu built with mingw parses for mingw (and asks gcc for its
 * headers), any other niu for MSVC, so the MSVC and Windows SDK headers parse
 * the way cl sees them. */
#if defined(_M_ARM64) || defined(__aarch64__)
#define NIU_WIN_ARCH "aarch64"
#elif defined(_M_IX86) || defined(__i386__)
#define NIU_WIN_ARCH "i686"
#else
#define NIU_WIN_ARCH "x86_64"
#endif

static char *niu_target_triple(const niu_options *o)
{
    if (o->target)
        return niu_strdup(o->target);
#if defined(__MINGW32__)
    return niu_strdup(NIU_WIN_ARCH "-w64-windows-gnu");
#elif defined(_WIN32)
    return niu_strdup(NIU_WIN_ARCH "-pc-windows-msvc");
#else
    return NULL;
#endif
}

/* The compiler niu asks for system include directories when $CC is not set.
 * Not every mingw has a `cc`, but every one has gcc. */
#ifdef __MINGW32__
#define NIU_DEFAULT_CC "gcc"
#else
#define NIU_DEFAULT_CC "cc"
#endif

/* Asks the target C compiler for its system include directories, the way
 * clangd's --query-driver does: `$CC -E -v -x c <empty>` prints them between
 * "#include <...> search starts here:" and "End of search list.". A bare
 * libclang often lacks some of them (on NixOS, glibc's headers live in a
 * directory only the cc wrapper knows). They are added with -idirafter, so
 * libclang's own directories still come first. */
static void niu_query_cc(const niu_options *o, niu_strlist *args)
{
    const char *cc = getenv("CC");
    const char *qargs[6];
    niu_buf out = {0};
    const char *p;
    int in_list = 0;
    if (!cc || !*cc)
        cc = NIU_DEFAULT_CC;
    qargs[0] = "-E";
    qargs[1] = "-v";
    qargs[2] = "-x";
    qargs[3] = "c";
    qargs[4] = niu_null_device();
    qargs[5] = NULL;
    if (niu_run_capture(cc, qargs, &out) != 0) {
        if (o->verbose)
            niu_message("note", "could not ask '%s' for its include directories", cc);
        niu_buf_free(&out);
        return;
    }
    for (p = niu_buf_str(&out); *p;) {
        size_t n = strcspn(p, "\r\n");
        char *line = niu_strndup(p, n);
        char *s = line;
        size_t m;
        p += n;
        while (*p == '\r' || *p == '\n')
            p++;
        while (*s == ' ' || *s == '\t')
            s++;
        m = strlen(s);
        while (m > 0 && (s[m - 1] == ' ' || s[m - 1] == '\t'))
            s[--m] = '\0';
        if (niu_starts_with(s, "#include <...> search starts here:")) {
            in_list = 1;
        } else if (niu_starts_with(s, "End of search list.")) {
            in_list = 0;
        } else if (in_list && m > 0) {
            static const char fw[] = " (framework directory)";
            if (niu_ends_with(s, fw)) {
                s[m - (sizeof fw - 1)] = '\0';
                niu_strlist_push(args, "-iframework");
            } else {
                niu_strlist_push(args, "-idirafter");
            }
            niu_strlist_push(args, s);
        }
        free(line);
    }
    niu_buf_free(&out);
}

#ifdef __APPLE__
/* The macOS SDK: $SDKROOT, else `xcrun --show-sdk-path`. */
static void niu_add_sysroot(niu_strlist *args)
{
    const char *sdk = getenv("SDKROOT");
    char *found = NULL;
    if (!sdk || !*sdk)
        sdk = found = niu_command_line_output("xcrun", "--show-sdk-path");
    if (sdk && *sdk) {
        niu_strlist_push(args, "-isysroot");
        niu_strlist_push(args, sdk);
    }
    free(found);
}
#endif

/* Everything niu knows about one parsed file. */
typedef struct {
    const niu_options *opts;
    CXTranslationUnit tu;
    const char *src; /* the original text, as given to libclang */
    size_t len;
    CXFile niu_h;     /* the embedded niu.h, or NULL when nothing included it */
    char *niu_h_path; /* its path inside the virtual directory */
    int is_niu_file;  /* the input includes <niu.h> itself */
    niu_strlist niu_headers; /* other files that include <niu.h> (not yet supported) */
    size_t niu_h_off;        /* where the input's `#include <niu.h>` is, or (size_t)-1 */
    size_t *niu_h_ranges;    /* every `#include <niu.h>` in the input: begin, end, ... */
    size_t n_niu_h;
    niu_prepass pp;          /* the token pre-pass; pp.text is what libclang reads */
    char *lowered;           /* the lowered text (without niu's header lines) */
    int errors;
} niu_unit;

static const char *niu_severity_name(int sev)
{
    switch (sev) {
    case CXDiagnostic_Note:
        return "note";
    case CXDiagnostic_Warning:
        return "warning";
    case CXDiagnostic_Error:
        return "error";
    case CXDiagnostic_Fatal:
        return "fatal error";
    default:
        return "remark";
    }
}

/* "file:line:col" for a location as the user's files name it (#line
 * directives in them are respected), or NULL when it has no file. */
static char *niu_location_string(CXSourceLocation loc)
{
    CXString fname;
    unsigned line = 0, col = 0;
    char *file, *r = NULL;
    cx.clang_getPresumedLocation(loc, &fname, &line, &col);
    file = niu_cxstr(fname);
    if (*file)
        r = niu_format("%s:%u:%u", file, line, col);
    free(file);
    return r;
}

static void niu_print_diagnostic(CXDiagnostic d)
{
    int sev = cx.clang_getDiagnosticSeverity(d);
    char *where, *msg, *opt;
    CXString disable;
    CXDiagnosticSet children;
    unsigned i, n;
    if (sev == CXDiagnostic_Ignored)
        return;
    where = niu_location_string(cx.clang_getDiagnosticLocation(d));
    msg = niu_cxstr(cx.clang_getDiagnosticSpelling(d));
    opt = niu_cxstr(cx.clang_getDiagnosticOption(d, &disable));
    cx.clang_disposeString(disable);
    fprintf(stderr, "%s: %s: %s", where ? where : "niu", niu_severity_name(sev), msg);
    if (*opt)
        fprintf(stderr, " [%s]", opt);
    fputc('\n', stderr);
    free(where);
    free(msg);
    free(opt);
    /* Notes attached to this diagnostic. The set belongs to d. */
    children = cx.clang_getChildDiagnostics(d);
    n = children ? cx.clang_getNumDiagnosticsInSet(children) : 0;
    for (i = 0; i < n; i++) {
        CXDiagnostic c = cx.clang_getDiagnosticInSet(children, i);
        niu_print_diagnostic(c);
        cx.clang_disposeDiagnostic(c);
    }
}

/* Prints libclang's diagnostics; returns the number of errors. A
 * diagnostic on a niu keyword is left out: it is about the C the pre-pass
 * put there, and niu reports what is wrong with the keyword itself. */
static int niu_report_diagnostics(CXTranslationUnit tu, niu_prepass *pp, CXFile main_file)
{
    unsigned i, n = cx.clang_getNumDiagnostics(tu);
    int errors = 0;
    for (i = 0; i < n; i++) {
        CXDiagnostic d = cx.clang_getDiagnostic(tu, i);
        int sev = cx.clang_getDiagnosticSeverity(d);
        CXFile f = NULL;
        unsigned line, col, off = 0;
        cx.clang_getExpansionLocation(cx.clang_getDiagnosticLocation(d), &f, &line, &col, &off);
        if (pp->n_markers && f && main_file && cx.clang_File_isEqual(f, main_file) && niu_marker_covering(pp, off)) {
            cx.clang_disposeDiagnostic(d);
            continue;
        }
        if (sev == CXDiagnostic_Error || sev == CXDiagnostic_Fatal)
            errors++;
        niu_print_diagnostic(d);
        cx.clang_disposeDiagnostic(d);
    }
    return errors;
}

/* Byte offset and file of a location, as spelled in the source. */
static CXFile niu_offset_of(CXSourceLocation loc, size_t *offset)
{
    CXFile f = NULL;
    unsigned line, col, off = 0;
    cx.clang_getSpellingLocation(loc, &f, &line, &col, &off);
    *offset = off;
    return f;
}

static int niu_is_niu_h(niu_unit *u, CXFile f)
{
    if (!f)
        return 0;
    if (!u->niu_h)
        u->niu_h = cx.clang_getFile(u->tu, u->niu_h_path);
    return u->niu_h && cx.clang_File_isEqual(f, u->niu_h);
}

/* An #include. `#include <niu.h>` in the input becomes a comment (spec.md
 * "Source files"); in any other file it makes that file a niu header,
 * which milestone 1 cannot lower yet. */
static void niu_visit_include(niu_unit *u, CXCursor c)
{
    CXSourceRange r;
    CXFile file;
    size_t begin, end;
    if (!niu_is_niu_h(u, cx.clang_getIncludedFile(c)))
        return;
    r = cx.clang_getCursorExtent(c);
    file = niu_offset_of(cx.clang_getRangeStart(r), &begin);
    niu_offset_of(cx.clang_getRangeEnd(r), &end);
    if (!cx.clang_Location_isFromMainFile(cx.clang_getCursorLocation(c))) {
        char *name = niu_cxstr(cx.clang_getFileName(file));
        if (!niu_strlist_contains(&u->niu_headers, name)) {
            char *where = niu_location_string(cx.clang_getCursorLocation(c));
            niu_message_at(where ? where : name, "error",
                           "'%s' includes <niu.h>, but lowering niu headers is not implemented yet", name);
            free(where);
            niu_strlist_push(&u->niu_headers, name);
            u->errors++;
        }
        free(name);
        return;
    }
    if (end > u->len || begin >= end || u->src[begin] != '#') {
        niu_message_at(u->opts->input, "error", "internal error: unexpected extent %lu..%lu for #include <niu.h>",
                       (unsigned long)begin, (unsigned long)end);
        u->errors++;
        return;
    }
    if (!u->is_niu_file)
        u->niu_h_off = begin;
    u->niu_h_ranges = (size_t *)niu_xrealloc(u->niu_h_ranges, (2 * u->n_niu_h + 2) * sizeof *u->niu_h_ranges);
    u->niu_h_ranges[2 * u->n_niu_h] = begin;
    u->niu_h_ranges[2 * u->n_niu_h + 1] = end;
    u->n_niu_h++;
    u->is_niu_file = 1;
}

/* The walker. Preprocessing cursors (#include) are top-level children of the
 * translation unit, so they are seen for every file; declarations and
 * statements are walked only in the input file, since only its code is
 * rewritten. niu features hook in here as they are implemented. */
static enum CXChildVisitResult niu_visit(CXCursor c, CXCursor parent, CXClientData data)
{
    niu_unit *u = (niu_unit *)data;
    (void)parent;
    if (cx.clang_getCursorKind(c) == CXCursor_InclusionDirective) {
        niu_visit_include(u, c);
        return CXChildVisit_Continue;
    }
    if (!cx.clang_Location_isFromMainFile(cx.clang_getCursorLocation(c)))
        return CXChildVisit_Continue;
    return CXChildVisit_Recurse;
}

/* One parse of the pre-passed text. */
static CXTranslationUnit niu_parse(const niu_options *o, CXIndex index, niu_strlist *args, niu_unit *u, int *rc)
{
    struct CXUnsavedFile unsaved[2];
    CXTranslationUnit tu = NULL;
    unsigned flags = CXTranslationUnit_DetailedPreprocessingRecord | CXTranslationUnit_KeepGoing;
    unsaved[0].Filename = o->input;
    unsaved[0].Contents = u->pp.text;
    unsaved[0].Length = (unsigned long)u->len;
    unsaved[1].Filename = u->niu_h_path;
    unsaved[1].Contents = niu_h_text;
    unsaved[1].Length = (unsigned long)(sizeof niu_h_text - 1);
    *rc = cx.clang_parseTranslationUnit2(index, o->input, (const char *const *)args->items, (int)args->count, unsaved,
                                         2, flags, &tu);
    return tu;
}

/* Parses the input and walks it. The text libclang reads is the input after
 * the token pre-pass, which keeps every offset; so the byte offsets libclang
 * reports are offsets into the original text, which is what niu edits.
 * Returns 1 when there were no errors; u->lowered is then the output. */
static int niu_process(const niu_options *o, const char *src, size_t len, niu_unit *u)
{
    niu_strlist args = {0};
    char *root = niu_virtual_root();
    char *incdir, *target;
    CXIndex index;
    int rc, ok = 0, attempt;
    size_t i, found;

    memset(u, 0, sizeof *u);
    u->opts = o;
    u->src = src;
    u->len = len;
    u->niu_h_off = (size_t)-1;
    incdir = niu_path_join(root, "include");
    u->niu_h_path = niu_path_join(incdir, "niu.h");

    /* niu's include directory comes first, so <niu.h> always means the
     * embedded header, even when a copy of it is on an -I path for editors. */
    niu_strlist_push(&args, "-x");
    niu_strlist_push(&args, "c");
    niu_strlist_push(&args, "-D__NIU_STD__=1");
    niu_strlist_push_owned(&args, niu_format("-I%s", incdir));
    target = niu_target_triple(o);
    if (target)
        niu_strlist_push_owned(&args, niu_format("--target=%s", target));
    for (i = 0; i < o->user_args.count; i++)
        niu_strlist_push(&args, o->user_args.items[i]);
#ifdef __APPLE__
    niu_add_sysroot(&args);
#endif
    if (!o->no_query_cc && !(target && strstr(target, "msvc")))
        niu_query_cc(o, &args);
    if (o->verbose) {
        fprintf(stderr, "niu: note: libclang arguments:");
        for (i = 0; i < args.count; i++)
            fprintf(stderr, " %s", args.items[i]);
        fputc('\n', stderr);
    }

    index = cx.clang_createIndex(0, 0);
    /* The pre-pass needs to know where `#include <niu.h>` is, and the text
     * can only guess (it might be inside #if 0). When the parse finds it
     * somewhere else, the pre-pass runs again from there. */
    found = niu_find_niu_include(src, len);
    for (attempt = 0; attempt < 2; attempt++) {
        niu_prepass_run(&u->pp, src, len, found);
        u->tu = niu_parse(o, index, &args, u, &rc);
        if (rc != CXError_Success || !u->tu)
            break;
        u->is_niu_file = 0;
        u->niu_h_off = (size_t)-1;
        u->n_niu_h = 0;
        u->niu_h = NULL; /* belongs to the previous parse */
        cx.clang_visitChildren(cx.clang_getTranslationUnitCursor(u->tu), niu_visit, u);
        if (u->niu_h_off == found || attempt == 1)
            break;
        found = u->niu_h_off;
        u->errors = 0;
        niu_strlist_free(&u->niu_headers);
        cx.clang_disposeTranslationUnit(u->tu);
        u->tu = NULL;
        niu_prepass_free(&u->pp);
    }
    if (rc != CXError_Success || !u->tu) {
        niu_message_at(o->input, "error", "libclang could not parse the file (error code %d)", rc);
        u->errors++;
    } else {
        niu_dctx dc;
        CXFile main_file = cx.clang_getFile(u->tu, o->input);
        memset(&dc, 0, sizeof dc);
        dc.tu = u->tu;
        dc.main_file = main_file;
        dc.input = o->input;
        dc.src = src;
        dc.len = len;
        dc.eol = niu_line_ending(src, len);
        dc.pp = &u->pp;
        dc.no_line = o->no_line;
        dc.warn_empty_defer_now = o->warn_empty_defer_now;
        u->errors += niu_report_diagnostics(u->tu, &u->pp, main_file);
        if (u->is_niu_file) {
            /* every `#include <niu.h>` line becomes a comment */
            for (i = 0; i < u->n_niu_h; i++) {
                size_t b = u->niu_h_ranges[2 * i], e = u->niu_h_ranges[2 * i + 1];
                niu_ledit_text(&dc, -1, b, e, niu_comment_out(src + b, e - b));
            }
            niu_defer_analyze(&dc);
            niu_print_ddiags(&dc);
            u->errors += dc.errors;
        }
        if (!u->is_niu_file && niu_has_niu_name(o->input) && o->warn_file_name)
            niu_message_at(o->input, "warning",
                           "'%s' is named like a niu file but does not include <niu.h> [-Wniu-file-name]",
                           niu_basename(o->input));
        if (u->errors == 0) {
            u->lowered = niu_lower_file(&dc);
            if (dc.internal_error) {
                niu_message_at(o->input, "error", "internal error: overlapping edits");
                u->errors++;
            }
        }
        ok = u->errors == 0;
        niu_dctx_free(&dc);
        cx.clang_disposeTranslationUnit(u->tu);
        u->tu = NULL;
    }
    cx.clang_disposeIndex(index);
    niu_strlist_free(&args);
    free(target);
    free(incdir);
    free(root);
    return ok;
}

static void niu_unit_free(niu_unit *u)
{
    niu_prepass_free(&u->pp);
    free(u->lowered);
    free(u->niu_h_ranges);
    niu_strlist_free(&u->niu_headers);
    free(u->niu_h_path);
}
