/* cli.c - the command line: niu transpile, niu check, and the commands that
 * are not implemented yet. Part of niu.c; not compiled on its own. */

static void niu_usage(FILE *f)
{
    fputs("usage: niu <command> [options] <file>\n"
          "\n"
          "commands:\n"
          "  transpile   write the lowered C next to <file> (x.niu.c -> x.c, x.c -> x.plain.c)\n"
          "  check       parse <file> and report diagnostics; writes nothing\n"
          "  build       not implemented yet\n"
          "\n"
          "options:\n"
          "  -I<dir> -D<name>[=<value>] -U<name> -std=<standard>\n"
          "                      passed on to libclang\n"
          "  --target=<triple>   parse for this target (default on Windows: MSVC)\n"
          "  --no-line           leave out the #line directive in the output\n"
          "  --no-query-cc       do not ask $CC (or cc; gcc for a mingw niu) for its system include directories\n"
          "  -Wno-niu-file-name  no warning for a .niu.c/.niu.h file without <niu.h>\n"
          "  -Wno-niu-plain-file no warning when transpiling a file without <niu.h>\n"
          "  -v                  show which libclang is used and how it is called\n"
          "  --version           show the versions of niu and libclang\n"
          "\n"
          "environment:\n"
          "  NIU_LIBCLANG        path of the libclang library file (or its directory)\n"
          "  CC                  the C compiler niu asks for system include directories\n",
          f);
}

static int niu_usage_error(const char *fmt, const char *arg)
{
    fputs("niu: error: ", stderr);
    fprintf(stderr, fmt, arg);
    fputs("\nniu: note: run 'niu --help' for usage\n", stderr);
    return NIU_EXIT_USAGE;
}

/* Parses the options of transpile/check. Returns -1 on success, else the
 * exit code. */
static int niu_parse_options(int argc, char **argv, niu_options *o)
{
    int i, only_files = 0;
    memset(o, 0, sizeof *o);
    o->warn_file_name = 1;
    o->warn_plain_file = 1;
    for (i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!only_files && a[0] == '-' && a[1] != '\0') {
            if (strcmp(a, "--") == 0) {
                only_files = 1;
            } else if (strcmp(a, "-I") == 0 || strcmp(a, "-D") == 0 || strcmp(a, "-U") == 0) {
                if (i + 1 >= argc)
                    return niu_usage_error("option '%s' needs a value", a);
                niu_strlist_push_owned(&o->user_args, niu_format("%s%s", a, argv[++i]));
            } else if (niu_starts_with(a, "-I") || niu_starts_with(a, "-D") || niu_starts_with(a, "-U") ||
                       niu_starts_with(a, "-std=")) {
                niu_strlist_push(&o->user_args, a);
            } else if (niu_starts_with(a, "--target=")) {
                o->target = a + strlen("--target=");
            } else if (strcmp(a, "--no-line") == 0) {
                o->no_line = 1;
            } else if (strcmp(a, "--no-query-cc") == 0) {
                o->no_query_cc = 1;
            } else if (strcmp(a, "-Wno-niu-file-name") == 0) {
                o->warn_file_name = 0;
            } else if (strcmp(a, "-Wniu-file-name") == 0) {
                o->warn_file_name = 1;
            } else if (strcmp(a, "-Wno-niu-plain-file") == 0) {
                o->warn_plain_file = 0;
            } else if (strcmp(a, "-Wniu-plain-file") == 0) {
                o->warn_plain_file = 1;
            } else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
                o->verbose = 1;
            } else {
                return niu_usage_error("unknown option '%s'", a);
            }
        } else if (o->input) {
            return niu_usage_error("more than one input file ('%s'); niu handles one file per run for now", a);
        } else {
            o->input = a;
        }
    }
    if (!o->input)
        return niu_usage_error("%s", "no input file");
    return -1;
}

/* niu transpile / niu check. */
static int niu_run(int argc, char **argv, int write_output)
{
    niu_options o;
    niu_buf src = {0};
    niu_unit u;
    int rc = niu_parse_options(argc, argv, &o);
    if (rc >= 0) {
        niu_strlist_free(&o.user_args);
        return rc;
    }
    rc = NIU_EXIT_ERROR;
    if (!niu_read_file(o.input, &src)) {
        niu_message("error", "cannot read '%s': %s", o.input, strerror(errno));
        goto done_options;
    }
    if (!niu_load_libclang(o.verbose))
        goto done_src;
    if (niu_process(&o, src.data, src.len, &u)) {
        if (!write_output) {
            rc = NIU_EXIT_OK;
        } else {
            niu_buf out = {0};
            char *name = niu_output_name(o.input);
            /* A plain file has nothing to lower, so its copy is the input
             * unchanged. A .niu.c/.niu.h name already got -Wniu-file-name. */
            if (!u.is_niu_file && !niu_has_niu_name(o.input) && o.warn_plain_file)
                niu_message_at(o.input, "warning",
                               "'%s' is not a niu file (it does not include <niu.h>), so '%s' is an unchanged "
                               "copy [-Wniu-plain-file]",
                               niu_basename(o.input), niu_basename(name));
            if (!niu_render_output(o.input, src.data, src.len, &u.edits, o.no_line, &out))
                niu_message_at(o.input, "error", "internal error: overlapping edits");
            else if (niu_write_output(o.input, name, &out))
                rc = NIU_EXIT_OK;
            niu_buf_free(&out);
            free(name);
        }
    }
    niu_unit_free(&u);
done_src:
    niu_buf_free(&src);
done_options:
    niu_strlist_free(&o.user_args);
    return rc;
}

static int niu_version(void)
{
    printf("niu %s\n", NIU_VERSION);
    if (!niu_load_libclang(0))
        return NIU_EXIT_ERROR;
    printf("libclang %s (%s)\n", cx.path, cx.version);
    return NIU_EXIT_OK;
}

static int niu_main(int argc, char **argv)
{
    const char *self = argc > 0 ? niu_basename(argv[0]) : "niu";
    const char *cmd;
    if (strcmp(self, "niucc") == 0 || strcmp(self, "niucc.exe") == 0) {
        niu_message("error", "niucc is not implemented yet");
        return NIU_EXIT_ERROR;
    }
    if (argc < 2) {
        niu_usage(stderr);
        return NIU_EXIT_USAGE;
    }
    cmd = argv[1];
    if (strcmp(cmd, "transpile") == 0)
        return niu_run(argc - 2, argv + 2, 1);
    if (strcmp(cmd, "check") == 0)
        return niu_run(argc - 2, argv + 2, 0);
    if (strcmp(cmd, "build") == 0 || strcmp(cmd, "niucc") == 0) {
        niu_message("error", "'niu %s' is not implemented yet", cmd);
        return NIU_EXIT_ERROR;
    }
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "version") == 0)
        return niu_version();
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "help") == 0) {
        niu_usage(stdout);
        return NIU_EXIT_OK;
    }
    return niu_usage_error("unknown command '%s'", cmd);
}
