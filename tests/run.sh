#!/bin/bash
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

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
printf '总计: %d passed, %d failed\n' "$pass" "$fail"

[ "$fail" -eq 0 ]
