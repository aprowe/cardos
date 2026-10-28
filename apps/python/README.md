# Python

_Moved from CLAUDE.md on 2026-10-27._

**Python: MicroPython 1.24.1 as an app** (2026-10-16, `apps/python/`,
Make). A REPL (history, tab completion, auto-indent) whose heap is
big-mode blocks (`CAPP_BIG | CAPP_BIG_NO_NET`, up to 112 KB as a split
heap); calls are stackless and the C stack is checked (3 KB of the
shell's 8). A key held down is a KeyboardInterrupt (`cardpy_poll`, every
400 bytecodes). The owner's loop is edit, run, edit again: Vim's `:run`
saves and `api->run("python", PATH)`.
- **Built how**: MicroPython's embed sources are generated into
  `apps/python/mp` from `apps/python/mpconfigport.h` by
  `apps/python/tools/make_micropython.sh`, which needs Linux -- from Windows, `HOST=
  root@157.245.252.47 bash apps/python/tools/make_micropython.sh`. Run it again after
  any change to mpconfigport.h **or to a name or module in
  `cardpy_mod.h`**: the interned strings and the module table are scanned
  from it. `tools/build_apps.py` (MULTI) links the app against the
  toolchain's libc/libm/libgcc and still refuses an unresolved symbol.
  `python.c` is the app; `cardpy_mod.h` (a .h only so build_apps does not
  take it for an app; python.c includes it) is the port: open, import,
  os, time, sys.stdout, exit.
- **python FILE** runs it as `__main__` in the REPL's own globals, says
  `-- ran NAME in N ms` (or `exited (code)`, or `with an error`), and
  stays at the prompt with what it defined, like `python -i`. Tracebacks
  name the file and line. The script's folder becomes the current
  directory; `sys.argv` is `[PATH]`, `__file__` is set.
- **Files**: `open()` is a stream over the CardApi file calls (r w a, +,
  b; read/readline/readlines/iteration/write/seek/tell/with;
  `print(file=f)` since `sys.stdout` is a stream too). Text is bytes
  passed through (no unicode str). `a` opens without truncating and seeks
  to the end, since the card's append flag is not in the CardApi. The OS has
  eight descriptors in all and an app is not told when fn-` closes it, so
  Python holds at most four, closes unreferenced ones when the heap is
  collected (a finaliser; tried before refusing a fifth, and after every
  script), and closes all on exit(). One a script left open when fn-`
  takes the app is lost until a restart -- the kernel does not close an
  app's descriptors on release.
- **import NAME** finds NAME.py beside the script (`sys.path` is
  `['', '/lib']`, '' the current directory), then in `/lib`. `os`:
  getcwd, chdir, listdir, stat (`[0] & 0x4000` a folder, `[6]` the size),
  mkdir, remove, rename. `time`: sleep, sleep_ms (a busy wait that a held
  key still stops), ticks_ms, ticks_diff, time (epoch, 0 until the clock
  is set). input() (API 49, 2026-10-16) takes typed keys through api->key_wait: the prompt and what is typed on the input line, Esc KeyboardInterrupt, ctrl-d on nothing EOFError. While a script runs its prints are drawn within 100 ms (api->repaint_now, from the output and the VM hook), and time.sleep waits in key_wait -- it yields, and a key stops it.
- **Leaving**: `exit()`, `quit()`, `sys.exit()` or `raise SystemExit` at
  the prompt, or ctrl-d on an empty line (only: it scrolled too, and
  scrolling to the bottom closed the REPL -- ctrl-b / ctrl-f scroll now,
  ctrl-u clears the line), call `api->leave` -- back to Vim or the launcher. A SystemExit inside a
  script ends the script, not the app. No `CappUi.save`: the session is
  the heap, far too big to keep.
- Output appears when a run ends, not during it: the run is one key
  handler, and nothing paints meanwhile.
- **Tests**: `apps/python/test/host_repl.c` drives the real app through
  `test/fakeapi.c` (its in-memory card, `fakeapi_left`): the REPL, a file
  run with imports from its folder and /lib, globals after, tracebacks,
  open in every mode, os, time, exit/ctrl-d leaving and closing files,
  SystemExit and KeyboardInterrupt inside a script. MicroPython wants gcc,
  so it is not in the MSVC suite: `HOST=root@157.245.252.47 bash
  apps/python/tools/python_host_test.sh`. Measured by build_apps: code 102.1 KB, data
  14.7 KB, rodata 21.7 KB. Not yet run on a device with files.
