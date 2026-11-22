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
#                 (cl and clang on Windows).
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

    # --no-line: the same without line 2. This also replaces the file
    # written above, which niu may do because it generated it.
    sed '2d' "$here/expected/$out" >"$d/expected-no-line"
    if (cd "$d" && "$niu" transpile --no-line "$name") >"$d/log" 2>&1 &&
        cmp "$d/$out" "$d/expected-no-line" >"$d/cmp" 2>&1; then
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
        candidates="cl clang"
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
compile() {
    c_cc=$1 c_kind=$2 c_std=$3 c_src=$4 c_exe=$5 c_log=$6
    shift 6
    case $c_kind in
    msvc) (cd "$(dirname "$c_src")" && "$c_cc" -nologo -W4 -WX "-std:$c_std" "$@" "-Fe$(basename "$c_exe")" "$(basename "$c_src")") >"$c_log" 2>&1 ;;
    tcc) "$c_cc" "-std=$c_std" -Wall -Werror "$@" -o "$c_exe" "$c_src" >"$c_log" 2>&1 ;;
    *) "$c_cc" "-std=$c_std" -Wall -Wextra -Werror -pedantic-errors "$@" -o "$c_exe" "$c_src" >"$c_log" 2>&1 ;;
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
            d=$(fresh_dir "cc-$(basename "$cc")-$label-$name" "$name")
            if ! (cd "$d" && "$niu" transpile "-std=$nstd" "$name") >"$d/log" 2>&1; then
                fail "$cc $label $name: niu transpile -std=$nstd failed" "$d/log"
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
            { [ $variant = no-line ] || ! grep -F where.plain.c "$d/log" >/dev/null; }; then
            pass "$cc: errors point at $(basename "$want") ($variant)"
        else
            { echo "expected the compiler's messages to name $want (and with #line, not where.plain.c):"; cat "$d/log"; } >"$d/why"
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
echo "passed: $passes, failed: $fails, skipped: $skips"
[ $fails = 0 ]
