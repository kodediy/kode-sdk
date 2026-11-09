#!/usr/bin/env bash
# Exercises tools/check_misra.sh against a canned report: no cppcheck, no
# ESP-IDF, no build. A stand-in for cppcheck answers `--version` and prints
# the report a case wrote, so each case is a report, a baseline and an
# expected exit code. The source tree is three empty files under a temporary
# directory, which the script is run from.
#   usage: tools/test_check_misra.sh
#   exit:  0 all cases passed, 1 something regressed
set -uo pipefail
script="$(cd "$(dirname "$0")" && pwd)/check_misra.sh"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
# The script compares against the physical path, and a temporary directory
# is a symbolic link on some hosts.
work="$(cd "$work" && pwd -P)"

mkdir "$work/bin"
cat > "$work/bin/cppcheck" <<'EOF'
#!/usr/bin/env bash
# The report goes to stderr with absolute paths, the way cppcheck prints it.
if [ "$1" = "--version" ]; then
    echo "Cppcheck 9.9.9"
    exit 0
fi
sed "s|^|$PWD/|" "$FAKE_REPORT" >&2
EOF
chmod +x "$work/bin/cppcheck"

mkdir -p "$work/src/components/kaos/core/include" "$work/src/components/qpc" \
         "$work/src/components/kui/fonts" "$work/src/components/kode/internal/include"
: > "$work/src/components/kaos/a_ao.c"
: > "$work/src/components/kaos/include/a.h"
: > "$work/src/components/qpc/qf.c"
: > "$work/src/components/kui/fonts/kui_font.c"
: > "$work/src/components/kode/internal/include/kode_colors.h"
cat > "$work/db.json" <<EOF
[
 {"directory": "$work/src", "command": "cc -c a_ao.c", "file": "$work/src/components/kaos/a_ao.c"},
 {"directory": "$work/src", "command": "cc -c qf.c", "file": "$work/src/components/qpc/qf.c"},
 {"directory": "$work/src", "command": "cc -c kui_font.c", "file": "$work/src/components/kui/fonts/kui_font.c"}
]
EOF

# Three findings in the source, one in its header, and three in files that are
# never judged.
cat > "$work/plain.report" <<'EOF'
components/kaos/a_ao.c:10:misra-c2012-8.7
components/kaos/a_ao.c:20:misra-c2012-8.7
components/kaos/a_ao.c:30:misra-c2012-10.8
components/kaos/include/a.h:5:misra-c2012-2.5
components/qpc/qf.c:1:misra-c2012-8.7
components/kui/fonts/kui_font.c:1:misra-c2012-8.7
components/kode/internal/include/kode_colors.h:7:misra-c2012-2.5
EOF
cat > "$work/plain.baseline" <<'EOF'
cppcheck 9.9.9
components/kaos/a_ao.c misra-c2012-10.8 1
components/kaos/a_ao.c misra-c2012-8.7 2
components/kaos/include/a.h misra-c2012-2.5 1
EOF

fail=0
check() {   # check <name> <report> <baseline> <expected_exit> [<text expected in the output>]
    local name="$1" report="$2" baseline="$3" expected_exit="$4" expected_text="${5:-}"
    local out rc
    out="$(cd "$work/src" && PATH="$work/bin:$PATH" FAKE_REPORT="$work/$report" \
           "$script" "$work/db.json" "$work/$baseline" components 2>&1)"
    rc=$?
    if printf '%s' "$out" | grep -q 'check_misra.sh: line'; then
        # The shell complaining about the script itself, whatever the verdict.
        printf 'FAIL  %s  the script tripped over its own input:\n%s\n' "$name" "$out"
        fail=1
    elif [ "$rc" -ne "$expected_exit" ]; then
        printf 'FAIL  %s  expected exit %s, got %s\n%s\n' "$name" "$expected_exit" "$rc" "$out"
        fail=1
    elif [ -n "$expected_text" ] && ! printf '%s' "$out" | grep -qF -- "$expected_text"; then
        printf 'FAIL  %s  expected the output to say %q, got:\n%s\n' "$name" "$expected_text" "$out"
        fail=1
    else
        printf 'ok    %s\n' "$name"
    fi
}

# ---- what the baseline allows, and the vendored files count for nothing ----
check fits plain.report plain.baseline 0 "MISRA: 4 findings in 2 files, none past the baseline"

# ---- one more of an id the file already had --------------------------------
{ cat "$work/plain.report"; echo 'components/kaos/a_ao.c:40:misra-c2012-8.7'; } > "$work/more.report"
check more more.report plain.baseline 1 "a_ao.c has 3 of misra-c2012-8.7, the baseline allows 2"

# ---- an id the file did not have -------------------------------------------
{ cat "$work/plain.report"; echo 'components/kaos/a_ao.c:40:nullPointer'; } > "$work/new.report"
check new new.report plain.baseline 1 "a_ao.c has 1 of nullPointer, the baseline allows 0"

# ---- the failure names every line of the id, in line order -----------------
check lines more.report plain.baseline 1 "a_ao.c:40"

# ---- fewer than allowed passes, and says the baseline can come down --------
grep -v ':20:' "$work/plain.report" > "$work/fewer.report"
check fewer fewer.report plain.baseline 0 "1 fewer than"

# ---- a header included by two sources is one finding -----------------------
{ cat "$work/plain.report"; echo 'components/kaos/include/a.h:5:misra-c2012-2.5'; } > "$work/twice.report"
check twice twice.report plain.baseline 0

# ---- a file cppcheck could not read is never baselined ---------------------
{ cat "$work/plain.report"; echo 'components/kaos/a_ao.c:1:syntaxError'; } > "$work/blind.report"
{ cat "$work/plain.baseline"; echo 'components/kaos/a_ao.c syntaxError 1'; } > "$work/blind.baseline"
check blind blind.report blind.baseline 1 "could not analyse"

# ---- a baseline taken with another cppcheck --------------------------------
sed '1s/.*/cppcheck 1.0.0/' "$work/plain.baseline" > "$work/version.baseline"
check version plain.report version.baseline 1 "was taken with cppcheck '1.0.0' and this is 9.9.9"

# ---- a baseline that is not there ------------------------------------------
check nobaseline plain.report absent.baseline 1 "missing"

# ---- --update writes what a plain run then accepts -------------------------
out="$(cd "$work/src" && PATH="$work/bin:$PATH" FAKE_REPORT="$work/more.report" \
       "$script" --update "$work/db.json" "$work/written.baseline" components 2>&1)"
if [ "$?" -ne 0 ] || ! grep -qx 'cppcheck 9.9.9' "$work/written.baseline" \
   || ! grep -qx 'components/kaos/a_ao.c misra-c2012-8.7 3' "$work/written.baseline" \
   || grep -q 'qpc\|fonts' "$work/written.baseline"; then
    printf 'FAIL  update  the baseline written is not the run:\n%s\n' "$out"
    cat "$work/written.baseline" 2> /dev/null
    fail=1
else
    printf 'ok    update\n'
fi
check updated more.report written.baseline 0

# ---- a build that compiles nothing under the directory ---------------------
echo '[]' > "$work/empty.json"
out="$(cd "$work/src" && PATH="$work/bin:$PATH" FAKE_REPORT="$work/plain.report" \
       "$script" "$work/empty.json" "$work/plain.baseline" components 2>&1)"
if [ "$?" -ne 1 ] || ! printf '%s' "$out" | grep -qF 'compiles no .c file'; then
    printf 'FAIL  nofiles  expected a refusal, got:\n%s\n' "$out"
    fail=1
else
    printf 'ok    nofiles\n'
fi

if [ "$fail" -eq 0 ]; then
    echo "all cases passed"
else
    echo "one or more cases failed"
fi
exit "$fail"
