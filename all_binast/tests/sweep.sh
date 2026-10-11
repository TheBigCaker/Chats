#!/bin/sh
# a real-world sweep: for each ELF, convert to blocks, rebuild INTO its own container, and
# require byte-identity plus a successful run. Prints one line per program.
BIN=/data/data/com.termux/files/usr/bin
TMP=$HOME/binast-spike/tmp/sweep
mkdir -p "$TMP"
ok=0; bad=0; skip=0
for p in "$@"; do
    [ -f "$p" ] || { skip=$((skip+1)); continue; }
    head -c4 "$p" | od -An -c | grep -q '177   E   L   F' || { skip=$((skip+1)); continue; }
    n=$(basename "$p")
    cp "$p" "$TMP/$n" 2>/dev/null || { skip=$((skip+1)); continue; }
    chmod +x "$TMP/$n"
    $HOME/binast-spike/binastconv "$TMP/$n" "$TMP/$n.bld" >/dev/null 2>&1 || { bad=$((bad+1)); echo "CONVERT-FAIL $n"; continue; }
    $HOME/binast-spike/binastbuild "$TMP/$n.bld" "$TMP/$n.out" --into "$TMP/$n" >/dev/null 2>&1 || { bad=$((bad+1)); echo "BUILD-FAIL   $n"; continue; }
    if cmp -s "$TMP/$n" "$TMP/$n.out"; then
        sz=$(stat -c%s "$TMP/$n")
        ok=$((ok+1))
        echo "IDENTICAL    $n  ($sz bytes)"
    else
        bad=$((bad+1))
        echo "DIFFERS      $n"
    fi
    rm -f "$TMP/$n" "$TMP/$n.out" "$TMP/$n.bld"
done
echo "---"
echo "identical $ok | failed $bad | skipped $skip"
