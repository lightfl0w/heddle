#!/bin/bash
set -u

ORIG_PATH=$PATH
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)

find_heddle() {
  local c
  for c in "$ROOT"/build/*/*/release/heddle "$ROOT"/build/*/*/release/heddle.exe; do
    [ -x "$c" ] && { printf '%s' "$c"; return; }
  done
  printf '%s' "$ROOT/build/linux/x86_64/release/heddle"
}

HEDDLE=${HEDDLE:-$(find_heddle)}

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) EXE=.exe; FAKE_TOOLS=0; HOST_OS=windows ;;
    Darwin)               EXE=;     FAKE_TOOLS=1; HOST_OS=macos ;;
    *)                    EXE=;     FAKE_TOOLS=1; HOST_OS=linux ;;
esac

if [ "$HOST_OS" = linux ]; then M32=1; else M32=0; fi

pass=0
fail=0

ok()   { printf '  \033[32mok\033[0m   %s\n' "$1"; pass=$((pass + 1)); }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1"; fail=$((fail + 1)); }

check_eq() {
    [ "$2" = "$3" ] && ok "$1" || bad "$1 (want '$3', got '$2')"
}

check_rc() {
    [ "$2" -eq "$3" ] && ok "$1" || bad "$1 (want rc=$3, got rc=$2)"
}

expect_err() {
    case "$2" in
        *"$3"*) ok "$1" ;;
        *)      bad "$1 (expected error containing '$3', got '$2')" ;;
    esac
}

run_show() {
    local o
    o=$("$@" 2>&1)
    local r=$?
    [ $r -eq 0 ] || printf '    [%s rc=%d]\n%s\n' "$1" "$r" "$o"
    return $r
}

if [ ! -x "$HEDDLE" ]; then
    echo "heddle not found at $HEDDLE; run 'xmake' first or set HEDDLE=" >&2
    exit 2
fi

echo "heddle: $HEDDLE"
echo

echo "cli:"
cd "$HERE/basic"
rm -rf out .heddle
$HEDDLE app >/dev/null 2>&1
check_rc "bare target" $? 0
rm -rf out .heddle
$HEDDLE build app >/dev/null 2>&1
check_rc "build subcommand" $? 0
rm -rf out .heddle
$HEDDLE -j4 app >/dev/null 2>&1
check_rc "target with -j" $? 0
out=$($HEDDLE 2>&1)
check_rc "no target shows usage" $? 2
expect_err "usage text" "$out" "usage:"
out=$($HEDDLE --bogus 2>&1)
check_rc "unknown option" $? 2
expect_err "unknown option message" "$out" "unknown option"
rm -rf out .heddle
echo

echo "heddle: $HEDDLE"
echo

echo "basic:"
cd "$HERE/basic"
rm -rf out .heddle

out=$($HEDDLE -C . -j4 -v app 2>&1)
rc=$?
check_rc "cold build" $rc 0
[ $rc -eq 0 ] || echo "$out"
check_eq "app output" "$(./out/app$EXE)" "42"

$HEDDLE -C . -j4 app >/dev/null 2>&1
check_rc "noop rebuild" $? 0

sleep 1
printf 'int util_id(void) {\n    return 2;\n}\n' > src/util/util.c
out=$($HEDDLE -C . -j4 -v app 2>&1)
check_rc "rebuild after edit" $? 0
check_eq "app output after edit" "$(./out/app$EXE)" "43"

case "$out" in
    *"4 ran"*) ok "partial rebuild (4 of 6)" ;;
    *)         bad "expected 4 ran, got: $out" ;;
esac

printf 'int util_id(void) {\n    return 1;\n}\n' > src/util/util.c
$HEDDLE -C . -j4 app >/dev/null 2>&1

rm -rf out .heddle
$HEDDLE -C . -t debug -j2 app >/dev/null 2>&1
check_rc "toolchain override" $? 0
if grep -q -- "-O0" .heddle/app.graph 2>/dev/null; then
    ok "debug cflags in graph"
else
    bad "debug cflags not in graph"
fi
rm -rf out .heddle
echo

echo "type aliases:"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cp -r "$HERE/basic/." "$WORK/"

for pair in "executable" "bin" "lib"; do
    sed "s/^type = \"exe\"$/type = \"$pair\"/" "$HERE/basic/heddle.toml" > "$WORK/heddle.toml"
    rm -rf "$WORK/out" "$WORK/.heddle"
    out=$(cd "$WORK" && $HEDDLE check 2>&1)
    check_rc "type=$pair accepted" $? 0
    case "$out" in
        *"targets ok"*) ok "type=$pair checks" ;;
        *)              bad "type=$pair checks" ;;
    esac
done

sed "s/^type = \"exe\"$/type = \"dll\"/" "$HERE/basic/heddle.toml" > "$WORK/heddle.toml"
out=$(cd "$WORK" && $HEDDLE check 2>&1)
check_rc "bad type rejected" $? 1
expect_err "bad type message" "$out" "unknown type 'dll'"

rm -rf "$WORK" "$HERE/basic/out" "$HERE/basic/.heddle"
echo

echo "multipkg:"
cd "$HERE/multipkg"
rm -rf out .heddle

$HEDDLE -C . -j4 app >/dev/null 2>&1
check_rc "cross-package build" $? 0
check_eq "app output" "$(./out/app$EXE)" "6"

$HEDDLE -C . -j4 app >/dev/null 2>&1
check_rc "noop rebuild" $? 0
rm -rf out .heddle
echo

echo "static check:"
cd "$HERE/bad"

for d in unknown_dep missing_src cycle dup_src missing_inc; do
    out=$(cd "$d" && $HEDDLE check 2>&1)
    rc=$?

    case $d in
        unknown_dep) want="unknown dependency" ;;
        missing_src) want="missing source" ;;
        cycle)       want="cycle" ;;
        dup_src)     want="also used by" ;;
        missing_inc) want="missing include dir" ;;
    esac

    check_rc "$d rejected" $rc 1
    expect_err "$d message" "$out" "$want"
done
echo

cd "$HERE/basic"
rm -rf out .heddle
out=$($HEDDLE check 2>&1)
check_rc "valid project passes check" $? 0
expect_err "check reports targets" "$out" "3 targets ok"
rm -rf out .heddle

echo

echo "os:"
cd "$HERE/os"
rm -rf out .heddle

if [ "$M32" = 0 ]; then
    echo "  skip (os fixture needs 32-bit multilib)"
elif command -v nasm >/dev/null 2>&1; then
    $HEDDLE image >/dev/null 2>&1
    check_rc "os image build" $? 0

    sig=$(xxd -s 510 -l 2 -p out/mbr.bin 2>/dev/null)
    check_eq "boot signature" "$sig" "55aa"

    if command -v readelf >/dev/null 2>&1; then
        check_eq "kernel is ELF32" \
            "$(readelf -h out/kernel$EXE 2>/dev/null | awk '/Class:/ {print $2}')" "ELF32"
    else
        echo "  skip (readelf not found)"
    fi

    head_ok=$(cmp -n 512 out/mbr.bin out/os.img >/dev/null 2>&1 && echo yes || echo no)
    check_eq "image starts with mbr" "$head_ok" "yes"

    $HEDDLE image >/dev/null 2>&1
    out=$($HEDDLE image -v 2>&1)
    expect_err "os noop" "$out" "0 ran"

    sleep 1
    printf '%%define PAD %s\n' "$$" > src/mbr.inc
    out=$($HEDDLE image -v 2>&1)
    expect_err "os rebuilds on .inc change" "$out" "2 ran"
else
    echo "  skip (nasm not found)"
fi
echo

if [ "$FAKE_TOOLS" = 0 ]; then
    echo "pkg (fake shell toolchain):"
    echo "  skip (needs a unix shell to fake arm-none-eabi-gcc)"
else
    echo "pkg (toolchain + dependencies):"
    cd "$HERE/pkg"
    rm -rf out .heddle heddle.lock
    export HEDDLE_REGISTRY="$HERE/pkg/registry"

    out=$($HEDDLE tool plan 2>&1)
    check_rc "tool plan" $? 0
    expect_err "plan derives prefix" "$out" "arm-none-eabi-"
    expect_err "plan derives cpu"    "$out" "cortex-m4"
    expect_err "plan lists toolchain" "$out" "arm-none-eabi"
    expect_err "plan lists dependency" "$out" "freertos"

    $HEDDLE tool install >/dev/null 2>&1
    check_rc "tool install" $? 0
    [ -f heddle.lock ] && ok "lockfile written" || bad "lockfile missing"
    grep -q "^toolchain arm-none-eabi 12.2.0 sha256:" heddle.lock \
        && ok "lock records toolchain hash" || bad "lock toolchain line"
    grep -q "^library freertos 10.5.1 sha256:" heddle.lock \
        && ok "lock records dependency hash" || bad "lock library line"

    $HEDDLE env verify >/dev/null 2>&1
    check_rc "env verify after install" $? 0

    rm -rf out .heddle/app.graph
    run_show $HEDDLE app
    check_rc "build with managed toolchain" $? 0
    check_eq "dependency linked in" "$(./out/app$EXE | head -1)" "10501"
    check_eq "managed compiler used" "$(./out/app$EXE | tail -1)" "managed"

    printf 'int freertos_version(void){return 0;}\n' \
        > .heddle/store/library/freertos/10.5.1/include/freertos.h
    $HEDDLE env verify >/dev/null 2>&1
    check_rc "env verify catches drift" $? 1

    out=$($HEDDLE -v tool install 2>&1)
    expect_err "install repairs drift" "$out" "repairing"
    $HEDDLE env verify >/dev/null 2>&1
    check_rc "env verify after repair" $? 0

    unset HEDDLE_REGISTRY
    rm -rf out .heddle/app.graph
    run_show $HEDDLE app
    check_rc "offline build (no registry)" $? 0
    check_eq "offline run" "$(./out/app$EXE | head -1)" "10501"

    rm -rf .heddle
    out=$($HEDDLE --offline tool install 2>&1)
    check_rc "offline cold install fails" $? 1
    expect_err "offline message" "$out" "offline"

    rm -rf .heddle
    $HEDDLE env verify >/dev/null 2>&1
    check_rc "verify without lock fails" $? 1

    rm -rf out .heddle heddle.lock
    unset HEDDLE_REGISTRY
    echo

    echo "pkg (tarball registry):"
    TARREG="$HERE/pkg/registry-targz"
    rm -rf "$TARREG"
    mkdir -p "$TARREG/library/freertos"
    ( cd "$HERE/pkg/registry/library/freertos/10.5.1" \
      && tar czf "$TARREG/library/freertos/10.5.1.tar.gz" . )
    mkdir -p "$TARREG/toolchain/arm-none-eabi"
    ( cd "$HERE/pkg/registry/toolchain/arm-none-eabi/12.2.0" \
      && tar czf "$TARREG/toolchain/arm-none-eabi/12.2.0.tar.gz" . )
    rm -rf out .heddle heddle.lock
    ( cd "$HERE/pkg" \
      && HEDDLE_REGISTRY="$TARREG" $HEDDLE tool install >/dev/null 2>&1 )
    check_rc "install from tarball registry" $? 0
    [ -f "$HERE/pkg/.heddle/store/library/freertos/10.5.1/include/freertos.h" ] \
        && ok "tarball hoisted to store root" || bad "tarball wrapper not hoisted"
    ( cd "$HERE/pkg" \
      && HEDDLE_REGISTRY="$TARREG" $HEDDLE app >/dev/null 2>&1 )
    check_rc "build from tarball registry" $? 0
    check_eq "tarball dependency linked" "$(cd "$HERE/pkg" && ./out/app$EXE | head -1)" "10501"
    ( cd "$HERE/pkg" && $HEDDLE env verify >/dev/null 2>&1 )
    check_rc "verify tarball store" $? 0
    rm -rf "$TARREG" "$HERE/pkg/out" "$HERE/pkg/.heddle" "$HERE/pkg/heddle.lock"
    echo
fi
echo

echo "recipe (source package):"
cd "$HERE/recipe"
rm -rf out .heddle heddle.lock
export HEDDLE_REGISTRY="$HERE/recipe/registry"

$HEDDLE tool install >/dev/null 2>&1
check_rc "source package install" $? 0
[ -f .heddle/store/library/greet/1.0/package.toml ]     && ok "recipe copied to store" || bad "recipe missing from store"

rm -rf out .heddle/app.graph
run_show $HEDDLE app
check_rc "source package build" $? 0
check_eq "c + asm linked" "$(./out/app$EXE)" "102"

graph=$(cat .heddle/app.graph)
case "$graph" in
    *"gcc -c"*"greet.c"*) ok "c source compiled in graph" ;;
    *)                    bad "c source not in graph: $graph" ;;
esac
case "$graph" in
    *"nasm"*"level.asm"*) ok "asm source compiled in graph" ;;
    *)                    bad "asm source not in graph: $graph" ;;
esac
[ "$HOST_OS" = linux ] && {
    case "$graph" in
        *"nasm -f elf64"*) ok "asm uses elf64 on linux" ;;
        *)                 bad "asm format wrong: $graph" ;;
    esac
}
case "$graph" in
    *"ar rcs"*"libgreet.a"*) ok "recipe artifact archived in graph" ;;
    *)                       bad "recipe archive missing: $graph" ;;
esac

rm -rf out .heddle/app.graph
out=$($HEDDLE -v app 2>&1)

if [ "$HOST_OS" = linux ]; then
    expect_err "source package rebuild hits CAS" "$out" "5 cached"
else
    expect_err "source package rebuild hits CAS" "$out" "cached"
    case "$out" in
        *"0 cached"*) bad "no CAS hits at all: $out" ;;
        *)            ok "source package partially cached" ;;
    esac
fi

v1=$($HEDDLE -v app 2>&1 | grep -o 'variant=[^ ]*')
cp heddle.toml heddle.toml.orig
cat > heddle.toml <<EOF
[build]
dir = "out"

[toolchain.host]
cc = "cc"
cflags = ["-DVARIANT_PROBE=1"]

[target]
arch = "x86_64"

[dependencies]
greet = "1.0"

[target.app]
type = "exe"
src = ["src/main.c"]
EOF
v2=$($HEDDLE -v app 2>&1 | grep -o 'variant=[^ ]*')
mv heddle.toml.orig heddle.toml
[ -n "$v1" ] && [ "$v1" != "$v2" ] \
    && ok "variant changes with toolchain flags" \
    || bad "variant did not change ($v1 vs $v2)"

rm -rf out .heddle heddle.lock
unset HEDDLE_REGISTRY
echo

echo "src glob:"
cd "$HERE/glob"
rm -rf out .heddle

$HEDDLE app >/dev/null 2>&1
check_rc "glob build" $? 0
check_eq "glob build output" "$(./out/app$EXE)" "71"

graph=$(cat .heddle/app.graph)
case "$graph" in
    *"src/util/util.c"*) ok "src/*.c expanded" ;;
    *)                   bad "src/util/*.c not expanded: $graph" ;;
esac
case "$graph" in
    *"src/extra/sub/b.c"*) ok "** expanded into subdirs" ;;
    *)                     bad "src/extra/**/*.c not expanded: $graph" ;;
esac
case "$graph" in
    *"app/main.c"*) ok "app/*.c expanded" ;;
    *)              bad "app/*.c not expanded: $graph" ;;
esac

n=$(grep -c "o out/extra_a.o src/extra/a.c" .heddle/app.graph)
check_eq "overlapping globs dedup" "$n" "1"

rm -rf out .heddle
$HEDDLE app >/dev/null 2>&1
out=$($HEDDLE app -v 2>&1)
expect_err "glob noop rebuild" "$out" "0 ran"
rm -rf out .heddle
echo

if [ "$FAKE_TOOLS" = 0 ]; then
    echo "linker script (fake shell toolchain):"
    echo "  skip (needs a unix shell to fake iar/armcc)"
else
    echo "linker script (toolchain neutral):"
    cd "$HERE/linkconv"
    rm -rf out .heddle .linkspy
    export PATH="$HERE/linkconv/bin:$PATH"

    [ "$HOST_OS" = linux ] && {
        run_show $HEDDLE -t native kernel
        check_rc "gnu build" $? 0
    } || $HEDDLE -t native kernel >/dev/null 2>&1
    case "$(cat .heddle/kernel.graph)" in
        *"-T src/kernel.ld"*)   ok "gnu passes -T" ;;
        *)                      bad "gnu lost -T" ;;
    esac
    [ "$(wc -l < .heddle/kernel.graph)" = "2" ] \
        && ok "gnu links in one step" || bad "gnu added a conversion step"

    rm -rf out .heddle
    [ "$HOST_OS" = linux ] && {
        run_show $HEDDLE -t iar kernel
        check_rc "iar build" $? 0
    } || $HEDDLE -t iar kernel >/dev/null 2>&1
    graph=$(cat .heddle/kernel.graph)
    case "$graph" in
        *"ldconv src/kernel.ld iar"*) ok "iar converts .ld to .icf" ;;
        *)                            bad "iar did not convert: $graph" ;;
    esac
    case "$graph" in
        *"--config=out/kernel.icf"*)  ok "iar uses --config" ;;
        *)                            bad "iar missing --config: $graph" ;;
    esac
    case "$graph" in
        *"--entry=reset_handler"*)    ok "iar entry is semantic" ;;
        *)                            bad "iar entry missing: $graph" ;;
    esac
    case "$graph" in
        *"< 1 0"*) ok "link waits for script" ;;
        *)         bad "link does not depend on conversion: $graph" ;;
    esac
    grep -q "define region FLASH" out/kernel.icf \
        && ok "converted icf has regions" || bad "converted icf wrong"

    rm -rf out .heddle
    [ "$HOST_OS" = linux ] && {
        run_show $HEDDLE -t armcc kernel
        check_rc "armcc build" $? 0
    } || $HEDDLE -t armcc kernel >/dev/null 2>&1
    graph=$(cat .heddle/kernel.graph)
    case "$graph" in
        *"--scatter=out/kernel.sct"*) ok "armcc uses --scatter" ;;
        *)                            bad "armcc missing --scatter: $graph" ;;
    esac
    grep -q "^  FLASH 0x8000000" out/kernel.sct \
        && ok "scatter has region" || bad "scatter wrong"

    rm -rf out .heddle .linkspy
    unset PATH
    export PATH="$ORIG_PATH"
    echo
fi
echo

echo "install:"
cd "$HERE/inst"
rm -rf out .heddle stage
cp heddle.toml heddle.toml.inst
sed "s|out/kernel\*|out/kernel$EXE|" heddle.toml.inst > heddle.toml
$HEDDLE kernel >/dev/null 2>&1
$HEDDLE mylib >/dev/null 2>&1

out=$($HEDDLE install --destdir=./stage --dry-run 2>&1)
check_rc "install dry-run" $? 0
expect_err "dry-run lists bin" "$out" "bin/kernel"
expect_err "dry-run keeps include subtree" "$out" "include/net/mylib.h"
expect_err "dry-run maps share" "$out" "share/mylib/README.md"
expect_err "dry-run maps etc" "$out" "etc/mylib/app.conf"
[ -d stage ] && bad "dry-run wrote to disk" || ok "dry-run leaves no files"

$HEDDLE install --destdir=./stage >/dev/null 2>&1
check_rc "install" $? 0

for f in bin/kernel$EXE lib/libmylib.a include/net/mylib.h \
         share/mylib/README.md etc/mylib/app.conf \
         rootfs/usr/bin/app rootfs/etc/app.conf; do
    [ -f "stage/opt/myos/$f" ] && ok "installed $f" || bad "missing $f"
done

rm -rf stage2 .heddle
$HEDDLE install mylib --destdir=./stage2 >/dev/null 2>&1
check_rc "install single target" $? 0
[ -f stage2/opt/myos/lib/libmylib.a ] && ok "single target lib" || bad "single target lib"
[ ! -f "stage2/opt/myos/bin/kernel$EXE" ] && ok "single target skips others" || bad "single target leaked"

cp heddle.toml heddle.notprefix.toml
grep -v '^prefix' heddle.notprefix.toml | grep -v '^\[install\]' > heddle.toml
out=$($HEDDLE install --dry-run 2>&1)
check_rc "missing prefix is refused" $? 1
expect_err "missing prefix message" "$out" "no install prefix"
mv heddle.notprefix.toml heddle.toml

rm -rf stage .heddle
$HEDDLE install --destdir=./stage >/dev/null 2>&1
$HEDDLE install --destdir=./stage2 >/dev/null 2>&1
$HEDDLE uninstall --destdir=./stage >/dev/null 2>&1
check_rc "uninstall" $? 0
n=$(find stage -type f ! -path '*.dSYM*' 2>/dev/null | wc -l)
if [ "$n" = "0" ]; then
    ok "uninstall cleared its destdir"
else
    bad "uninstall left files"
    find stage -type f 2>/dev/null | head -20
fi
[ -f stage2/opt/myos/bin/kernel ] \
    && ok "uninstall left the other destdir" || bad "uninstall crossed destdirs"

rm -rf stage .heddle
cp heddle.toml heddle.keep.toml

cat > heddle.toml <<EOF
[build]
dir = "out"

[install]
prefix = "/opt/myos"

[target]
arch = "x86_64"
sysroot = "/opt/sysroot"

[target.kernel]
type = "exe"
src = ["src/kernel.c"]

[target.kernel.install]
bin = "out/kernel$EXE"

[target.mylib]
type = "staticlib"
src = ["src/mylib.c"]
inc = ["include"]

[target.mylib.install]
lib = "out/libmylib.a"
include = ["include/**/*.h"]
sysroot_lib = ["out/libmylib.a"]
sysroot_include = ["include/**/*.h"]
EOF

$HEDDLE install --destdir=./stage >/dev/null 2>&1
check_rc "sysroot install" $? 0
[ -f stage/opt/sysroot/lib/libmylib.a ]       && ok "sysroot lib" || bad "sysroot lib"
[ -f stage/opt/sysroot/include/net/mylib.h ]  && ok "sysroot include" || bad "sysroot include"
mv heddle.keep.toml heddle.toml
mv heddle.toml.inst heddle.toml

rm -rf out .heddle stage stage2
echo

echo "init:"
WORK=$(mktemp -d)
cd "$WORK"

out=$($HEDDLE init --list 2>&1)
check_rc "init --list" $? 0
expect_err "lists exe" "$out" "exe"
expect_err "lists embedded" "$out" "embedded"

out=$($HEDDLE init bogus 2>&1)
check_rc "unknown type rejected" $? 2
expect_err "unknown type message" "$out" "unknown project type"

$HEDDLE init exe exe1 </dev/null >/dev/null 2>&1
check_rc "init exe" $? 0
[ -f exe1/heddle.toml ] && [ -f exe1/src/main.c ] \
    && ok "exe files created" || bad "exe files missing"
( cd exe1 && $HEDDLE app >/dev/null 2>&1 )
check_rc "generated exe builds" $? 0
check_eq "generated exe runs" "$(cd exe1 && ./out/app$EXE)" "hello"
( cd exe1 && $HEDDLE check >/dev/null 2>&1 )
check_rc "generated exe checks" $? 0

$HEDDLE init lib lib1 </dev/null >/dev/null 2>&1
( cd lib1 && $HEDDLE mylib >/dev/null 2>&1 )
check_rc "generated lib builds" $? 0
[ -f lib1/include/mylib.h ] && ok "lib header created" || bad "lib header missing"

$HEDDLE init exe star1 --star </dev/null >/dev/null 2>&1
check_rc "init exe --star" $? 0
[ -f star1/heddle.star ] && ok "star manifest created" || bad "no heddle.star"
[ ! -f star1/heddle.toml ] && ok "no toml when --star" || bad "toml written too"
( cd star1 && $HEDDLE app >/dev/null 2>&1 )
check_rc "generated star builds" $? 0
check_eq "generated star runs" "$(cd star1 && ./out/app$EXE)" "hello"

$HEDDLE init lib starlib --star </dev/null >/dev/null 2>&1
( cd starlib && $HEDDLE mylib >/dev/null 2>&1 )
check_rc "generated star lib builds" $? 0

$HEDDLE init embedded fw1 --arch=riscv32imac </dev/null >/dev/null 2>&1
check_rc "init embedded" $? 0
grep -q 'arch = "riscv32imac"' fw1/heddle.toml \
    && ok "embedded arch recorded" || bad "embedded arch wrong"
( cd fw1 && $HEDDLE check >/dev/null 2>&1 )
check_rc "generated embedded checks" $? 0
cprefix=$(cd fw1 && $HEDDLE check 2>&1)
case "$cprefix" in
    *riscv64-unknown-elf*) ok "embedded derives cross prefix" ;;
    *)                     bad "embedded prefix wrong: $cprefix" ;;
esac

out=$($HEDDLE init embedded fw2 --arch=nope 2>&1)
check_rc "bad arch rejected" $? 2
expect_err "bad arch message" "$out" "unknown arch"

if command -v riscv64-unknown-elf-gcc >/dev/null 2>&1; then
    $HEDDLE init embedded fw3 --arch=riscv32imac </dev/null >/dev/null 2>&1
    ( cd fw3 && $HEDDLE firmware >/dev/null 2>&1 )
    check_rc "riscv32 link carries -march/-mabi" $? 0
    if [ -f fw3/out/firmware ]; then
        rclass=$(riscv64-unknown-elf-readelf -h fw3/out/firmware |
            awk '/Class:/ {print $2}')
        check_eq "riscv32 firmware is ELF32" "$rclass" "ELF32"
    fi
else
    echo "  skip (riscv32 firmware link needs riscv64-unknown-elf-gcc)"
fi

if [ "$M32" = 0 ]; then
    echo "  skip (baremetal template needs 32-bit multilib)"
elif command -v nasm >/dev/null 2>&1; then
    $HEDDLE init baremetal os1 </dev/null >/dev/null 2>&1
    ( cd os1 && $HEDDLE image >/dev/null 2>&1 )
    check_rc "generated baremetal builds" $? 0
    sig=$(xxd -s 510 -l 2 -p os1/out/mbr.bin 2>/dev/null)
    check_eq "generated boot signature" "$sig" "55aa"
else
    echo "  skip (nasm not found)"
fi

out=$($HEDDLE init exe exe1 </dev/null 2>&1)
expect_err "existing files skipped" "$out" "exists"
out=$($HEDDLE init exe exe1 --force </dev/null 2>&1)
expect_err "force overwrites" "$out" "create"

rm -rf "$WORK"
echo

echo "vcpkg:"
cd "$HERE/vcpkg"
rm -rf proj/.heddle proj/out proj/heddle.lock installed

mkdir -p installed/x64-linux/include installed/x64-linux/lib installed/x64-linux/share
cp payload/zlib.h installed/x64-linux/include/
cc -c payload/zlib.c -Ipayload -o installed/x64-linux/lib/zlib.o 2>/dev/null
ar rcs installed/x64-linux/lib/libz.a installed/x64-linux/lib/zlib.o
rm -f installed/x64-linux/lib/zlib.o

out=$($HEDDLE vcpkg show ports/zlib 2>&1)
check_rc "vcpkg show" $? 0
expect_err "show reads name" "$out" "zlib"
expect_err "show reads version" "$out" "1.3.1"
expect_err "show reads deps" "$out" "minizip"

out=$($HEDDLE vcpkg triplet thumbv7em-none-eabihf 2>&1)
check_rc "triplet maps" $? 0
expect_err "triplet arch" "$out" 'arch = "armv7em"'
expect_err "triplet float" "$out" 'float = "hard"'

$HEDDLE vcpkg check . >/dev/null 2>&1
check_rc "vcpkg registry detected" $? 0
$HEDDLE vcpkg check proj >/dev/null 2>&1
check_rc "non-registry rejected" $? 1

out=$($HEDDLE vcpkg triplet nope 2>&1)
check_rc "unknown triplet rejected" $? 1
expect_err "unknown triplet message" "$out" "unknown triplet"

cd proj
$HEDDLE vcpkg import ../ports/zlib --from ../installed --triplet x64-linux >/dev/null 2>&1
check_rc "vcpkg import" $? 0
[ -f .heddle/store/library/zlib/1.3.1/include/zlib.h ] \
    && ok "imported include" || bad "imported include missing"
[ -f .heddle/store/library/zlib/1.3.1/lib/libz.a ] \
    && ok "imported lib" || bad "imported lib missing"
[ -f heddle.lock ] && ok "import wrote lock" || bad "import lock missing"
grep -q "^library zlib 1.3.1 sha256:" heddle.lock \
    && ok "lock records vcpkg package" || bad "lock line wrong"

$HEDDLE env verify >/dev/null 2>&1
check_rc "verify imported package" $? 0

$HEDDLE app >/dev/null 2>&1
check_rc "build against imported package" $? 0
check_eq "linked against real lib name" "$(./out/app$EXE)" "131"
grep -q '\-lz ' .heddle/app.graph || grep -q '\-lz$' .heddle/app.graph \
    && ok "uses libz.a name not port name" || bad "link name wrong"

rm -rf .heddle heddle.lock out
out=$(HEDDLE_REGISTRY="$HERE/vcpkg" $HEDDLE tool install 2>&1)
check_rc "registry install without build is refused" $? 1
expect_err "registry hint" "$out" "vcpkg import"

cd "$HERE/vcpkg"
rm -rf installed proj/.heddle proj/out proj/heddle.lock
echo

echo "migrate:"

cd "$HERE/migrate"
rm -f heddle.toml
rm -rf out .heddle

out=$($HEDDLE migrate . --dry-run 2>&1)
check_rc "cmake dry-run" $? 0
expect_err "dry-run lists targets" "$out" "3 targets"
[ -f heddle.toml ] && bad "dry-run wrote a file" || ok "dry-run writes nothing"

$HEDDLE migrate . >/dev/null 2>&1
check_rc "cmake migrate" $? 0
[ -f heddle.toml ] && ok "wrote heddle.toml" || bad "no heddle.toml"
grep -q "^\[target.app\]" heddle.toml      && ok "target section" || bad "no target"
grep -q 'type = "staticlib"' heddle.toml   && ok "static kind mapped" || bad "kind wrong"
grep -q 'deps = \["math", "util"\]' heddle.toml \
    && ok "link_libraries -> deps" || bad "deps wrong"
grep -q '"-DLEVEL=3"' heddle.toml          && ok "compile_definitions -> cflags" || bad "defines wrong"

$HEDDLE check >/dev/null 2>&1
check_rc "migrated cmake checks" $? 0
$HEDDLE app >/dev/null 2>&1
check_rc "migrated cmake builds" $? 0
check_eq "migrated cmake runs" "$(./out/app$EXE)" "42"

rm -f heddle.toml heddle.star
rm -rf out .heddle
$HEDDLE migrate . --star >/dev/null 2>&1
check_rc "migrate --star" $? 0
grep -q "target(" heddle.star && ok "star output has target()" || bad "no target() in star"
$HEDDLE app >/dev/null 2>&1
check_rc "migrated star builds" $? 0
check_eq "migrated star runs" "$(./out/app$EXE)" "42"
rm -f heddle.star
rm -rf out .heddle
$HEDDLE migrate . >/dev/null 2>&1

out=$($HEDDLE migrate . 2>&1)
check_rc "existing manifest refused" $? 1
expect_err "clobber message" "$out" "exists"

cd "$HERE/migrate_xm"
rm -f heddle.toml
rm -rf out .heddle

out=$($HEDDLE migrate . --dry-run 2>&1)
check_rc "xmake dry-run" $? 0
expect_err "xmake lists targets" "$out" "3 targets"

$HEDDLE migrate . >/dev/null 2>&1
check_rc "xmake migrate" $? 0
grep -q 'deps = \["math", "util"\]' heddle.toml \
    && ok "add_deps -> deps" || bad "xmake deps wrong"
grep -q '"-DLEVEL=3"' heddle.toml \
    && ok "add_defines -> cflags" || bad "xmake defines wrong"
grep -q '"-Wall"' heddle.toml \
    && ok "add_cxflags -> cflags" || bad "xmake cflags wrong"

$HEDDLE app >/dev/null 2>&1
check_rc "migrated xmake builds" $? 0
check_eq "migrated xmake runs" "$(./out/app$EXE)" "42"

cd "$HERE/migrate_edge"
rm -f heddle.toml
rm -rf out .heddle

$HEDDLE migrate . --from cmake >/dev/null 2>&1
check_rc "cmake -T migrate" $? 0
grep -q 'linker_script = "kernel.ld"' heddle.toml \
    && ok "-T -> linker_script" || bad "-T not mapped"
grep -q '"-lpthread"' heddle.toml \
    && ok "system lib -> -lflag" || bad "system lib wrong"

rm -f heddle.toml
$HEDDLE migrate . --from xmake >/dev/null 2>&1
check_rc "xmake -T migrate" $? 0
grep -q 'linker_script = "kernel.ld"' heddle.toml \
    && ok "xmake -T -> linker_script" || bad "xmake -T not mapped"
grep -q '"-lpthread"' heddle.toml \
    && ok "xmake syslinks -> -lflag" || bad "xmake syslinks wrong"

out=$($HEDDLE migrate . --from nope 2>&1)
check_rc "bad --from rejected" $? 2
expect_err "bad --from message" "$out" "unknown --from"

out=$($HEDDLE migrate /nonexistent 2>&1)
check_rc "missing source refused" $? 1
expect_err "missing source message" "$out" "no"

rm -f heddle.toml
rm -rf out .heddle
echo

echo "star:"

cd "$HERE/star"
rm -rf out .heddle

$HEDDLE check >/dev/null 2>&1
check_rc "star project checks" $? 0

$HEDDLE app >/dev/null 2>&1
check_rc "star project builds" $? 0
check_eq "star project runs" "$(./out/app$EXE)" "42"

grep -q "src/util.c" .heddle/app.graph \
    && ok "def/slice generated util" || bad "util not in graph"
grep -q "libmath.a" .heddle/app.graph \
    && ok "math target built" || bad "math missing"
grep -q -- "-DLEVEL=3" .heddle/app.graph \
    && ok "target cflags applied" || bad "cflags missing"

cp heddle.star heddle.star.good

cat > heddle.star <<'EOF'
build(dir = "out")
target(name = "bad", type = "exe", src = ["src/nope.c"])
EOF
out=$($HEDDLE check 2>&1)
check_rc "missing source rejected" $? 1
expect_err "missing source message" "$out" "missing source"

cat > heddle.star <<'EOF'
build(dir = "out")
target(name = "t", type = &&&)
EOF
out=$($HEDDLE check 2>&1)
check_rc "parse error rejected" $? 1
expect_err "parse error has line" "$out" "heddle.star:2"

cat > heddle.star <<'EOF'
build(dir = "out")
i = 0
while i < 3:
    i = i + 1
EOF
out=$($HEDDLE check 2>&1)
check_rc "while rejected" $? 1
expect_err "while message" "$out" "not supported"

cp heddle.star.good heddle.star
rm -f heddle.star.good
rm -rf out .heddle

cd "$HERE/star_full"
rm -rf out .heddle

out=$($HEDDLE check 2>&1)
check_rc "star full checks" $? 0
expect_err "two boards generated" "$out" "fw_nucleo"
expect_err "second board" "$out" "fw_bluepill"

out=$($HEDDLE tool plan 2>&1)
check_rc "star tool plan" $? 0
expect_err "target_config arch" "$out" "armv7em"
expect_err "derived prefix" "$out" "arm-none-eabi-"
expect_err "package listed" "$out" "freertos"

rm -rf out .heddle

cd "$HERE/star_install"
rm -rf out .heddle stage
cp heddle.star heddle.star.orig
sed "s|out/app\*|out/app$EXE|" heddle.star.orig > heddle.star
b=$($HEDDLE app 2>&1) || echo "$b"
b=$($HEDDLE mylib 2>&1) || echo "$b"
out=$($HEDDLE install --destdir=./stage --prefix=opt/x 2>&1)
rc=$?
check_rc "star install" $rc 0
[ $rc -eq 0 ] || echo "$out"
[ -f "stage/opt/x/bin/app$EXE" ]    && ok "star install bin" || bad "star install bin"
[ -f stage/opt/x/lib/libmylib.a ]  && ok "star install lib" || bad "star install lib"
[ -f stage/opt/x/include/mylib.h ] && ok "star install include" || bad "star install include"
mv heddle.star.orig heddle.star
rm -rf out .heddle stage

cd "$HERE/star_platform"
rm -rf out .heddle

out=$($HEDDLE check 2>&1)
check_rc "platform project checks" $? 0
expect_err "loop made both boards" "$out" "fw_nucleo"
expect_err "second board" "$out" "fw_bluepill"

if [ "$FAKE_TOOLS" = 0 ]; then
    echo "  skip (fake arm-none-eabi-gcc needs a unix shell)"
else
    mkdir -p .heddle/store/library/freertos/10.5.1/lib
    mkdir -p store_tmp
    ( cd store_tmp && ar rcs ../.heddle/store/library/freertos/10.5.1/lib/libfreertos.a )
    rmdir store_tmp 2>/dev/null || true

    export PATH="$HERE/star_platform/bin:$PATH"
    $HEDDLE fw_nucleo >/dev/null 2>&1
    check_rc "build fw_nucleo" $? 0
    grep -q "cortex-m4" .heddle/fw_nucleo.graph \
        && ok "nucleo uses armv7em" || bad "nucleo arch wrong"
    grep -q -- "-mfloat-abi=hard" .heddle/fw_nucleo.graph \
        && ok "nucleo hard float" || bad "nucleo float wrong"

    $HEDDLE fw_bluepill >/dev/null 2>&1
    check_rc "build fw_bluepill" $? 0
    grep -q "cortex-m3" .heddle/fw_bluepill.graph \
        && ok "bluepill uses armv7m" || bad "bluepill arch wrong"
    grep -q -- "-mfloat-abi=soft" .heddle/fw_bluepill.graph \
        && ok "bluepill soft float" || bad "bluepill float wrong"

    grep -q "arm-none-eabi-gcc" .heddle/fw_nucleo.graph \
        && ok "star toolchain cc used" || bad "toolchain cc not used"

    out=$($HEDDLE tool plan 2>&1)
    expect_err "package object registered" "$out" "freertos"
    check_rc "package does not break check" $? 0
fi

unset PATH
export PATH="$ORIG_PATH"
rm -rf out .heddle

cd "$HERE/star"
rm -rf out .heddle
cp heddle.star heddle.star.good
cat > heddle.toml <<'EOF'
[build]
dir = "out"
[target.toml_only]
type = "exe"
src = ["src/util.c"]
EOF
out=$($HEDDLE check 2>&1)
expect_err "star overrides toml" "$out" "app"
case "$out" in
    *toml_only*) bad "toml used when star present" ;;
    *)           ok "toml ignored when star present" ;;
esac
rm -f heddle.toml
out=$($HEDDLE check 2>&1)
check_rc "toml fallback checks" $? 0
expect_err "toml fallback selected" "$out" "app"
cp heddle.star.good heddle.star
rm -f heddle.star.good
rm -rf out .heddle
echo

echo "compdb:"

cd "$HERE/basic"
rm -rf out .heddle compile_commands.json

$HEDDLE --compile-db app >/dev/null 2>&1
check_rc "compile-db" $? 0
[ -f compile_commands.json ] && ok "wrote compile_commands.json" || bad "no db file"

if command -v python3 >/dev/null 2>&1; then
    python3 - <<'PYEOF' >/tmp/cdb_check.$$
import json
d = json.load(open("compile_commands.json"))
assert len(d) == 3, len(d)
for e in d:
    assert set(e) == {"directory", "file", "command", "output"}, e
    assert e["directory"].startswith("/") or e["directory"][1:3] == ":/", e["directory"]
    assert "-c" in e["command"], e["command"]
print("ok")
PYEOF
    [ "$(cat /tmp/cdb_check.$$)" = "ok" ] \
        && ok "db is valid clang json" || bad "db json wrong"
    rm -f /tmp/cdb_check.$$
else
    echo "  skip (no python3)"
fi

grep -q "src/util/util.c" compile_commands.json \
    && ok "covers dependency target" || bad "missing dep source"
grep -q "app/main.c" compile_commands.json \
    && ok "covers root target" || bad "missing root source"

rm -f compile_commands.json
$HEDDLE --compile-db util >/dev/null 2>&1
python3 -c "import json; d=json.load(open('compile_commands.json')); assert len(d)==1, len(d)" 2>/dev/null \
    && ok "single target closure" || bad "closure wrong"

rm -f compile_commands.json db.json
$HEDDLE --compile-db=db.json app >/dev/null 2>&1
[ -f db.json ] && [ ! -f compile_commands.json ] \
    && ok "custom filename" || bad "custom filename not honored"

rm -f db.json
$HEDDLE --compile-db >/dev/null 2>&1
python3 -c "import json; assert len(json.load(open('compile_commands.json')))==3" 2>/dev/null \
    && ok "all targets mode" || bad "all mode wrong"

cd "$HERE/os"
rm -rf out .heddle compile_commands.json
$HEDDLE --compile-db image >/dev/null 2>&1
check_rc "os compile-db" $? 0
grep -q "nasm" compile_commands.json \
    && ok "assembler in db" || bad "assembler missing"
grep -q '"file": "src/kernel.c"' compile_commands.json \
    && ok "c in db" || bad "c missing"
! grep -q "os.img" compile_commands.json \
    && ok "custom target excluded" || bad "custom leaked"

rm -f compile_commands.json
cd "$HERE/basic"
rm -f compile_commands.json
rm -rf out .heddle
echo

echo "test:"

cd "$HERE/test"
rm -rf out .heddle

out=$($HEDDLE test tmath 2>&1)
check_rc "single test passes" $? 0
expect_err "single test reports ok" "$out" "1 passed, 0 failed"

out=$($HEDDLE test 2>&1)
check_rc "suite reports failure" $? 1
expect_err "suite counts" "$out" "2 passed, 1 failed"
expect_err "failing case named" "$out" "FAIL (exit 3, want 0)"

out=$($HEDDLE -v test targs 2>&1)
check_rc "args test passes" $? 0
expect_err "args quoted" "$out" "hello world"

$HEDDLE test nosuch >/dev/null 2>&1
[ $? -ne 0 ] && ok "unknown target rejected" || bad "unknown target accepted"

rm -rf out .heddle
echo

echo "vcpkg auto:"

cd "$HERE/vcpkg"
rm -rf proj/.heddle proj/out proj/heddle.lock installed

if [ -n "$EXE" ]; then
    echo "  skip (vcpkg auto-discovery stub needs a POSIX shell)"
    echo
else
VTRIP=x64-linux

mkdir -p "installed/$VTRIP/include" "installed/$VTRIP/lib" "installed/$VTRIP/bin"
cp payload/zlib.h "installed/$VTRIP/include/"
cc -c payload/zlib.c -Ipayload -o "installed/$VTRIP/lib/zlib.o" 2>/dev/null
ar rcs "installed/$VTRIP/lib/libz.a" "installed/$VTRIP/lib/zlib.o"
rm -f "installed/$VTRIP/lib/zlib.o"

mkdir -p fakebin
printf '#!/bin/sh\necho vcpkg\n' > fakebin/vcpkg
chmod +x fakebin/vcpkg

cd proj
PATH="$HERE/vcpkg/fakebin:$PATH" $HEDDLE tool install -v >auto.log 2>&1
check_rc "vcpkg auto install" $? 0
grep -q "vcpkg root" auto.log && ok "vcpkg root discovered" || bad "vcpkg root not found"
[ -f .heddle/store/library/zlib/1.3.1/include/zlib.h ] \
    && ok "vcpkg auto imported" || bad "vcpkg auto import failed"

rm -rf out .heddle/loom* .heddle/*.graph
PATH="$HERE/vcpkg/fakebin:$PATH" $HEDDLE app >/dev/null 2>&1
check_rc "vcpkg auto build" $? 0
grep -q -- "-lz" .heddle/app.graph && ok "vcpkg libs linked" || bad "vcpkg libs missing"

rm -rf out .heddle auto.log
cd "$HERE/vcpkg"
rm -rf installed fakebin proj/.heddle proj/out proj/heddle.lock
fi
echo

echo "pkgconfig:"

cd "$HERE/pkgconfig"
rm -rf out .heddle

if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists zlib 2>/dev/null; then
    out=$($HEDDLE app 2>&1)
    rc=$?
    check_rc "pkgconfig build" $rc 0
    [ $rc -eq 0 ] || printf '%s\n' "$out" | sed 's/^/    /' 
    grep -q -- "-lz" .heddle/app.graph \
        && ok "pkgconfig libs" || bad "pkgconfig libs missing"
    ./out/app$EXE 2>/dev/null
    check_rc "pkgconfig run" $? 0

    rm -rf out .heddle
    out=$($HEDDLE check 2>&1)
    check_rc "pkgconfig check" $? 0
else
    echo "  skip (needs pkg-config + zlib)"
fi

rm -rf out .heddle
echo

echo "watch:"

cd "$HERE/watch"
rm -rf out .heddle watch.log

"$HEDDLE" watch --interval 0.2 app >watch.log 2>&1 &
WPID=$!

for _ in $(seq 1 50); do
    grep -q "heddle: ok" watch.log 2>/dev/null && break
    sleep 0.2
done

grep -q "heddle: ok" watch.log && ok "initial build" || bad "no initial build"

sleep 0.5
printf '#include "val.h"\nint val(void){return 8;}\n' > src/lib.c

for _ in $(seq 1 50); do
    [ "$(grep -c 'change detected' watch.log 2>/dev/null)" -ge 1 ] && break
    sleep 0.2
done

grep -q "change detected" watch.log && ok "change detected" || bad "change not detected"

sleep 0.5
printf '#define WVAL 9\n' > src/val.h

for _ in $(seq 1 50); do
    [ "$(grep -c 'change detected' watch.log 2>/dev/null)" -ge 2 ] && break
    sleep 0.2
done

[ "$(grep -c 'change detected' watch.log 2>/dev/null)" -ge 2 ] \
    && ok "header change detected" || bad "header change missed"

kill "$WPID" 2>/dev/null
wait "$WPID" 2>/dev/null

rm -rf out .heddle watch.log
echo

printf '总计: %d passed, %d failed\n' "$pass" "$fail"

[ "$fail" -eq 0 ]
