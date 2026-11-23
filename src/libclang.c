/* libclang.c - the part of the libclang C API that niu uses, declared here
 * instead of including clang-c/Index.h, and the code that finds and loads
 * libclang at run time. Part of niu.c; not compiled on its own.
 *
 * The declarations match clang-c/Index.h of LLVM 18 and later. Every
 * function is looked up with dlsym/GetProcAddress, and niu refuses a
 * libclang that lacks any of them. */

/* ---- types ---- */

typedef void *CXIndex;
typedef void *CXTranslationUnit;
typedef void *CXFile;
typedef void *CXDiagnostic;
typedef void *CXDiagnosticSet;
typedef void *CXClientData;

typedef struct {
    const void *data;
    unsigned private_flags;
} CXString;

typedef struct {
    const void *ptr_data[2];
    unsigned int_data;
} CXSourceLocation;

typedef struct {
    const void *ptr_data[2];
    unsigned begin_int_data;
    unsigned end_int_data;
} CXSourceRange;

/* kind is an enum CXCursorKind in Index.h; enums are int-sized here. */
typedef struct {
    int kind;
    int xdata;
    const void *data[3];
} CXCursor;

/* kind is an enum CXTypeKind. */
typedef struct {
    int kind;
    void *data[2];
} CXType;

struct CXUnsavedFile {
    const char *Filename;
    const char *Contents;
    unsigned long Length;
};

/* enum CXErrorCode */
#define CXError_Success 0
#define CXError_Failure 1
#define CXError_Crashed 2
#define CXError_InvalidArguments 3
#define CXError_ASTReadError 4

/* enum CXDiagnosticSeverity */
#define CXDiagnostic_Ignored 0
#define CXDiagnostic_Note 1
#define CXDiagnostic_Warning 2
#define CXDiagnostic_Error 3
#define CXDiagnostic_Fatal 4

/* enum CXTranslationUnit_Flags */
#define CXTranslationUnit_DetailedPreprocessingRecord 0x01
#define CXTranslationUnit_KeepGoing 0x200

/* enum CXCursorKind (only the kinds niu looks at) */
#define CXCursor_InclusionDirective 503

/* enum CXChildVisitResult */
enum CXChildVisitResult { CXChildVisit_Break, CXChildVisit_Continue, CXChildVisit_Recurse };

typedef enum CXChildVisitResult (*CXCursorVisitor)(CXCursor cursor, CXCursor parent, CXClientData client_data);

/* ---- functions ---- */

/* X(return type, name, parameters). All of them must exist. */
#define NIU_CLANG_FUNCTIONS(X)                                                                                         \
    X(CXString, clang_getClangVersion, (void))                                                                         \
    X(const char *, clang_getCString, (CXString))                                                                      \
    X(void, clang_disposeString, (CXString))                                                                           \
    X(CXIndex, clang_createIndex, (int, int))                                                                          \
    X(void, clang_disposeIndex, (CXIndex))                                                                             \
    X(int, clang_parseTranslationUnit2,                                                                                \
      (CXIndex, const char *, const char *const *, int, struct CXUnsavedFile *, unsigned, unsigned,                    \
       CXTranslationUnit *))                                                                                           \
    X(void, clang_disposeTranslationUnit, (CXTranslationUnit))                                                         \
    X(unsigned, clang_getNumDiagnostics, (CXTranslationUnit))                                                          \
    X(CXDiagnostic, clang_getDiagnostic, (CXTranslationUnit, unsigned))                                                \
    X(void, clang_disposeDiagnostic, (CXDiagnostic))                                                                   \
    X(int, clang_getDiagnosticSeverity, (CXDiagnostic))                                                                \
    X(CXSourceLocation, clang_getDiagnosticLocation, (CXDiagnostic))                                                   \
    X(CXString, clang_getDiagnosticSpelling, (CXDiagnostic))                                                           \
    X(CXString, clang_getDiagnosticOption, (CXDiagnostic, CXString *))                                                 \
    X(CXDiagnosticSet, clang_getChildDiagnostics, (CXDiagnostic))                                                      \
    X(unsigned, clang_getNumDiagnosticsInSet, (CXDiagnosticSet))                                                       \
    X(CXDiagnostic, clang_getDiagnosticInSet, (CXDiagnosticSet, unsigned))                                             \
    X(void, clang_getPresumedLocation, (CXSourceLocation, CXString *, unsigned *, unsigned *))                         \
    X(void, clang_getSpellingLocation, (CXSourceLocation, CXFile *, unsigned *, unsigned *, unsigned *))               \
    X(int, clang_Location_isFromMainFile, (CXSourceLocation))                                                          \
    X(CXCursor, clang_getTranslationUnitCursor, (CXTranslationUnit))                                                   \
    X(unsigned, clang_visitChildren, (CXCursor, CXCursorVisitor, CXClientData))                                        \
    X(int, clang_getCursorKind, (CXCursor))                                                                            \
    X(CXSourceLocation, clang_getCursorLocation, (CXCursor))                                                           \
    X(CXSourceRange, clang_getCursorExtent, (CXCursor))                                                                \
    X(CXSourceLocation, clang_getRangeStart, (CXSourceRange))                                                          \
    X(CXSourceLocation, clang_getRangeEnd, (CXSourceRange))                                                            \
    X(CXFile, clang_getIncludedFile, (CXCursor))                                                                       \
    X(CXFile, clang_getFile, (CXTranslationUnit, const char *))                                                        \
    X(CXString, clang_getFileName, (CXFile))                                                                           \
    X(int, clang_File_isEqual, (CXFile, CXFile))                                                                       \
    X(void, clang_getExpansionLocation, (CXSourceLocation, CXFile *, unsigned *, unsigned *, unsigned *))             \
    X(CXString, clang_getCursorSpelling, (CXCursor))                                                                   \
    X(CXCursor, clang_getCursorReferenced, (CXCursor))                                                                 \
    X(unsigned, clang_equalCursors, (CXCursor, CXCursor))                                                              \
    X(int, clang_Cursor_isNull, (CXCursor))                                                                            \
    X(unsigned, clang_isAttribute, (int))                                                                              \
    X(CXType, clang_getCursorType, (CXCursor))                                                                         \
    X(CXType, clang_getCursorResultType, (CXCursor))                                                                   \
    X(CXType, clang_getUnqualifiedType, (CXType))                                                                      \
    X(CXType, clang_getCanonicalType, (CXType))                                                                        \
    X(CXString, clang_getTypeSpelling, (CXType))                                                                       \
    X(unsigned, clang_equalTypes, (CXType, CXType))                                                                    \
    X(int, clang_Cursor_getStorageClass, (CXCursor))                                                                   \
    X(int, clang_getCursorTLSKind, (CXCursor))                                                                         \
    X(unsigned, clang_Cursor_isMacroFunctionLike, (CXCursor))                                                          \
    X(const char *, clang_getFileContents, (CXTranslationUnit, CXFile, size_t *))                                    \
    X(CXType, clang_getPointeeType, (CXType))                                                                          \
    X(CXType, clang_getArrayElementType, (CXType))                                                                     \
    X(long long, clang_getArraySize, (CXType))                                                                         \
    X(CXType, clang_getResultType, (CXType))                                                                           \
    X(int, clang_getNumArgTypes, (CXType))                                                                             \
    X(CXType, clang_getArgType, (CXType, unsigned))                                                                    \
    X(unsigned, clang_isFunctionTypeVariadic, (CXType))                                                                \
    X(int, clang_getFunctionTypeCallingConv, (CXType))                                                                 \
    X(CXCursor, clang_getTypeDeclaration, (CXType))                                                                    \
    X(CXType, clang_getTypedefDeclUnderlyingType, (CXCursor))                                                          \
    X(CXType, clang_Type_getNamedType, (CXType))                                                                       \
    X(CXType, clang_Type_getModifiedType, (CXType))                                                                    \
    X(CXType, clang_Type_getValueType, (CXType))                                                                       \
    X(unsigned, clang_isConstQualifiedType, (CXType))                                                                  \
    X(unsigned, clang_isVolatileQualifiedType, (CXType))                                                               \
    X(unsigned, clang_isRestrictQualifiedType, (CXType))                                                               \
    X(CXCursor, clang_getCanonicalCursor, (CXCursor))                                                                  \
    X(CXCursor, clang_getCursorSemanticParent, (CXCursor))

/* Functions niu does not call (yet), but whose presence shows that the
 * library is at least LLVM 17. LLVM 18 added no C API functions, so the
 * last step to 18 is checked with the version string (see below). */
static const char *const niu_clang_markers[] = {
    "clang_createIndexWithOptions",       /* LLVM 17 */
    "clang_getBinaryOperatorKindSpelling", /* LLVM 17 */
};

#define NIU_LIBCLANG_MIN_MAJOR 18

typedef struct {
#define NIU_X(ret, name, params) ret(*name) params;
    NIU_CLANG_FUNCTIONS(NIU_X)
#undef NIU_X
    void *handle;
    char *path;    /* the library file that was loaded */
    char *version; /* clang_getClangVersion() */
} niu_libclang;

static niu_libclang cx;

/* Copies a CXString into a malloc'd C string and disposes it. */
static char *niu_cxstr(CXString s)
{
    const char *c = cx.clang_getCString(s);
    char *r = niu_strdup(c ? c : "");
    cx.clang_disposeString(s);
    return r;
}

/* Major version from an upstream version string such as
 * "Ubuntu clang version 18.1.3 (1ubuntu1)"; 0 when there is none, -1 for
 * Apple's libclang, which uses its own numbering. */
static int niu_clang_major(const char *version)
{
    const char *p = strstr(version, "clang version ");
    if (strstr(version, "Apple"))
        return -1;
    if (!p)
        return 0;
    p += strlen("clang version ");
    if (*p < '0' || *p > '9')
        return 0;
    return atoi(p);
}

/* Tries to load one library file as libclang. On failure returns 0 and puts
 * the reason into why. */
static int niu_try_libclang(const char *path, niu_buf *why)
{
    niu_libclang l;
    size_t i;
    memset(&l, 0, sizeof l);
    if (!niu_is_file(path)) {
        niu_buf_puts(why, niu_exists(path) ? "not a file" : "the file does not exist");
        return 0;
    }
    l.handle = niu_lib_open(path, why);
    if (!l.handle)
        return 0;
#define NIU_X(ret, name, params)                                                                                       \
    {                                                                                                                  \
        niu_fnptr fn = niu_lib_symbol(l.handle, #name);                                                                \
        if (!fn) {                                                                                                     \
            niu_buf_puts(why, "it has no function " #name "; libclang 18 or newer is required");                      \
            niu_lib_close(l.handle);                                                                                   \
            return 0;                                                                                                  \
        }                                                                                                              \
        memcpy(&l.name, &fn, sizeof fn);                                                                               \
    }
    NIU_CLANG_FUNCTIONS(NIU_X)
#undef NIU_X
    for (i = 0; i < sizeof niu_clang_markers / sizeof *niu_clang_markers; i++) {
        if (!niu_lib_symbol(l.handle, niu_clang_markers[i])) {
            niu_buf_printf(why, "it has no function %s, so it is older than libclang 17; libclang 18 or newer is required",
                           niu_clang_markers[i]);
            niu_lib_close(l.handle);
            return 0;
        }
    }
    {
        CXString s = l.clang_getClangVersion();
        const char *c = l.clang_getCString(s);
        int major;
        l.version = niu_strdup(c ? c : "");
        l.clang_disposeString(s);
        major = niu_clang_major(l.version);
        if (major > 0 && major < NIU_LIBCLANG_MIN_MAJOR) {
            niu_buf_printf(why, "it is libclang %d (\"%s\"); libclang 18 or newer is required", major, l.version);
            free(l.version);
            niu_lib_close(l.handle);
            return 0;
        }
    }
    l.path = niu_strdup(path);
    cx = l;
    return 1;
}

/* ---- discovery ---- */

#define NIU_LIBCLANG_MAX_GUESS 40 /* highest LLVM version tried by name */

/* File names libclang can have in a directory, most likely first. */
static void niu_libclang_names(niu_strlist *names)
{
#if defined(_WIN32)
    niu_strlist_push(names, "libclang.dll");
#elif defined(__APPLE__)
    niu_strlist_push(names, "libclang.dylib");
#else
    int v;
    niu_strlist_push(names, "libclang.so");
    for (v = NIU_LIBCLANG_MAX_GUESS; v >= NIU_LIBCLANG_MIN_MAJOR; v--) {
        niu_strlist_push_owned(names, niu_format("libclang.so.%d", v));
        niu_strlist_push_owned(names, niu_format("libclang.so.%d.1", v));
        niu_strlist_push_owned(names, niu_format("libclang-%d.so", v));
        niu_strlist_push_owned(names, niu_format("libclang-%d.so.1", v));
        niu_strlist_push_owned(names, niu_format("libclang-%d.so.%d", v, v));
    }
    niu_strlist_push(names, "libclang.so.1");
#endif
}

typedef struct {
    niu_strlist names;    /* library file names to look for in a directory */
    niu_strlist tried;    /* candidates already tried */
    niu_strlist rejected; /* "path: reason" for each candidate that existed */
    int verbose;
} niu_search;

/* Tries one library file. Missing files are skipped quietly. */
static int niu_search_file(niu_search *s, const char *path)
{
    niu_buf why = {0};
    if (niu_strlist_contains(&s->tried, path))
        return 0;
    niu_strlist_push(&s->tried, path);
    if (!niu_exists(path))
        return 0;
    if (niu_try_libclang(path, &why)) {
        niu_buf_free(&why);
        return 1;
    }
    niu_strlist_push_owned(&s->rejected, niu_format("%s: %s", path, niu_buf_str(&why)));
    niu_buf_free(&why);
    return 0;
}

/* Tries every libclang name in a directory. */
static int niu_search_dir(niu_search *s, const char *dir)
{
    size_t i;
    if (s->verbose)
        niu_message("note", "looking for libclang in %s", dir);
    if (!niu_is_dir(dir))
        return 0;
    for (i = 0; i < s->names.count; i++) {
        char *p = niu_path_join(dir, s->names.items[i]);
        int ok = niu_search_file(s, p);
        free(p);
        if (ok)
            return 1;
    }
    return 0;
}

/* First line of a command's output, or NULL when it failed. */
static char *niu_command_line_output(const char *program, const char *arg)
{
    niu_buf out = {0};
    const char *args[3];
    char *r;
    size_t n;
    args[0] = program;
    args[1] = arg;
    args[2] = NULL;
    if (niu_run_capture(NULL, args, &out) != 0) {
        niu_buf_free(&out);
        return NULL;
    }
    r = niu_buf_take(&out);
    n = strcspn(r, "\r\n");
    r[n] = '\0';
    if (n == 0) {
        free(r);
        return NULL;
    }
    return r;
}

/* 2. `llvm-config --libdir` (and --bindir, where Windows keeps the DLL). */
static int niu_search_llvm_config(niu_search *s)
{
    char *prog = niu_find_program("llvm-config");
    const char *flags[2];
    size_t i;
    int found = 0;
    if (!prog)
        return 0;
    flags[0] = "--libdir";
    flags[1] = "--bindir";
    for (i = 0; i < 2 && !found; i++) {
        char *dir = niu_command_line_output(prog, flags[i]);
        if (dir) {
            found = niu_search_dir(s, dir);
            free(dir);
        }
    }
    free(prog);
    return found;
}

/* 3. The lib directory next to the clang on PATH (bin on Windows), both for
 * the path as found and with symbolic links resolved. */
static int niu_search_clang_on_path(niu_search *s)
{
    char *clang = niu_find_program("clang");
    char *paths[2];
    int i, found = 0;
    if (!clang)
        return 0;
    paths[0] = clang;
    paths[1] = niu_real_path(clang);
    for (i = 0; i < 2 && !found; i++) {
        char *bin, *dir;
        if (!paths[i])
            continue;
        bin = niu_dirname(paths[i]);
#ifdef _WIN32
        dir = niu_strdup(bin);
#else
        {
            char *up = niu_dirname(bin);
            dir = niu_path_join(up, "lib");
            free(up);
        }
#endif
        found = niu_search_dir(s, dir);
        free(dir);
        free(bin);
    }
    free(paths[0]);
    free(paths[1]);
    return found;
}

/* 4. Known install locations. */
static int niu_search_known_paths(niu_search *s)
{
#if defined(_WIN32)
    const char *vars[3];
    size_t i;
    vars[0] = "ProgramFiles";
    vars[1] = "ProgramW6432";
    vars[2] = "ProgramFiles(x86)";
    for (i = 0; i < 3; i++) {
        const char *pf = getenv(vars[i]);
        if (pf && *pf) {
            char *dir = niu_format("%s\\LLVM\\bin", pf);
            int ok = niu_search_dir(s, dir);
            free(dir);
            if (ok)
                return 1;
        }
    }
    return niu_search_dir(s, "C:\\Program Files\\LLVM\\bin");
#elif defined(__APPLE__)
    static const char *const toolchain = "/Toolchains/XcodeDefault.xctoolchain/usr/lib";
    const char *dev = getenv("DEVELOPER_DIR");
    char *sel;
    int v;
    if (dev && *dev) {
        char *dir = niu_format("%s%s", dev, toolchain);
        int ok = niu_search_dir(s, dir);
        free(dir);
        if (ok)
            return 1;
    }
    /* The developer directory chosen with xcode-select: an Xcode.app or
     * the Command Line Tools. */
    sel = niu_command_line_output("xcode-select", "-p");
    if (sel) {
        char *dir = niu_ends_with(sel, "CommandLineTools") ? niu_format("%s/usr/lib", sel)
                                                            : niu_format("%s%s", sel, toolchain);
        int ok = niu_search_dir(s, dir);
        free(dir);
        free(sel);
        if (ok)
            return 1;
    }
    if (niu_search_dir(s, "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/lib") ||
        niu_search_dir(s, "/Library/Developer/CommandLineTools/usr/lib") ||
        niu_search_dir(s, "/opt/homebrew/opt/llvm/lib") || niu_search_dir(s, "/usr/local/opt/llvm/lib"))
        return 1;
    for (v = NIU_LIBCLANG_MAX_GUESS; v >= NIU_LIBCLANG_MIN_MAJOR; v--) {
        char *a = niu_format("/opt/homebrew/opt/llvm@%d/lib", v);
        char *b = niu_format("/usr/local/opt/llvm@%d/lib", v);
        int ok = niu_search_dir(s, a) || niu_search_dir(s, b);
        free(a);
        free(b);
        if (ok)
            return 1;
    }
    return 0;
#else
    static const char *const dirs[] = {
        "/usr/lib/x86_64-linux-gnu", "/usr/lib/aarch64-linux-gnu", "/usr/lib64", "/usr/lib", "/usr/local/lib",
    };
    const char *home = getenv("HOME");
    const char *user = getenv("USER");
    size_t i;
    int v;
    /* NixOS: the system profile, then the user's profiles. */
    if (niu_search_dir(s, "/run/current-system/sw/lib"))
        return 1;
    if (home && *home) {
        char *dir = niu_format("%s/.nix-profile/lib", home);
        int ok = niu_search_dir(s, dir);
        free(dir);
        if (ok)
            return 1;
    }
    if (user && *user) {
        char *dir = niu_format("/etc/profiles/per-user/%s/lib", user);
        int ok = niu_search_dir(s, dir);
        free(dir);
        if (ok)
            return 1;
    }
    /* Debian/Ubuntu (llvm-NN) and Fedora (llvmNN) side-by-side versions. */
    for (v = NIU_LIBCLANG_MAX_GUESS; v >= NIU_LIBCLANG_MIN_MAJOR; v--) {
        char *a = niu_format("/usr/lib/llvm-%d/lib", v);
        char *b = niu_format("/usr/lib64/llvm%d/lib", v);
        int ok = (niu_is_dir(a) && niu_search_dir(s, a)) || (niu_is_dir(b) && niu_search_dir(s, b));
        free(a);
        free(b);
        if (ok)
            return 1;
    }
    for (i = 0; i < sizeof dirs / sizeof *dirs; i++)
        if (niu_search_dir(s, dirs[i]))
            return 1;
    return 0;
#endif
}

static void niu_libclang_install_note(void)
{
    niu_message("note", "install libclang 18 or newer (it comes with LLVM/Clang, for example the libclang1-18 package "
                        "on Debian/Ubuntu, the Xcode Command Line Tools on macOS, or the LLVM installer on Windows), "
                        "or set NIU_LIBCLANG to the path of the libclang library file");
}

/* Finds and loads libclang, in this order: $NIU_LIBCLANG, `llvm-config
 * --libdir`, the lib directory next to clang on PATH, known paths.
 * Prints an error and returns 0 when none works. */
static int niu_load_libclang(int verbose)
{
    niu_search s;
    const char *env = getenv("NIU_LIBCLANG");
    size_t i;
    int found;
    memset(&s, 0, sizeof s);
    s.verbose = verbose;
    niu_libclang_names(&s.names);

    if (env && *env) {
        /* An explicit choice is final: niu does not fall back to searching,
         * so a wrong NIU_LIBCLANG is never hidden by another libclang. */
        niu_buf why = {0};
        found = niu_is_dir(env) ? niu_search_dir(&s, env) : niu_try_libclang(env, &why);
        if (!found) {
            if (niu_is_dir(env))
                niu_message("error", "NIU_LIBCLANG is set to the directory '%s', but it contains no usable libclang",
                            env);
            else
                niu_message("error", "cannot use NIU_LIBCLANG=%s: %s", env, niu_buf_str(&why));
            for (i = 0; i < s.rejected.count; i++)
                niu_message("note", "rejected %s", s.rejected.items[i]);
            niu_message("note", "set NIU_LIBCLANG to the path of libclang 18 or newer (the libclang.so, "
                                "libclang.dylib or libclang.dll file), or unset it to let niu search for libclang");
        }
        niu_buf_free(&why);
    } else {
        found = niu_search_llvm_config(&s) || niu_search_clang_on_path(&s) || niu_search_known_paths(&s);
        if (!found) {
            niu_message("error", "libclang 18 or newer was not found");
            for (i = 0; i < s.rejected.count; i++)
                niu_message("note", "rejected %s", s.rejected.items[i]);
            niu_libclang_install_note();
        }
    }
    if (found && verbose)
        niu_message("note", "using libclang %s (%s)", cx.path, cx.version);
    niu_strlist_free(&s.names);
    niu_strlist_free(&s.tried);
    niu_strlist_free(&s.rejected);
    return found;
}
