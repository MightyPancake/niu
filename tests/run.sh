#!/bin/sh
# tests/run.sh - niu's test runner. Plain POSIX sh; CI runs it with bash on
# Linux, macOS and Windows (Git Bash).
#
#   bash tests/run.sh [path/to/niu]
#
# It transpiles every file in tests/samples, compares the output with
# tests/expected, and compiles and runs the output with every C compiler in
# every output standard it supports. Then it checks niu's error behaviour.
#
# Environment:
#   NIU_TEST_CC   the C compilers to compile the outputs with, separated by
#                 spaces. A compiler listed here that is missing fails the
#                 run. Default: those of gcc, clang, tcc that are installed
#                 (cl, clang and gcc on Windows).
#   NIU_LIBCLANG  passed on to niu (see README.md).

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)

case $(uname -s) in
MINGW* | MSYS* | CYGWIN*) windows=1 exe=.exe ;;
*) windows=0 exe= ;;
esac

niu=${1:-$root/niu$exe}
case $niu in
/* | [A-Za-z]:*) ;;
*) niu=$(pwd)/$niu ;;
esac
if [ ! -x "$niu" ]; then
    echo "run.sh: niu not found at $niu (build it with: cc niu.c -o niu)" >&2
    exit 2
fi

work=$(mktemp -d 2>/dev/null || mktemp -d -t niu)
trap 'rm -rf "$work"' EXIT

passes=0
fails=0
skips=0

pass() {
    passes=$((passes + 1))
    printf 'ok    %s\n' "$1"
}

# fail <test name> [details file]
fail() {
    fails=$((fails + 1))
    printf 'FAIL  %s\n' "$1"
    if [ $# -gt 1 ] && [ -s "$2" ]; then
        sed 's/^/      | /' "$2"
    fi
}

skip() {
    skips=$((skips + 1))
    printf 'skip  %s\n' "$1"
}

# A fresh directory with copies of the given files from tests/samples.
fresh_dir() {
    d=$work/$1
    shift
    rm -rf "$d"
    mkdir -p "$d"
    for f in "$@"; do
        cp "$here/samples/$f" "$d/"
    done
    echo "$d"
}

# Name of the lowered copy (spec.md "Output").
output_name() {
    case $1 in
    *.niu.c) echo "${1%.niu.c}.c" ;;
    *.c) echo "${1%.c}.plain.c" ;;
    esac
}

echo "niu: $niu"
"$niu" --version || {
    echo "run.sh: niu cannot load libclang; see the message above" >&2
    exit 1
}

# ---------------------------------------------------------------------------
echo
echo "== round trip"

samples=$(cd "$here/samples" && ls -- *.c)

for name in $samples; do
    out=$(output_name "$name")
    d=$(fresh_dir "rt-$name" "$name")

    # Default output: generated comment, #line, then the edited original.
    # (The plain samples' -Wniu-plain-file is tested separately below.)
    if (cd "$d" && "$niu" transpile -Wno-niu-plain-file "$name") >"$d/log" 2>&1; then
        if [ -s "$d/log" ]; then
            fail "$name: transpile printed diagnostics" "$d/log"
        elif cmp "$d/$out" "$here/expected/$out" >"$d/cmp" 2>&1; then
            pass "$name -> $out"
        else
            diff "$here/expected/$out" "$d/$out" >>"$d/cmp" 2>&1
            fail "$name -> $out differs from tests/expected/$out" "$d/cmp"
        fi
    else
        fail "$name: transpile failed" "$d/log"
        continue
    fi

    # --no-line: the same without any #line directive (line 2, and those
    # around copies of deferred statements). This also replaces the file
    # written above, which niu may do because it generated it. Line endings
    # are compared above; here CRs are dropped on both sides, because Git
    # Bash's text tools drop them unpredictably.
    tr -d '\r' <"$here/expected/$out" | grep -v '^#line ' >"$d/expected-no-line"
    if (cd "$d" && "$niu" transpile --no-line -Wno-niu-plain-file "$name") >"$d/log" 2>&1 &&
        tr -d '\r' <"$d/$out" >"$d/actual-no-line" &&
        cmp "$d/actual-no-line" "$d/expected-no-line" >"$d/cmp" 2>&1; then
        pass "$name -> $out with --no-line (replacing niu's own file)"
    else
        cat "$d/cmp" >>"$d/log"
        fail "$name: --no-line output" "$d/log"
    fi

    # A plain file comes out byte for byte identical after the header.
    case $name in
    plain*)
        (cd "$d" && "$niu" transpile "$name") >/dev/null 2>&1
        if tail -n +3 "$d/$out" | cmp - "$d/$name" >"$d/cmp" 2>&1; then
            pass "$name: output is the input byte for byte after the header"
        else
            fail "$name: output body differs from the input" "$d/cmp"
        fi
        ;;
    esac
done

# ---------------------------------------------------------------------------
echo
echo "== compiling the output"

if [ -n "${NIU_TEST_CC-}" ]; then
    compilers=$NIU_TEST_CC
    required=1
else
    required=0
    compilers=
    if [ $windows = 1 ]; then
        candidates="cl clang gcc"
    else
        candidates="gcc clang tcc"
    fi
    for c in $candidates; do
        if command -v "$c" >/dev/null 2>&1; then
            compilers="$compilers $c"
        fi
    done
fi

# compiler_kind <compiler>: msvc, tcc or gnu (gcc and clang style options)
compiler_kind() {
    b=$(basename "$1")
    b=${b%.exe}
    case $b in
    cl | clang-cl) echo msvc ;;
    *tcc*) echo tcc ;;
    *) echo gnu ;;
    esac
}

# compile <compiler> <kind> <std flag> <source> <exe> <log> [extra flags...]
# With gnu_mode=1, gcc and clang compile GNU C (-std=gnu11 for c11, no
# -pedantic-errors), for the gnu_* samples.
gnu_mode=0
compile() {
    c_cc=$1 c_kind=$2 c_std=$3 c_src=$4 c_exe=$5 c_log=$6
    shift 6
    case $c_kind in
    msvc) (cd "$(dirname "$c_src")" && "$c_cc" -nologo -W4 -WX "-std:$c_std" "$@" "-Fe$(basename "$c_exe")" "$(basename "$c_src")") >"$c_log" 2>&1 ;;
    tcc) "$c_cc" "-std=$c_std" -Wall -Werror "$@" -o "$c_exe" "$c_src" >"$c_log" 2>&1 ;;
    *)
        if [ $gnu_mode = 1 ]; then
            "$c_cc" "-std=gnu${c_std#c}" -Wall -Wextra -Werror "$@" -o "$c_exe" "$c_src" >"$c_log" 2>&1
        else
            "$c_cc" "-std=$c_std" -Wall -Wextra -Werror -pedantic-errors "$@" -o "$c_exe" "$c_src" >"$c_log" 2>&1
        fi
        ;;
    esac
}

# line_file_name <kind> <source> <#line name>: what __FILE__ and error
# messages say after `#line 1 "<#line name>"` in <source>. gcc, clang and cl
# use the name as written. tcc puts the directory of the source in front.
line_file_name() {
    case $1 in
    tcc) echo "$(dirname "$2")/$3" ;;
    *) echo "$3" ;;
    esac
}

# location <kind> <file> <line>: how the compiler writes an error location.
location() {
    case $1 in
    msvc) echo "$2($3)" ;;
    *) echo "$2:$3:" ;;
    esac
}

# probe <compiler> <kind> <std flag> <min __STDC_VERSION__> <max>: does the
# compiler accept the flag and really use that standard?
probe() {
    pd=$work/probe
    mkdir -p "$pd"
    cat >"$pd/probe.c" <<EOF
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < $4 || __STDC_VERSION__ > $5
#error "not this standard"
#endif
int main(void) { return 0; }
EOF
    compile "$1" "$2" "$3" "$pd/probe.c" "$pd/probe$exe" "$pd/log"
}

if [ -z "$compilers" ]; then
    echo "run.sh: no C compiler found to compile the outputs with" >&2
    fails=$((fails + 1))
fi

for cc in $compilers; do
    if ! command -v "$cc" >/dev/null 2>&1; then
        if [ $required = 1 ]; then
            fail "$cc: compiler not found (listed in NIU_TEST_CC)"
        fi
        continue
    fi
    kind=$(compiler_kind "$cc")
    # Output standards: label, niu's -std, the compiler's flag(s) to try,
    # the __STDC_VERSION__ range, and whether a missing mode is fatal.
    if [ "$kind" = msvc ]; then
        modes="c11:c11:c11:201112L:201112L:1 c17:c17:c17:201710L:201710L:1 latest:c23:clatest:201710L:999999L:0"
    else
        modes="c99:c99:c99:199901L:199901L:1 c11:c11:c11:201112L:201112L:1 c23:c23:c23,c2x:201711L:999999L:0"
    fi
    for mode in $modes; do
        label=$(echo "$mode" | cut -d: -f1)
        nstd=$(echo "$mode" | cut -d: -f2)
        flags=$(echo "$mode" | cut -d: -f3 | tr , ' ')
        vmin=$(echo "$mode" | cut -d: -f4)
        vmax=$(echo "$mode" | cut -d: -f5)
        must=$(echo "$mode" | cut -d: -f6)
        flag=
        for f in $flags; do
            if probe "$cc" "$kind" "$f" "$vmin" "$vmax"; then
                flag=$f
                break
            fi
        done
        if [ -z "$flag" ]; then
            if [ "$must" = 1 ]; then
                fail "$cc does not support $label" "$work/probe/log"
            else
                skip "$cc $label: not supported by this compiler"
            fi
            continue
        fi
        for name in $samples; do
            out=$(output_name "$name")
            # gnu_* samples use GNU C (statement expressions), which cl lacks
            gnu_mode=0 nstd_flag=$nstd
            case $name in
            gnu_*)
                [ "$kind" = msvc ] && continue
                gnu_mode=1 nstd_flag=gnu${nstd#c}
                ;;
            esac
            d=$(fresh_dir "cc-$(basename "$cc")-$label-$name" "$name")
            if ! (cd "$d" && "$niu" transpile "-std=$nstd_flag" "$name") >"$d/log" 2>&1; then
                fail "$cc $label $name: niu transpile -std=$nstd_flag failed" "$d/log"
                continue
            fi
            if ! compile "$cc" "$kind" "$flag" "$d/$out" "$d/prog$exe" "$d/log"; then
                fail "$cc -std=$flag $out: does not compile" "$d/log"
                continue
            fi
            # hello.niu.c also prints __FILE__, which shows what the compiler
            # made of `#line 1 "hello.niu.c"`.
            want=ok
            case $name in
            hello.niu.c) want="ok
$(line_file_name "$kind" "$d/$out" hello.niu.c)" ;;
            esac
            if (cd "$d" && ./prog$exe) >"$d/stdout" 2>"$d/log" &&
                [ "$(tr -d '\r' <"$d/stdout")" = "$want" ]; then
                pass "$cc -std=$flag $out"
            else
                { echo "expected output:"; echo "$want"; echo "actual output:"; cat "$d/stdout"; } >>"$d/log"
                fail "$cc -std=$flag $out: the program failed" "$d/log"
            fi
        done
        gnu_mode=0
    done
done

# ---------------------------------------------------------------------------
echo
echo "== #line: where compiler errors point"

# A niu file with an #error that only the compiler of the output hits.
where_c='/* where.c */
#include <niu.h>

#ifdef NIU_TEST_FAIL
#error "line check"
#endif
int main(void) { return 0; }
'

for cc in $compilers; do
    command -v "$cc" >/dev/null 2>&1 || continue
    kind=$(compiler_kind "$cc")
    flag=c11
    d=$(fresh_dir "where-$(basename "$cc")")
    printf '%s' "$where_c" >"$d/where.c"
    # With #line the error is on line 5 of where.c; without it, on line 6
    # of where.plain.c (one line for the generated comment).
    for variant in line no-line; do
        if [ $variant = line ]; then
            nflags='' want=$(location "$kind" "$(line_file_name "$kind" "$d/where.plain.c" where.c)" 5)
        else
            # (gcc, clang and tcc print the path they were given, which
            # Git Bash may have rewritten, so only its end is compared.)
            nflags=--no-line want=$(location "$kind" where.plain.c 6)
        fi
        # shellcheck disable=SC2086
        if ! (cd "$d" && "$niu" transpile $nflags where.c) >"$d/log" 2>&1; then
            fail "$cc where.c ($variant): niu transpile failed" "$d/log"
            continue
        fi
        if compile "$cc" "$kind" "$flag" "$d/where.plain.c" "$d/prog$exe" "$d/log" -DNIU_TEST_FAIL; then
            fail "$cc where.c ($variant): compiled although it has an #error" "$d/log"
        elif tr -d '\r' <"$d/log" | grep -F -- "$want" >/dev/null &&
            { [ $variant = no-line ] || ! grep -E 'where\.plain\.c[:(]' "$d/log" >/dev/null; }; then
            pass "$cc: errors point at $(basename "$want") ($variant)"
        else
            { echo "expected the compiler's messages to name $want (and with #line, never where.plain.c as a location):"; cat "$d/log"; } >"$d/why"
            fail "$cc where.c ($variant): wrong error location" "$d/why"
        fi
    done
done

# ---------------------------------------------------------------------------
echo
echo "== niu.h"

# The built-in niu.h (src/niu_h.c, one C string per line) must be the same
# as include/niu.h, which editor tools use.
d=$(fresh_dir niu-h)
sed -n 's/^    "\(.*\)\\n"[;]*$/\1/p' "$root/src/niu_h.c" | sed 's/\\"/"/g; s/\\\\/\\/g' >"$d/embedded.h"
if cmp "$root/include/niu.h" "$d/embedded.h" >"$d/cmp" 2>&1; then
    pass "src/niu_h.c is the same as include/niu.h"
else
    diff "$root/include/niu.h" "$d/embedded.h" >>"$d/cmp" 2>&1
    fail "src/niu_h.c differs from include/niu.h" "$d/cmp"
fi

# Uses every niu keyword, so it only compiles where niu.h defines stubs.
tooling_c='#include <stdio.h>
#include <stdlib.h>
#include <niu.h>

int main(void)
{
    char *p = (char *)malloc(4);
    defer free(p);
    _Defer { p = NULL; }
    if (!p) {
        defer_now;
        _Defer_now;
        return 1;
    }
    printf("ok\n");
    return 0;
}
'

for cc in $compilers; do
    command -v "$cc" >/dev/null 2>&1 || continue
    kind=$(compiler_kind "$cc")
    d=$(fresh_dir "niu-h-$(basename "$cc")")
    printf '%s' "$tooling_c" >"$d/tool.c"
    inc=$root/include
    if compile "$cc" "$kind" c11 "$d/tool.c" "$d/prog$exe" "$d/log" "-I$inc"; then
        fail "$cc: a niu file compiled without niu" "$d/log"
    elif tr -d '\r' <"$d/log" | grep -F "this file must be compiled with niu" >/dev/null; then
        pass "$cc: niu.h stops a plain compile with its #error"
    else
        fail "$cc: compiling a niu file without niu gave the wrong error" "$d/log"
    fi
    if compile "$cc" "$kind" c11 "$d/tool.c" "$d/prog$exe" "$d/log" "-I$inc" -D__NIU_TOOLING__ &&
        (cd "$d" && ./prog$exe) >"$d/stdout" 2>>"$d/log" &&
        [ "$(tr -d '\r' <"$d/stdout")" = ok ]; then
        pass "$cc: -D__NIU_TOOLING__ compiles niu keywords as stubs"
    else
        fail "$cc: niu keywords do not compile with -D__NIU_TOOLING__" "$d/log"
    fi
done

# ---------------------------------------------------------------------------
echo
echo "== errors and warnings"

# expect <test name> <expected exit code> <expected stderr|-> <cmd...>
# Runs the command in $d and compares exit code and (unless -) stderr.
expect() {
    tname=$1 want_rc=$2 want_err=$3
    shift 3
    (cd "$d" && "$@") >"$d/stdout" 2>"$d/stderr"
    rc=$?
    tr -d '\r' <"$d/stderr" >"$d/stderr.lf"
    if [ "$rc" != "$want_rc" ]; then
        echo "exit code $rc, expected $want_rc" >"$d/why"
        cat "$d/stderr" >>"$d/why"
        fail "$tname" "$d/why"
        return 1
    fi
    if [ "$want_err" != - ] && [ "$(cat "$d/stderr.lf")" != "$want_err" ]; then
        { echo "expected stderr:"; echo "$want_err"; echo "actual stderr:"; cat "$d/stderr.lf"; } >"$d/why"
        fail "$tname" "$d/why"
        return 1
    fi
    pass "$tname"
}

# stderr_has <test name> <text>...: the last command's stderr contains each text
stderr_has() {
    tname=$1
    shift
    for t in "$@"; do
        if ! grep -F -- "$t" "$d/stderr.lf" >/dev/null; then
            { echo "stderr does not contain: $t"; cat "$d/stderr.lf"; } >"$d/why"
            fail "$tname" "$d/why"
            return 1
        fi
    done
    pass "$tname"
}

# The name of the lowered copy is taken by a hand-written file.
d=$(fresh_dir err-exists hello.niu.c)
printf 'int hand_written;\n' >"$d/hello.c"
cp "$d/hello.c" "$d/hello.c.orig"
expect "refuses to replace a file niu did not generate" 1 \
    "hello.niu.c: error: cannot write 'hello.c': the file exists and was not generated by niu" \
    "$niu" transpile hello.niu.c &&
    if cmp "$d/hello.c" "$d/hello.c.orig" >/dev/null 2>&1; then
        pass "the hand-written file is unchanged"
    else
        fail "the hand-written file was modified"
    fi

# libclang cannot be loaded: NIU_LIBCLANG is final, so this does not depend
# on what is installed.
d=$(fresh_dir err-libclang app.c)
expect "NIU_LIBCLANG pointing at a missing file" 1 - \
    env NIU_LIBCLANG="$d/no-such-dir/libclang.so" "$niu" check app.c &&
    stderr_has "  ... names libclang 18 and NIU_LIBCLANG" "libclang 18 or newer" "NIU_LIBCLANG" "the file does not exist"
expect "NIU_LIBCLANG pointing at a file that is not a library" 1 - \
    env NIU_LIBCLANG="$d/app.c" "$niu" check app.c &&
    stderr_has "  ... names libclang 18 and NIU_LIBCLANG" "libclang 18 or newer" "NIU_LIBCLANG"
expect "NIU_LIBCLANG pointing at a directory without libclang" 1 - \
    env NIU_LIBCLANG="$d" "$niu" check app.c &&
    stderr_has "  ... names libclang 18 and NIU_LIBCLANG" "libclang 18 or newer" "NIU_LIBCLANG"

# libclang's diagnostics, with file:line:col and severity.
d=$(fresh_dir err-diag)
printf 'int main(void)\n{\n    return missing;\n}\n' >"$d/bad.c"
expect "errors are reported as file:line:col and fail" 1 \
    "bad.c:3:12: error: use of undeclared identifier 'missing'" \
    "$niu" transpile bad.c &&
    if [ -e "$d/bad.plain.c" ]; then fail "no output is written after an error"; else pass "no output is written after an error"; fi

printf '#warning "just a warning"\nint main(void) { return 0; }\n' >"$d/warn.c"
# (The wording of the message itself varies between clang versions.)
expect "warnings are reported and do not fail" 0 - "$niu" transpile warn.c &&
    stderr_has "  ... as file:line:col: warning: ... [option]" "warn.c:1:2: warning: " "just a warning" "[-W#warnings]" &&
    if [ -f "$d/warn.plain.c" ]; then pass "output is written despite warnings"; else fail "output is written despite warnings"; fi

printf '#include <niu.h>\n#include "missing.h"\nint main(void) { return 0; }\n' >"$d/missing.c"
expect "a missing header is an error" 1 - "$niu" check missing.c &&
    stderr_has "  ... message" "missing.c:2:10: " "error: " "missing.h' file not found"

# -D, -U and -I reach libclang.
mkdir -p "$d/inc"
printf '#define FROM_INC 1\n' >"$d/inc/opt.h"
printf '#include "opt.h"\n#if !defined(FROM_INC) || !defined(FLAG) || FLAG != 2 || defined(__NIU_STD__)\n#error "options were not passed"\n#endif\nint main(void) { return 0; }\n' >"$d/opts.c"
expect "-I, -D and -U are passed to libclang" 0 "" "$niu" check -I inc -DFLAG=2 -U__NIU_STD__ opts.c
expect "-std= is passed to libclang" 1 - "$niu" check -std=c99 -std=not-a-standard opts.c

# A niu header: lowering it is milestone 2.
d=$(fresh_dir err-header)
printf '#include <niu.h>\nint helper(void);\n' >"$d/util.h"
printf '#include "util.h"\nint main(void) { return 0; }\n' >"$d/user.c"
expect "a niu header is reported as not implemented yet" 1 - "$niu" transpile user.c &&
    stderr_has "  ... message" "util.h:1:1: error:" "includes <niu.h>, but lowering niu headers is not implemented yet"

# A .niu.c name without <niu.h>.
d=$(fresh_dir err-name)
printf 'int main(void) { return 0; }\n' >"$d/odd.niu.c"
expect "-Wniu-file-name for a .niu.c file without <niu.h>" 0 \
    "odd.niu.c: warning: 'odd.niu.c' is named like a niu file but does not include <niu.h> [-Wniu-file-name]" \
    "$niu" transpile odd.niu.c
expect "-Wno-niu-file-name turns it off" 0 "" "$niu" transpile -Wno-niu-file-name odd.niu.c

# Transpiling a plain file warns, and still writes the unchanged copy.
d=$(fresh_dir err-plain plain.c)
expect "-Wniu-plain-file for transpiling a file without <niu.h>" 0 \
    "plain.c: warning: 'plain.c' is not a niu file (it does not include <niu.h>), so 'plain.plain.c' is an unchanged copy [-Wniu-plain-file]" \
    "$niu" transpile plain.c &&
    if cmp "$d/plain.plain.c" "$here/expected/plain.plain.c" >/dev/null 2>&1; then
        pass "  ... and plain.plain.c is still written"
    else
        fail "  ... plain.plain.c was not written as expected"
    fi
expect "-Wno-niu-plain-file turns it off" 0 "" "$niu" transpile -Wno-niu-plain-file plain.c
expect "niu check does not give -Wniu-plain-file" 0 "" "$niu" check plain.c

# niu check writes nothing.
d=$(fresh_dir check app.c)
expect "niu check succeeds on a niu file" 0 "" "$niu" check app.c &&
    if [ -e "$d/app.plain.c" ]; then fail "niu check wrote a file"; else pass "niu check writes nothing"; fi

# The command line.
expect "niu build is not implemented yet" 1 "niu: error: 'niu build' is not implemented yet" "$niu" build app.c
expect "unknown options are usage errors" 2 - "$niu" transpile --frobnicate app.c
expect "one input file per run" 2 - "$niu" check app.c app.c
expect "a missing input file is reported" 1 - "$niu" check no-such-file.c &&
    stderr_has "  ... message" "cannot read 'no-such-file.c'"

# ---------------------------------------------------------------------------
echo
echo "== defer errors and warnings"

# defer_case <test name> <expected exit code> <expected stderr> [niu options]:
# runs `niu check` on the file x.c, which the caller has just written to
# $d/x.c from the standard input. Every message here is niu's own, so the
# whole of stderr is compared.
defer_case() {
    dc_name=$1 dc_rc=$2 dc_err=$3
    shift 3
    expect "$dc_name" "$dc_rc" "$dc_err" "$niu" check "$@" x.c
}

# The prelude every case starts with: lines 1 to 3.
prelude='#include <stdlib.h>
#include <stdio.h>
#include <niu.h>'

d=$(fresh_dir defer-errors)

printf '%s\nvoid f(char *p) {\n    if (p) defer free(p);\n}\n' "$prelude" >"$d/x.c"
defer_case "1: a defer must be directly inside braces" 1 \
    "x.c:5:12: error: 'defer' must appear directly inside a block '{ }'"

printf '%s\nvoid f(char *p) {\nl:  defer free(p);\n}\n' "$prelude" >"$d/x.c"
defer_case "1: a defer cannot have a label" 1 \
    "x.c:5:5: error: 'defer' must appear directly inside a block '{ }'"

printf '%s\nint f(int ok) {\n    defer { if (!ok) return 1; }\n    return 0;\n}\n' "$prelude" >"$d/x.c"
defer_case "2: return cannot leave a deferred statement" 1 \
    "x.c:5:22: error: 'return' cannot leave a deferred statement
x.c:5:5: note: the 'defer' is here"

printf '%s\nvoid f(void) {\n    for (;;) { defer { break; } }\n}\n' "$prelude" >"$d/x.c"
defer_case "2: break cannot leave a deferred statement" 1 \
    "x.c:5:24: error: 'break' cannot leave a deferred statement
x.c:5:16: note: the 'defer' is here"

printf '%s\nvoid f(int x) {\n    defer { for (;;) { if (x) break; } }\n}\n' "$prelude" >"$d/x.c"
defer_case "2: a break that stays inside is fine" 0 ""

printf '%s\nvoid f(char *p) {\n    goto skip;\n    defer free(p);\nskip:\n    free(p);\n}\n' "$prelude" >"$d/x.c"
defer_case "3: goto into the scope of a defer" 1 \
    "x.c:5:5: error: 'goto' jumps into the scope of a 'defer'
x.c:6:5: note: the 'defer' is here"

printf '%s\nvoid f(int x) {\n    switch (x) {\n        defer puts("x");\n    case 1:\n        break;\n    }\n}\n' "$prelude" >"$d/x.c"
defer_case "3: case label inside the scope of a defer" 1 \
    "x.c:7:5: error: 'case' label is inside the scope of a 'defer'
x.c:6:9: note: the 'defer' is here"

printf '%s\nvoid f(void) {\n    defer { l: ; }\n}\n' "$prelude" >"$d/x.c"
defer_case "4: no labels in a deferred statement" 1 \
    "x.c:5:13: error: labels are not allowed in a deferred statement
x.c:5:5: note: the 'defer' is here"

printf '%s\nvoid f(void) {\n    defer { static int n; n++; }\n}\n' "$prelude" >"$d/x.c"
defer_case "5: no static variables in a deferred statement" 1 \
    "x.c:5:13: error: 'static' variables are not allowed in a deferred statement
x.c:5:5: note: the 'defer' is here"

printf '%s\nvoid f(int *other) {\n    int *p = malloc(4);\n    defer free(p);\n    {\n        int *p = other;\n        return;\n    }\n}\n' "$prelude" >"$d/x.c"
defer_case "6: a name that means something else at an exit" 1 \
    "x.c:9:9: error: 'p' in this deferred statement would refer to a different declaration at this exit
x.c:8:14: note: 'p' is declared here
x.c:6:5: note: the 'defer' is here"

printf '%s\nvoid f(int n) {\n    void *t = &&out;\n    defer puts("x");\n    if (n) goto *t;\nout:\n    ;\n}\n' "$prelude" >"$d/x.c"
defer_case "7: no computed goto in the scope of a defer" 1 \
    "x.c:7:12: error: computed 'goto' is not allowed in the scope of a 'defer'
x.c:6:5: note: the 'defer' is here"

printf '%s\nint __niu_mine;\n' "$prelude" >"$d/x.c"
defer_case "8: __niu names are reserved" 1 \
    "x.c:4:5: error: identifiers starting with '__niu' are reserved"

# '$' is what niu's markers refer to (niu.h declares it)
printf '%s\nint $;\nint main(void) { defer puts("x"); return 0; }\n' "$prelude" >"$d/x.c"
defer_case "a file-scope \$ is reserved" 1 \
    "x.c:4:5: error: '\$' is reserved in niu files"

printf '%s\nint f(int $) {\n    int v = $;\n    defer puts("x");\n    return v;\n}\n' "$prelude" >"$d/x.c"
defer_case "a parameter named \$ is reserved" 1 \
    "x.c:4:11: error: '\$' is reserved in niu files"

printf '#define $ 1\n%s\nint main(void) { defer puts("x"); return $; }\n' "$prelude" >"$d/x.c"
defer_case "an object-like macro \$ is reserved, even before niu.h" 1 \
    "x.c:1:9: error: '\$' is reserved in niu files"

printf '%s\n#define $(x) (x)\nstruct $ { int $; };\nint main(void) {\n    struct $ s;\n    s.$ = 0;\n    defer puts("x");\n    return $(s.$);\n}\n' "$prelude" >"$d/x.c"
defer_case "a function-like macro, tag or member named \$ is fine" 0 ""

# spec.md, constraint 9
printf '%s\nvoid log_error(const char *);\nvoid f(int bad, int worse) {\n    char *p = malloc(8);\n    defer free(p);\n    if (bad) {\n        defer_now;\n        fprintf(stderr, "fatal\\n");\n        exit(1);\n    }\n    if (worse) {\n        defer_now;\n        log_error("worse");\n    }\n}\n' "$prelude" >"$d/x.c"
defer_case "9: control continues after defer_now" 1 \
    "x.c:16:5: error: control can leave the scope of a 'defer' after 'defer_now'; its cleanup would run twice
x.c:14:9: note: 'defer_now' is here"

printf '%s\nvoid f(char *p) {\n    defer free(p);\n    for (;;) {\n        defer_now;\n        break;\n    }\n}\n' "$prelude" >"$d/x.c"
defer_case "9: a jump after defer_now" 1 \
    "x.c:8:9: error: control can leave the scope of a 'defer' after 'defer_now'; its cleanup would run twice
x.c:7:9: note: 'defer_now' is here"

printf '%s\nvoid f(char *p, int a) {\n    defer free(p);\n    if (a) {\n        defer_now;\n        if (a > 1)\n            abort();\n        else\n            exit(1);\n    }\n}\n' "$prelude" >"$d/x.c"
defer_case "9: every path ends in a noreturn call" 0 ""

printf '%s\n_Noreturn void die(void);\nvoid f(char *p) {\n    defer free(p);\n    {\n        defer_now;\n        die();\n    }\n}\n' "$prelude" >"$d/x.c"
defer_case "9: _Noreturn functions count" 0 ""

printf '%s\nvoid f(void) {\n    defer { defer_now; }\n}\n' "$prelude" >"$d/x.c"
defer_case "10: no defer_now in a deferred statement" 1 \
    "x.c:5:13: error: 'defer_now' cannot appear inside a deferred statement
x.c:5:5: note: the 'defer' is here"

printf '%s\n#define LOG(msg) fprintf(stderr, "%%s\\n", msg)\nint f(void) {\n    defer LOG("leaving");\n#undef LOG\n#define LOG(msg)\n    return 0;\n}\n' "$prelude" >"$d/x.c"
defer_case "11: a macro the defer uses is redefined" 1 \
    "x.c:7:1: error: 'LOG' is redefined while a deferred statement that uses it is still pending
x.c:6:5: note: the 'defer' is here"

printf '%s\nvoid f(void) {\n    defer printf("%%d", __COUNTER__);\n}\n' "$prelude" >"$d/x.c"
defer_case "11: no __COUNTER__ in a deferred statement" 1 \
    "x.c:5:5: error: '__COUNTER__' cannot be used in a deferred statement"

printf '%s\nvoid f(void) {\n    defer_now;\n}\n' "$prelude" >"$d/x.c"
defer_case "-Wniu-empty-defer-now" 0 \
    "x.c:5:5: warning: 'defer_now' has no pending defers to run [-Wniu-empty-defer-now]"
defer_case "-Wno-niu-empty-defer-now turns it off" 0 "" -Wno-niu-empty-defer-now

printf 'struct opts { int defer; int defer_now; };\n#include <stdio.h>\n#include <niu.h>\nint f(struct opts *p) {\n    struct opts o = { .defer = 1, .defer_now = 2 };\n    defer printf("%%d", o.defer);\n    return p->defer + p -> defer_now;\n}\n' >"$d/x.c"
defer_case "after . or -> a keyword is a member name" 0 ""

printf '%s\nint f(int x) {\n    return x-->defer;\n}\n' "$prelude" >"$d/x.c"
defer_case "the > of --> is not a member access" 1 \
    "x.c:5:16: error: 'defer' is a keyword in niu files and cannot be used as a name"

printf '%s\nvoid f(void) {\n    defer\n}\n' "$prelude" >"$d/x.c"
defer_case "a defer needs a statement" 1 \
    "x.c:5:5: error: expected a statement after 'defer'"

printf '%s\nvoid f(void) {\n    int defer = 3;\n    (void)defer;\n}\n' "$prelude" >"$d/x.c"
defer_case "a niu keyword cannot be a name in a niu file" 1 \
    "x.c:5:9: error: 'defer' is a keyword in niu files and cannot be used as a name
x.c:6:11: error: 'defer' is a keyword in niu files and cannot be used as a name"

printf '#include <stdio.h>\nint defer = 1;\n#include <niu.h>\nvoid f(void) { defer puts("x"); }\n' >"$d/x.c"
defer_case "before #include <niu.h>, defer is a name" 0 ""

# Defers and macros (the companion macro, spec notes)
printf '%s\n#define CHECK(x) if (!(x)) return -1\nint f(char *p) {\n    defer free(p);\n    CHECK(p);\n    return 0;\n}\n' "$prelude" >"$d/x.c"
defer_case "an exit from a macro in the scope of a defer" 1 \
    "x.c:7:5: error: 'return' comes from the macro 'CHECK' and leaves the scope of a 'defer'; niu cannot add the cleanup there
x.c:6:5: note: the 'defer' is here"

printf '%s\n#define TWO(p) defer free(p); puts("x")\nvoid f(char *p) {\n    TWO(p);\n}\n' "$prelude" >"$d/x.c"
defer_case "a defer must be the last statement of its macro" 1 \
    "x.c:6:5: error: the 'defer' must be the last statement in the macro 'TWO'
x.c:4:1: note: 'TWO' is defined here"

printf '%s\n#define AUTOFREE(p) defer free(p)\n#define OUTER(p) AUTOFREE(p)\nvoid f(char *p) {\n    OUTER(p);\n}\n' "$prelude" >"$d/x.c"
defer_case "a macro with a defer must be used directly" 1 \
    "x.c:7:5: error: the macro 'AUTOFREE' contains a 'defer', so it must be used directly, not from another macro
x.c:4:1: note: 'AUTOFREE' is defined here"

printf '%s\n#define FATAL() do { defer_now; exit(1); } while (0)\n' "$prelude" >"$d/x.c"
defer_case "no defer_now in a macro" 1 \
    "x.c:4:22: error: 'defer_now' cannot be used in a macro"

# ---------------------------------------------------------------------------
echo
echo "== var and let errors"

d=$(fresh_dir var-errors)

# var_out <test name> <expected line>...: transpiles x.c and checks that the
# output has each line
var_out() {
    vo_name=$1
    shift
    if ! (cd "$d" && "$niu" transpile x.c) >"$d/log" 2>&1 || [ -s "$d/log" ]; then
        fail "$vo_name: transpile failed" "$d/log"
        return 1
    fi
    for l in "$@"; do
        if ! grep -F -x -- "$l" "$d/x.plain.c" >/dev/null; then
            { echo "the output has no line: $l"; cat "$d/x.plain.c"; } >"$d/why"
            fail "$vo_name" "$d/why"
            return 1
        fi
    done
    pass "$vo_name"
}

printf '%s\nvoid f(void) {\n    var x;\n}\n' "$prelude" >"$d/x.c"
defer_case "1: var requires an initializer" 1 \
    "x.c:5:5: error: 'var' requires an initializer"

printf '%s\nvoid f(void) {\n    var a = {1, 2};\n}\n' "$prelude" >"$d/x.c"
defer_case "1: no brace initializer" 1 \
    "x.c:5:13: error: 'var' cannot take a brace initializer; write the type instead"

printf '%s\nstruct point { int x, y; };\nvoid f(void) {\n    var a = (struct point){1, 2};\n    (void)a;\n}\n' "$prelude" >"$d/x.c"
defer_case "1: a compound literal is an expression" 0 ""

printf '%s\nvoid f(void) {\n    var a = 1, b = 2.0;\n}\n' "$prelude" >"$d/x.c"
defer_case "2: one variable per declaration" 1 \
    "x.c:5:16: error: 'var' declares a single variable"

printf '%s\nvoid f(int n) {\n    var *p = &n;\n}\n' "$prelude" >"$d/x.c"
defer_case "3: no pointer declarator" 1 \
    "x.c:5:9: error: the name after 'var' must be a plain identifier; its type comes from the initializer"

printf '%s\nvoid f(void) {\n    var a[] = "x";\n}\n' "$prelude" >"$d/x.c"
defer_case "3: no array declarator" 1 \
    "x.c:5:10: error: the name after 'var' must be a plain identifier; its type comes from the initializer"

printf '%s\nint g(void);\nvoid f(void) {\n    var h(void) = g;\n}\n' "$prelude" >"$d/x.c"
defer_case "3: no function declarator" 1 \
    "x.c:6:10: error: the name after 'var' must be a plain identifier; its type comes from the initializer"

printf '%s\nvoid f(void) {\n    var x = x + 1;\n}\n' "$prelude" >"$d/x.c"
defer_case "4: not in its own initializer" 1 \
    "x.c:5:13: error: 'x' cannot appear in its own initializer"

printf '%s\nstruct o { int x; };\nvoid f(struct o o) {\n    var x = o.x;\n    (void)x;\n}\n' "$prelude" >"$d/x.c"
defer_case "4: a member with the same name is fine" 0 ""

printf '%s\nvoid f(void) {\n    var u = (struct { int z; }){1};\n}\n' "$prelude" >"$d/x.c"
defer_case "5: a type without a name" 1 \
    "x.c:5:5: error: the type of 'u' has no name, so niu cannot write it out"

printf '%s\nvoid f(int n) {\n    int a[n];\n    var p = &a;\n}\n' "$prelude" >"$d/x.c"
defer_case "5: a variably modified type" 1 \
    "x.c:6:5: error: the type of 'p' is a variably modified type, which niu cannot write out"

printf '%s\nvoid f(void) {\n    extern var x = 1;\n}\n' "$prelude" >"$d/x.c"
defer_case "6: not with extern" 1 \
    "x.c:5:5: error: 'var' cannot be used with 'extern'"

printf '%s\ntypedef var x = 1;\n' "$prelude" >"$d/x.c"
defer_case "6: not with typedef" 1 \
    "x.c:4:1: error: 'var' cannot be used with 'typedef'"

printf '%s\nvoid f(void) {\n    auto var x = 1;\n}\n' "$prelude" >"$d/x.c"
defer_case "6: not with auto" 1 \
    "x.c:5:5: error: 'var' cannot be used with 'auto'"

printf '%s\nvoid f(void) {\n    const var x = 1;\n}\n' "$prelude" >"$d/x.c"
defer_case "6: no qualifiers" 1 \
    "x.c:5:5: error: 'var' takes the place of the type; use 'let' for a 'const' variable"

printf '%s\nvoid f(void) {\n    unsigned let x = 1;\n}\n' "$prelude" >"$d/x.c"
defer_case "6: no type specifiers with let" 1 \
    "x.c:5:5: error: 'let' takes the place of the type; 'let' already makes the variable 'const'"

printf '%s\nvoid f(void) {\n    const let x = 1;\n}\n' "$prelude" >"$d/x.c"
defer_case "6: no const with let" 1 \
    "x.c:5:5: error: 'let' takes the place of the type; 'let' already makes the variable 'const'"

printf '%s\nvoid f(void) {\n    _Alignas(16) var x = 1;\n}\n' "$prelude" >"$d/x.c"
defer_case "6: not with _Alignas" 1 \
    "x.c:5:5: error: 'var' cannot be used with '_Alignas'; write the type out instead"

printf '%s\nvoid f(void) {\n    static var a = 1;\n    register let b = 2;\n    (void)a; (void)b;\n}\n' "$prelude" >"$d/x.c"
defer_case "6: static and register are fine" 0 ""

printf '%s\n#define V var x = 1\n' "$prelude" >"$d/x.c"
defer_case "7: not in a macro" 1 \
    "x.c:4:11: error: 'var' cannot be used in a macro"

printf '%s\n#define ID(x) x\nint f(void) {\n    ID(var v = 1;)\n    return v;\n}\n' "$prelude" >"$d/x.c"
var_out "7: in a macro argument it is lowered" "    ID(int v = 1;)"

printf '%s\nvoid g(var x);\n' "$prelude" >"$d/x.c"
defer_case "8: not a parameter" 1 \
    "x.c:4:8: error: 'var' can only declare a variable"

printf '%s\nstruct m { let x; };\n' "$prelude" >"$d/x.c"
defer_case "8: not a member" 1 \
    "x.c:4:12: error: 'let' can only declare a variable"

printf '%s\nvar h(void) { return 1; }\n' "$prelude" >"$d/x.c"
defer_case "8: not a function" 1 \
    "x.c:4:1: error: 'var' can only declare a variable"

printf '%s\nint f(void) {\n    return (var)1 + sizeof(var);\n}\n' "$prelude" >"$d/x.c"
defer_case "8: not in a cast or sizeof" 1 \
    "x.c:5:13: error: 'var' can only declare a variable
x.c:5:28: error: 'var' can only declare a variable"

# spec.md, constraint 9
printf '%s\nstruct S { int a; };\nstruct S *get_s(void);\nvoid f(void) {\n    struct S { double d; };\n    var p = get_s();\n}\n' "$prelude" >"$d/x.c"
defer_case "9: the type's name is hidden" 1 \
    "x.c:8:5: error: the type of 'p' cannot be written here: 'struct S' means a different type in this scope
x.c:7:12: note: 'struct S' is declared here"

printf '%s\ntypedef struct { int a; } A;\nA get_a(void);\nvoid f(void) {\n    int A = 0;\n    var p = get_a();\n}\n' "$prelude" >"$d/x.c"
defer_case "9: a hidden typedef of a struct without a tag" 1 \
    "x.c:8:5: error: the type of 'p' cannot be written here: 'A' means a different type in this scope
x.c:7:9: note: 'A' is declared here"

printf '%s\nvoid f(void) {\n    var v = ({ struct L { int a; } t = {1}; t; });\n}\n' "$prelude" >"$d/x.c"
defer_case "9: a name that is out of scope has no note" 1 \
    "x.c:5:5: error: the type of 'v' cannot be written here: 'struct L' means a different type in this scope"

# the fallback order (the portable sample tests it too)
printf '%s\ntypedef struct S { int a; } S;\nS get_s(void);\nstruct T { int t; };\ntypedef struct T T_t;\nstruct T get_t(void);\ntypedef unsigned short u16;\nu16 get_u(void);\nvoid f(void) {\n    var a = get_s();\n    int S = 0, u16 = 0;\n    var b = get_s();\n    struct T { double d; };\n    var t = get_t();\n    var u = get_u();\n    (void)a; (void)b; (void)t; (void)u; (void)S; (void)u16;\n}\n' "$prelude" >"$d/x.c"
var_out "9: typedef name, then tag, then another typedef, then the underlying type" \
    "    S a = get_s();" "    struct S b = get_s();" "    T_t t = get_t();" "    unsigned short u = get_u();"

# let's const goes next to the name
printf '%s\nvoid handler(int);\nint grid[8][8];\nvoid f(void) {\n    let cb = handler;\n    let row = &grid[0];\n    let n = 1;\n    (void)cb; (void)row; (void)n;\n}\n' "$prelude" >"$d/x.c"
var_out "let: const inside the declarator" \
    "    void (*const cb)(int) = handler;" "    int (*const row)[8] = &grid[0];" "    const int n = 1;"

printf '%s\nvoid f(void) {\n    int var = 3;\n    (void)var;\n}\n' "$prelude" >"$d/x.c"
defer_case "var is a keyword in niu files" 1 \
    "x.c:5:9: error: 'var' is a keyword in niu files and cannot be used as a name
x.c:6:11: error: 'var' is a keyword in niu files and cannot be used as a name"

printf 'struct opts { int var; int let; };\n#include <stdio.h>\n#include <niu.h>\nint f(struct opts *p) {\n    struct opts o = { .var = 1, .let = 2 };\n    let v = o.var;\n    return p->let + p -> var + v;\n}\n' >"$d/x.c"
defer_case "after . or -> var and let are member names" 0 ""

# var and let in a deferred statement: the copies have the written type,
# and defer constraint 6 covers the names niu writes
printf '%s\n#include <string.h>\nint f(const char *s, int bad) {\n    defer { var n = strlen(s); let p = &n; (void)p; }\n    if (bad) return 1;\n    return 0;\n}\n' "$prelude" >"$d/x.c"
var_out "var and let in a deferred statement" \
    "    if (bad) { int __niu_return_temp = 1; { size_t n = strlen(s); size_t *const p = &n; (void)p; } return __niu_return_temp; }"

printf '%s\ntypedef struct S { int a; } S;\nS get_s(void);\nint f(int bad) {\n    defer { var p = get_s(); (void)p; }\n    {\n        int S = 0;\n        if (bad) return S;\n    }\n    return 0;\n}\n' "$prelude" >"$d/x.c"
defer_case "defer 6: the type niu writes must mean the same at every exit" 1 \
    "x.c:10:18: error: 'S' in this deferred statement would refer to a different declaration at this exit
x.c:9:13: note: 'S' is declared here
x.c:7:5: note: the 'defer' is here"

# libclang's columns after a var on the same line are those of the original
printf '%s\nvoid f(void) {\n    var a = 1; int b = a + missing;\n}\n' "$prelude" >"$d/x.c"
defer_case "libclang's columns after a var are the original's" 1 \
    "x.c:5:28: error: use of undeclared identifier 'missing'"

# niu.h's tooling stubs: var and let as __auto_type, which gcc and clang
# know (cl and tcc do not, see spec.md "Editor tools"). compile uses
# -pedantic-errors, so this also checks that niu.h turns clang's
# -Wgnu-auto-type off.
tooling_var_c='#include <stdio.h>
#include <string.h>
#include <niu.h>

int main(void)
{
    var n = strlen("abc");
    let p = &n;
    if (*p != 3)
        return 1;
    printf("ok\n");
    return 0;
}
'
for cc in $compilers; do
    command -v "$cc" >/dev/null 2>&1 || continue
    kind=$(compiler_kind "$cc")
    [ "$kind" = gnu ] || continue
    d=$(fresh_dir "niu-h-var-$(basename "$cc")")
    printf '%s' "$tooling_var_c" >"$d/tool.c"
    if compile "$cc" "$kind" c11 "$d/tool.c" "$d/prog$exe" "$d/log" "-I$root/include" -D__NIU_TOOLING__ &&
        (cd "$d" && ./prog$exe) >"$d/stdout" 2>>"$d/log" &&
        [ "$(tr -d '\r' <"$d/stdout")" = ok ]; then
        pass "$cc: -D__NIU_TOOLING__ compiles var and let as __auto_type with -pedantic-errors"
    else
        fail "$cc: var and let do not compile with -D__NIU_TOOLING__" "$d/log"
    fi
done

# ---------------------------------------------------------------------------
echo
echo "passed: $passes, failed: $fails, skipped: $skips"
[ $fails = 0 ]
