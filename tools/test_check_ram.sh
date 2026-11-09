#!/usr/bin/env bash
# Exercises tools/check_ram.sh against canned toolchain output: no ESP-IDF,
# no ELF. Two stand-ins for riscv32-esp-elf-size and riscv32-esp-elf-nm print
# whatever sits next to the "ELF" a case names, so each case is three small
# text files and an expected exit code.
#   usage: tools/test_check_ram.sh
#   exit:  0 all cases passed, 1 something regressed
set -uo pipefail
cd "$(dirname "$0")/.."

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

mkdir "$work/bin"
cat > "$work/bin/riscv32-esp-elf-size" <<'EOF'
#!/usr/bin/env bash
# The last argument is the ELF; its canned answer sits beside it.
for last; do :; done
cat "$last.size"
EOF
cat > "$work/bin/riscv32-esp-elf-nm" <<'EOF'
#!/usr/bin/env bash
for last; do :; done
cat "$last.nm"
EOF
chmod +x "$work/bin/riscv32-esp-elf-size" "$work/bin/riscv32-esp-elf-nm"

# 0x4FF20000 holds code, 0x4FF34000 data, 0x4FF38000 bss: 300000 bytes of
# internal RAM in all. The flash section is outside it and counts for nothing.
sections() {   # sections <elf>
    : > "$1"   # the "ELF" itself: the stand-ins only read what sits beside it
    cat > "$1.size" <<'EOF'
app.elf  :
section            size         addr
.iram0.text       80000   1341259776
.dram0.data       20000   1341339776
.dram0.bss       200000   1341359776
.flash.text     1600000   1073741856
Total           1900000
EOF
}

fail=0
check() {   # check <name> <expected_exit> [<text expected in the output>]
    local name="$1" expected_exit="$2" expected_text="${3:-}"
    local out rc
    out="$(PATH="$work/bin:$PATH" tools/check_ram.sh "$work/$name.elf" "$work/$name.budget" 2>&1)"
    rc=$?
    if printf '%s' "$out" | grep -q 'check_ram.sh: line'; then
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

# ---- under the ceiling, every large static listed --------------------------
sections "$work/fits.elf"
cat > "$work/fits.elf.nm" <<'EOF'
4ff3a2e4 00006000 b stack.0	/src/components/kgui/kgui_ao.c:12920
4ff63134 000040b0 b s_pool	/src/sdk/components/kqp/src/kqp.c:45
4ff20100 00000400 b s_small	/src/sdk/components/kaos/boot/src/boot_ao.c:10
EOF
cat > "$work/fits.budget" <<'EOF'
# a comment, and a blank line after it

static_max 310000
stack.0   kgui_ao.c   24576
s_pool    kqp.c       16560
EOF
check fits 0 "internal RAM, static: 300000 of 310000 bytes, 10000 left"

# ---- the static image is past the ceiling ----------------------------------
sections "$work/over.elf"
cp "$work/fits.elf.nm" "$work/over.elf.nm"
sed 's/^static_max .*/static_max 299999/' "$work/fits.budget" > "$work/over.budget"
check over 1 "past the ceiling"

# ---- a large static nobody listed ------------------------------------------
sections "$work/unlisted.elf"
cat > "$work/unlisted.elf.nm" <<'EOF'
4ff40000 0000bb80 b s_run	/src/sdk/components/kdrv/rfid/src/krfid_em4100.c:31
4ff3a2e4 00006000 b stack.0	/src/components/kgui/kgui_ao.c:12920
4ff63134 000040b0 b s_pool	/src/sdk/components/kqp/src/kqp.c:45
EOF
cp "$work/fits.budget" "$work/unlisted.budget"
check unlisted 1 "s_run (krfid_em4100.c) puts 48000 bytes"

# ---- a listed static that grew past its line -------------------------------
sections "$work/grew.elf"
cat > "$work/grew.elf.nm" <<'EOF'
4ff3a2e4 00008000 b stack.0	/src/components/kgui/kgui_ao.c:12920
4ff63134 000040b0 b s_pool	/src/sdk/components/kqp/src/kqp.c:45
EOF
cp "$work/fits.budget" "$work/grew.budget"
check grew 1 "stack.0 (kgui_ao.c) is 32768 bytes, listed at 24576"

# ---- the same name in another file is another static -----------------------
sections "$work/twin.elf"
cat > "$work/twin.elf.nm" <<'EOF'
4ff3a2e4 00006000 b stack.0	/src/components/kgui/kgui_ao.c:12920
4ff63134 000040b0 b s_pool	/src/sdk/components/kqp/src/kqp.c:45
4ff50000 000040b0 b s_pool	/src/sdk/components/kdrv/kdrv_new.c:12
EOF
cp "$work/fits.budget" "$work/twin.budget"
check twin 1 "s_pool (kdrv_new.c) puts 16560 bytes"

# ---- large, but not in internal RAM, or not data: none of its business -----
sections "$work/elsewhere.elf"
cat > "$work/elsewhere.elf.nm" <<'EOF'
48000000 00100000 b s_in_psram	/src/sdk/components/kdrv/kdrv_x.c:1
40001000 00010000 T big_function	/src/sdk/components/kdrv/kdrv_x.c:2
4ff20000 00003000 T iram_function	/src/sdk/components/kdrv/kdrv_x.c:3
4ff3a2e4 00006000 b stack.0	/src/components/kgui/kgui_ao.c:12920
4ff63134 000040b0 b s_pool	/src/sdk/components/kqp/src/kqp.c:45
EOF
cp "$work/fits.budget" "$work/elsewhere.budget"
check elsewhere 0

# ---- a symbol with no source line is named by a dash -----------------------
sections "$work/nodebug.elf"
cat > "$work/nodebug.elf.nm" <<'EOF'
4ff3a2e4 00006000 b vendor_blob
EOF
printf 'static_max 310000\nvendor_blob  -  24576\n' > "$work/nodebug.budget"
check nodebug 0

# ---- a budget with no ceiling in it ----------------------------------------
sections "$work/noceiling.elf"
cp "$work/fits.elf.nm" "$work/noceiling.elf.nm"
grep -v '^static_max' "$work/fits.budget" > "$work/noceiling.budget"
check noceiling 1 "static_max"

# ---- a budget that is not there --------------------------------------------
sections "$work/nobudget.elf"
cp "$work/fits.elf.nm" "$work/nobudget.elf.nm"
check nobudget 1 "missing"

if [ "$fail" -eq 0 ]; then
    echo "all cases passed"
else
    echo "one or more cases failed"
fi
exit "$fail"
