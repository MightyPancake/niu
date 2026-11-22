/* platform_specific.c - everything that differs between Windows and POSIX:
 * library loading, files, paths, processes. The rest of niu only calls the
 * niu_ helpers defined here. Part of niu.c; not compiled on its own. */

#ifdef _WIN32
#define NIU_PATH_SEP '\\'
#define NIU_PATH_LIST_SEP ';'
#define NIU_EXE_SUFFIX ".exe"
#else
#define NIU_PATH_SEP '/'
#define NIU_PATH_LIST_SEP ':'
#define NIU_EXE_SUFFIX ""
#endif

/* A generic function pointer, cast to the real type after lookup. */
typedef void (*niu_fnptr)(void);

static int niu_is_sep(char c)
{
#ifdef _WIN32
    return c == '\\' || c == '/';
#else
    return c == '/';
#endif
}

/* ---- shared libraries ---- */

/* Loads a shared library by path. On failure returns NULL and puts the
 * reason into why. */
static void *niu_lib_open(const char *path, niu_buf *why)
{
#ifdef _WIN32
    /* The altered search path makes the DLL's own directory the place where
     * its dependencies are looked up, as for an installed LLVM. */
    HMODULE h = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) {
        char msg[512];
        DWORD err = GetLastError();
        DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0, msg,
                                 (DWORD)sizeof msg, NULL);
        while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == '\r' || msg[n - 1] == ' '))
            msg[--n] = '\0';
        if (n == 0)
            niu_buf_printf(why, "LoadLibrary failed with error %lu", (unsigned long)err);
        else
            niu_buf_puts(why, msg);
        return NULL;
    }
    return (void *)h;
#else
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char *e = dlerror();
        niu_buf_puts(why, e ? e : "dlopen failed");
    }
    return h;
#endif
}

static niu_fnptr niu_lib_symbol(void *lib, const char *name)
{
    niu_fnptr fn = NULL;
#ifdef _WIN32
    FARPROC p = GetProcAddress((HMODULE)lib, name);
    fn = (niu_fnptr)p;
#else
    /* ISO C has no conversion from object to function pointers; copying
     * the bits is what POSIX guarantees to work for dlsym. */
    void *p = dlsym(lib, name);
    memcpy(&fn, &p, sizeof fn);
#endif
    return fn;
}

static void niu_lib_close(void *lib)
{
#ifdef _WIN32
    FreeLibrary((HMODULE)lib);
#else
    dlclose(lib);
#endif
}

/* ---- files and directories ---- */

static int niu_is_file(const char *path)
{
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

static int niu_is_dir(const char *path)
{
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

/* Something exists at path (file, directory or anything else). */
static int niu_exists(const char *path)
{
#ifdef _WIN32
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

/* Replaces dst with src, atomically where the platform allows it. */
static int niu_replace_file(const char *src, const char *dst)
{
#ifdef _WIN32
    return MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(src, dst) == 0;
#endif
}

/* Absolute path with symbolic links resolved (where the platform has them),
 * or NULL. The caller frees it. */
static char *niu_real_path(const char *path)
{
#ifdef _WIN32
    char *r = _fullpath(NULL, path, 0);
#else
    char *r = realpath(path, NULL);
#endif
    char *copy;
    if (!r)
        return NULL;
    copy = niu_strdup(r);
    free(r);
    return copy;
}

static const char *niu_null_device(void)
{
#ifdef _WIN32
    return "NUL";
#else
    return "/dev/null";
#endif
}

/* ---- paths ---- */

static char *niu_path_join(const char *dir, const char *name)
{
    size_t n = strlen(dir);
    if (n == 0)
        return niu_strdup(name);
    if (niu_is_sep(dir[n - 1]))
        return niu_format("%s%s", dir, name);
    return niu_format("%s%c%s", dir, NIU_PATH_SEP, name);
}

/* Directory part of path ("." when there is none). */
static char *niu_dirname(const char *path)
{
    size_t n = strlen(path);
    while (n > 0 && !niu_is_sep(path[n - 1]))
        n--;
    if (n == 0)
        return niu_strdup(".");
    while (n > 1 && niu_is_sep(path[n - 1]))
        n--;
    return niu_strndup(path, n);
}

/* File name part of path; points into path. */
static const char *niu_basename(const char *path)
{
    const char *b = path, *p;
    for (p = path; *p; p++)
        if (niu_is_sep(*p))
            b = p + 1;
#ifdef _WIN32
    if (b == path && path[0] && path[1] == ':')
        b = path + 2;
#endif
    return b;
}

/* Searches PATH for a program; returns its path or NULL. */
static char *niu_find_program(const char *name)
{
    const char *path = getenv("PATH");
    const char *p, *end;
    if (!path || !*path)
        return NULL;
    for (p = path;; p = end + 1) {
        char *dir, *cand, *file;
        end = strchr(p, NIU_PATH_LIST_SEP);
        if (!end)
            end = p + strlen(p);
        dir = niu_strndup(p, (size_t)(end - p));
        file = niu_format("%s%s", name, NIU_EXE_SUFFIX);
        cand = niu_path_join(*dir ? dir : ".", file);
        free(dir);
        free(file);
#ifdef _WIN32
        if (niu_is_file(cand))
            return cand;
#else
        if (niu_is_file(cand) && access(cand, X_OK) == 0)
            return cand;
#endif
        free(cand);
        if (!*end)
            break;
    }
    return NULL;
}

/* ---- processes ---- */

static void niu_quote_arg(niu_buf *cmd, const char *arg)
{
#ifdef _WIN32
    niu_buf_putc(cmd, '"');
    for (; *arg; arg++) {
        if (*arg == '"')
            niu_buf_putc(cmd, '\\');
        niu_buf_putc(cmd, *arg);
    }
    niu_buf_putc(cmd, '"');
#else
    niu_buf_putc(cmd, '\'');
    for (; *arg; arg++) {
        if (*arg == '\'')
            niu_buf_puts(cmd, "'\\''");
        else
            niu_buf_putc(cmd, *arg);
    }
    niu_buf_putc(cmd, '\'');
#endif
}

/* Runs a command and captures its stdout and stderr into out.
 * raw is put into the command line unquoted, so it can hold a program with
 * arguments, as $CC often does (may be NULL); every entry of args (ending
 * with NULL) is quoted as one argument. Returns the exit status, or -1 when
 * the command could not be started. */
static int niu_run_capture(const char *raw, const char *const *args, niu_buf *out)
{
    niu_buf cmd = {0};
    FILE *p;
    char chunk[4096];
    size_t n;
    int status;
#ifdef _WIN32
    /* cmd.exe drops the first and the last quote of the line, so the whole
     * command is put in one more pair of quotes. */
    niu_buf_putc(&cmd, '"');
#endif
    if (raw)
        niu_buf_puts(&cmd, raw);
    for (; *args; args++) {
        if (cmd.len > 1 || (cmd.len == 1 && cmd.data[0] != '"'))
            niu_buf_putc(&cmd, ' ');
        niu_quote_arg(&cmd, *args);
    }
    niu_buf_puts(&cmd, " 2>&1");
#ifdef _WIN32
    niu_buf_putc(&cmd, '"');
#endif
    fflush(NULL);
#ifdef _WIN32
    p = _popen(niu_buf_str(&cmd), "rb");
#else
    p = popen(niu_buf_str(&cmd), "r");
#endif
    niu_buf_free(&cmd);
    if (!p)
        return -1;
    while ((n = fread(chunk, 1, sizeof chunk, p)) > 0)
        niu_buf_append(out, chunk, n);
    niu_buf_str(out);
#ifdef _WIN32
    status = _pclose(p);
#else
    status = pclose(p);
#endif
    return status;
}

/* ---- the virtual directory that holds the embedded niu.h ---- */

/* An absolute directory that does not exist on disk. libclang sees the
 * embedded niu.h as an unsaved file in <root>/include. */
static char *niu_virtual_root(void)
{
#ifdef _WIN32
    char cwd[MAX_PATH + 1];
    DWORD n = GetCurrentDirectoryA((DWORD)sizeof cwd, cwd);
    char drive = 'C';
    if (n >= 2 && n < sizeof cwd && cwd[1] == ':')
        drive = cwd[0];
    return niu_format("%c:\\__niu__", drive);
#else
    return niu_strdup("/__niu__");
#endif
}
