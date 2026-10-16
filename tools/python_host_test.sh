#!/usr/bin/env bash
# The Python app's REPL test (apps/python/test/host_repl.c) on Linux with gcc:
# the app, MicroPython's core and the fake CardApi, built for the host. From
# Windows, which has no gcc:
#
#     HOST=root@157.245.252.47 bash tools/python_host_test.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FILES="apps/python apps/termlog.h kernel/app/capp.h test/fakeapi.c test/fakeapi.h"
build='
set -e
cd /tmp/cardos-pytest
gcc -std=gnu99 -O1 -g -w -I. -Iapps/python -Iapps/python/mp -DNDEBUG -DSTACK_LIMIT=40000 \
  -o host_repl apps/python/test/host_repl.c apps/python/mp/py/*.c test/fakeapi.c -lm
./host_repl
'
tar czf /tmp/cardos-pytest.tgz -C "$ROOT" $FILES
if [ -n "${HOST:-}" ]; then
  scp -q /tmp/cardos-pytest.tgz "$HOST:/tmp/cardos-pytest.tgz"
  ssh "$HOST" "rm -rf /tmp/cardos-pytest && mkdir /tmp/cardos-pytest && tar xzf /tmp/cardos-pytest.tgz -C /tmp/cardos-pytest && $build"
else
  rm -rf /tmp/cardos-pytest && mkdir /tmp/cardos-pytest && tar xzf /tmp/cardos-pytest.tgz -C /tmp/cardos-pytest
  bash -c "$build"
fi
