#!/usr/bin/env bash
# Generate MicroPython's embed sources for the Python app into apps/python/mp,
# from apps/python/mpconfigport.h. Needs Linux (make, a host C compiler,
# python3, git); Windows has none of them, so from there:
#
#     HOST=root@157.245.252.47 bash tools/make_micropython.sh
#
# runs it on the droplet over ssh and copies the result back. Run it again
# after changing mpconfigport.h: the interned strings depend on it.
set -euo pipefail

TAG=v1.24.1
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CONF="$ROOT/apps/python/mpconfigport.h"
OUT="$ROOT/apps/python/mp"

generate='
set -e
W=/tmp/cardos-mpy
[ -d $W/mpy ] || git clone -q --depth 1 --branch '"$TAG"' https://github.com/micropython/micropython.git $W/mpy
rm -rf $W/out && mkdir -p $W/out && tar xzf /tmp/cardos-pyconf.tgz -C $W/out
cd $W/out
cp cardpy_mod.h cardpy_scan.c   # the scanner reads only .c files
printf "MICROPYTHON_TOP = $W/mpy\nSRC_QSTR += cardpy_scan.c\ninclude \$(MICROPYTHON_TOP)/ports/embed/embed.mk\n" > micropython_embed.mk
make -s -f micropython_embed.mk >/dev/null
cp $W/mpy/LICENSE micropython_embed/
tar czf /tmp/cardos-mpembed.tgz -C micropython_embed .
'

# The config and the headers it includes (cardpy_hal.h): every .h beside it.
# cardpy_mod.h is also scanned (SRC_QSTR) for the names and modules it adds.
tar czf /tmp/cardos-pyconf.tgz -C "$(dirname "$CONF")" $(cd "$(dirname "$CONF")" && ls *.h)
if [ -n "${HOST:-}" ]; then
  scp -q /tmp/cardos-pyconf.tgz "$HOST:/tmp/cardos-pyconf.tgz"
  ssh "$HOST" "$generate"
  scp -q "$HOST:/tmp/cardos-mpembed.tgz" /tmp/cardos-mpembed.tgz
else
  bash -c "$generate"
fi
rm -rf "$OUT" && mkdir -p "$OUT"
tar xzf /tmp/cardos-mpembed.tgz -C "$OUT"
echo "apps/python/mp: MicroPython $TAG, $(find "$OUT" -name '*.c' | wc -l) sources"
