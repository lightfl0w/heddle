#!/bin/bash
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)

HEDDLE=${HEDDLE:-$ROOT/build/linux/x86_64/release/heddle}

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

$HEDDLE -C . -j4 app >/dev/null 2>&1
check_rc "cold build" $? 0
check_eq "app output" "$(./out/app)" "42"

$HEDDLE -C . -j4 app >/dev/null 2>&1
check_rc "noop rebuild" $? 0

sleep 1
printf 'int util_id(void) {\n    return 2;\n}\n' > src/util/util.c
out=$($HEDDLE -C . -j4 -v app 2>&1)
check_rc "rebuild after edit" $? 0
check_eq "app output after edit" "$(./out/app)" "43"

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
check_eq "app output" "$(./out/app)" "6"

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
[ "$fail" -eq 0 ]

echo "os:"
cd "$HERE/os"
rm -rf out .heddle

if command -v nasm >/dev/null 2>&1; then
    $HEDDLE image >/dev/null 2>&1
    check_rc "os image build" $? 0

    sig=$(xxd -s 510 -l 2 -p out/mbr.bin 2>/dev/null)
    check_eq "boot signature" "$sig" "55aa"

    check_eq "kernel is ELF32" \
        "$(readelf -h out/kernel 2>/dev/null | awk '/Class:/ {print $2}')" "ELF32"

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
$HEDDLE app >/dev/null 2>&1
check_rc "build with managed toolchain" $? 0
check_eq "dependency linked in" "$(./out/app | head -1)" "10501"
check_eq "managed compiler used" "$(./out/app | tail -1)" "managed"

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
$HEDDLE app >/dev/null 2>&1
check_rc "offline build (no registry)" $? 0
check_eq "offline run" "$(./out/app | head -1)" "10501"

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
check_eq "tarball dependency linked" "$(cd "$HERE/pkg" && ./out/app | head -1)" "10501"
( cd "$HERE/pkg" && $HEDDLE env verify >/dev/null 2>&1 )
check_rc "verify tarball store" $? 0
rm -rf "$TARREG" "$HERE/pkg/out" "$HERE/pkg/.heddle" "$HERE/pkg/heddle.lock"
echo

echo "recipe (source package):"
cd "$HERE/recipe"
rm -rf out .heddle heddle.lock
export HEDDLE_REGISTRY="$HERE/recipe/registry"

$HEDDLE tool install >/dev/null 2>&1
check_rc "source package install" $? 0
[ -f .heddle/store/library/greet/1.0/package.toml ]     && ok "recipe copied to store" || bad "recipe missing from store"

rm -rf out .heddle/app.graph
$HEDDLE app >/dev/null 2>&1
check_rc "source package build" $? 0
check_eq "c + asm linked" "$(./out/app)" "102"

graph=$(cat .heddle/app.graph)
case "$graph" in
    *"gcc -c"*"greet.c"*) ok "c source compiled in graph" ;;
    *)                    bad "c source not in graph: $graph" ;;
esac
case "$graph" in
    *"nasm -f elf64"*"level.asm"*) ok "asm source compiled in graph (elf64)" ;;
    *)                             bad "asm source not in graph: $graph" ;;
esac
case "$graph" in
    *"ar rcs"*"libgreet.a"*) ok "recipe artifact archived in graph" ;;
    *)                       bad "recipe archive missing: $graph" ;;
esac

rm -rf out .heddle/app.graph
out=$($HEDDLE -v app 2>&1)
expect_err "source package rebuild hits CAS" "$out" "5 cached"

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
check_eq "glob build output" "$(./out/app)" "71"

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

printf '总计: %d passed, %d failed\n' "$pass" "$fail"
