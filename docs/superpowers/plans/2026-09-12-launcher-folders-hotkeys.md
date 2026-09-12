# Launcher Folders and Opt Hotkeys Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Subdirectories of `/desktop` show as folders on the launcher carousel, and `Opt+letter` app shortcuts are stored in NVS and settable from the launcher or the console.

**Architecture:** Folders are real directories; `icons.c` scans one level down into the same flat table with a `parent` index, and the carousel walks one level at a time. Hotkeys are a portable 26-slot table (`kernel/sys/hotkeys.c`) with a load/save hook pair, wired to NVS in `src/main.c`; `global_key` fires them through `shell_exec`, the launcher binds them with `k`+letter, and a `hotkey` console command edits the same table.

**Tech Stack:** C11, ESP-IDF (device), CMake + tinytest host suite in `build/` (built from a VS developer shell: `cmake --build build && build\cardos_tests.exe`).

**Spec:** `docs/superpowers/specs/2026-09-12-launcher-folders-hotkeys-design.md`

## Global Constraints

- Portable modules (`kernel/sys/hotkeys.c`) must compile on the host with no ESP-IDF headers; device glue stays in `src/main.c`.
- Watch for ESP-IDF name collisions: prefix everything `hotkey_`/`hotkeys_`.
- `MAX_ICONS` stays 24; `Icon.name` stays 20 bytes; hotkey names are 16 bytes (`HOTKEY_NAME_MAX`).
- Reserved Opt letters: `b`, `w`, `h`. Defaults on first boot: t Todo, s Stocks, e Edit, m Mines.
- After touching nothing in `apps/`, `build_apps.py` is not needed; the firmware build is `python -m platformio run`.
- Commit messages end with `Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`. Use `rtk git ...`.

---

### Task 1: Hotkey store (portable, host-tested)

**Files:**
- Create: `kernel/sys/hotkeys.h`, `kernel/sys/hotkeys.c`
- Create: `test/test_hotkeys.c`
- Modify: `test/test_main.c` (declarations near line 167, RUN lines after the rpc block near line 416)
- Modify: `host/CMakeLists.txt:21` (add `${ROOT}/kernel/sys/hotkeys.c`)

**Interfaces:**
- Produces:
  ```c
  #define HOTKEY_NAME_MAX 16
  typedef struct {
    int  (*load)(char *buf, int size);          /* bytes read, or <=0 for none */
    void (*save)(const char *buf, int len);
  } HotkeyStore;
  void        hotkeys_init(const HotkeyStore *store);   /* NULL store: memory only */
  const char *hotkey_get(char letter);                   /* NULL if unbound */
  int         hotkey_set(char letter, const char *name); /* 0, -1 reserved, -2 not a letter */
  int         hotkey_reserved(char letter);
  int         hotkey_count(void);                        /* bound letters, for listing */
  ```
  Letters are case-insensitive on input. Blob format: 26 × `HOTKEY_NAME_MAX` bytes, NUL-padded, slot 0 = `a`.

- [ ] **Step 1: Write the failing tests**

`test/test_hotkeys.c`:
```c
/* Opt+letter bindings. The store is a fake so the tests can see what was
 * saved and hand it back, which is the whole round trip NVS provides. */
#include <string.h>
#include "tinytest.h"
#include "kernel/sys/hotkeys.h"

static char s_blob[26 * HOTKEY_NAME_MAX];
static int  s_blob_len;
static int  s_saves;

static int fake_load(char *buf, int size) {
  if (s_blob_len <= 0) return 0;
  if (size > s_blob_len) size = s_blob_len;
  memcpy(buf, s_blob, (size_t)size);
  return size;
}
static void fake_save(const char *buf, int len) {
  if (len > (int)sizeof s_blob) len = (int)sizeof s_blob;
  memcpy(s_blob, buf, (size_t)len);
  s_blob_len = len;
  s_saves++;
}
static const HotkeyStore FAKE = { fake_load, fake_save };

static void fresh(void) { s_blob_len = 0; s_saves = 0; hotkeys_init(&FAKE); }

void test_hotkeys_first_boot_seeds_the_old_table(void) {
  fresh();
  CHECK(!strcmp(hotkey_get('t'), "Todo"));
  CHECK(!strcmp(hotkey_get('s'), "Stocks"));
  CHECK(!strcmp(hotkey_get('e'), "Edit"));
  CHECK(!strcmp(hotkey_get('m'), "Mines"));
  CHECK(hotkey_get('q') == NULL);
  CHECK_EQ(hotkey_count(), 4);
}

void test_hotkeys_set_get_clear_and_save(void) {
  fresh();
  CHECK_EQ(hotkey_set('p', "Pinball"), 0);
  CHECK(!strcmp(hotkey_get('p'), "Pinball"));
  CHECK(!strcmp(hotkey_get('P'), "Pinball"));   /* case does not matter */
  CHECK_EQ(s_saves, 1);
  CHECK_EQ(hotkey_set('p', NULL), 0);
  CHECK(hotkey_get('p') == NULL);
  CHECK_EQ(hotkey_set('p', ""), 0);
  CHECK_EQ(s_saves, 3);
}

void test_hotkeys_survive_a_reload(void) {
  fresh();
  hotkey_set('p', "Pinball");
  hotkey_set('t', NULL);
  hotkeys_init(&FAKE);                            /* "reboot" */
  CHECK(!strcmp(hotkey_get('p'), "Pinball"));
  CHECK(hotkey_get('t') == NULL);                 /* the seed did not come back */
  CHECK(!strcmp(hotkey_get('s'), "Stocks"));
}

void test_hotkeys_refuse_reserved_and_bad_letters(void) {
  fresh();
  CHECK(hotkey_reserved('b'));
  CHECK(hotkey_reserved('W'));
  CHECK(hotkey_reserved('h'));
  CHECK(!hotkey_reserved('x'));
  CHECK_EQ(hotkey_set('b', "Mines"), -1);
  CHECK_EQ(hotkey_set('1', "Mines"), -2);
  CHECK_EQ(hotkey_set(0, "Mines"), -2);
  CHECK(hotkey_get('1') == NULL);
  CHECK(hotkey_get('b') == NULL);
}

void test_hotkeys_truncate_a_long_name(void) {
  fresh();
  CHECK_EQ(hotkey_set('x', "ANameLongerThanFifteenCharacters"), 0);
  CHECK_EQ((int)strlen(hotkey_get('x')), HOTKEY_NAME_MAX - 1);
}

void test_hotkeys_work_without_a_store(void) {
  hotkeys_init(NULL);
  CHECK(!strcmp(hotkey_get('t'), "Todo"));
  CHECK_EQ(hotkey_set('x', "Web"), 0);
  CHECK(!strcmp(hotkey_get('x'), "Web"));
}
```

Add to `test/test_main.c` after the rpc declarations (line ~167):
```c
void test_hotkeys_first_boot_seeds_the_old_table(void);
void test_hotkeys_set_get_clear_and_save(void);
void test_hotkeys_survive_a_reload(void);
void test_hotkeys_refuse_reserved_and_bad_letters(void);
void test_hotkeys_truncate_a_long_name(void);
void test_hotkeys_work_without_a_store(void);
```
and after `RUN(test_rpc_wake_word_alone_leaves_nothing);`:
```c
  printf("-- hotkeys --\n");
  RUN(test_hotkeys_first_boot_seeds_the_old_table);
  RUN(test_hotkeys_set_get_clear_and_save);
  RUN(test_hotkeys_survive_a_reload);
  RUN(test_hotkeys_refuse_reserved_and_bad_letters);
  RUN(test_hotkeys_truncate_a_long_name);
  RUN(test_hotkeys_work_without_a_store);
```

In `host/CMakeLists.txt` line 21 change to:
```cmake
  ${ROOT}/kernel/sys/input.c ${ROOT}/kernel/sys/rpc.c ${ROOT}/kernel/sys/hotkeys.c
```

- [ ] **Step 2: Write the header so the tests compile, then build to see them fail**

`kernel/sys/hotkeys.h`:
```c
/* Opt+letter shortcuts. Portable: the store is two callbacks so this file
 * has host tests and the NVS glue lives with the rest of it in src/main.c.
 *
 * Twenty-six slots, each an app name, saved as one blob. A name rather than
 * a path because that is what the launcher shows and what `run` takes, and
 * an app that moves into a folder keeps its name. */
#ifndef CARDOS_HOTKEYS_H
#define CARDOS_HOTKEYS_H

#define HOTKEY_NAME_MAX 16
#define HOTKEY_SLOTS    26
#define HOTKEY_BLOB     (HOTKEY_SLOTS * HOTKEY_NAME_MAX)

typedef struct {
  int  (*load)(char *buf, int size);   /* bytes read, or <= 0 for none */
  void (*save)(const char *buf, int len);
} HotkeyStore;

/* Loads the table, or seeds the defaults when there is nothing saved.
 * NULL for a store keeps the table in memory only. */
void hotkeys_init(const HotkeyStore *store);

/* The app bound to a letter, or NULL. Case does not matter. */
const char *hotkey_get(char letter);

/* Bind, or clear with NULL or "". 0 on success, -1 for a letter the system
 * owns, -2 for something that is not a letter. Saved at once. */
int hotkey_set(char letter, const char *name);

/* Letters the shell itself answers to: b (bluetooth), w (wifi), h (help). */
int hotkey_reserved(char letter);

/* How many letters are bound. */
int hotkey_count(void);

#endif /* CARDOS_HOTKEYS_H */
```

Build from a VS developer shell (`cmake --build build`). Expected: link errors for `hotkeys_init` etc. — the tests fail to link.

- [ ] **Step 3: Implement**

`kernel/sys/hotkeys.c`:
```c
/* Opt+letter shortcuts. See hotkeys.h. */

#include "kernel/sys/hotkeys.h"

#include <stdio.h>
#include <string.h>

static char s_table[HOTKEY_SLOTS][HOTKEY_NAME_MAX];
static HotkeyStore s_store;

static const char RESERVED[] = "bwh";

static int slot_of(char letter) {
  if (letter >= 'A' && letter <= 'Z') letter = (char)(letter + 32);
  if (letter < 'a' || letter > 'z') return -1;
  return letter - 'a';
}

static void seed(void) {
  memset(s_table, 0, sizeof s_table);
  snprintf(s_table['t' - 'a'], HOTKEY_NAME_MAX, "%s", "Todo");
  snprintf(s_table['s' - 'a'], HOTKEY_NAME_MAX, "%s", "Stocks");
  snprintf(s_table['e' - 'a'], HOTKEY_NAME_MAX, "%s", "Edit");
  snprintf(s_table['m' - 'a'], HOTKEY_NAME_MAX, "%s", "Mines");
}

static void save(void) {
  if (s_store.save) s_store.save(&s_table[0][0], HOTKEY_BLOB);
}

void hotkeys_init(const HotkeyStore *store) {
  int i, n = 0;

  if (store) s_store = *store;
  else { s_store.load = NULL; s_store.save = NULL; }

  memset(s_table, 0, sizeof s_table);
  if (s_store.load) n = s_store.load(&s_table[0][0], HOTKEY_BLOB);
  if (n <= 0) { seed(); return; }

  /* Whatever was saved, every slot ends in a NUL: a blob from a build with a
   * different HOTKEY_NAME_MAX must not become an unterminated name. */
  for (i = 0; i < HOTKEY_SLOTS; i++) s_table[i][HOTKEY_NAME_MAX - 1] = 0;
}

int hotkey_reserved(char letter) {
  int s = slot_of(letter);
  return s >= 0 && strchr(RESERVED, (char)('a' + s)) != NULL;
}

const char *hotkey_get(char letter) {
  int s = slot_of(letter);
  if (s < 0 || !s_table[s][0]) return NULL;
  return s_table[s];
}

int hotkey_set(char letter, const char *name) {
  int s = slot_of(letter);
  if (s < 0) return -2;
  if (hotkey_reserved(letter)) return -1;
  snprintf(s_table[s], HOTKEY_NAME_MAX, "%s", name ? name : "");
  save();
  return 0;
}

int hotkey_count(void) {
  int i, n = 0;
  for (i = 0; i < HOTKEY_SLOTS; i++) n += s_table[i][0] != 0;
  return n;
}
```

- [ ] **Step 4: Build and run the suite**

`cmake --build build && build\cardos_tests.exe` — expected: `-- hotkeys --` block all `ok`, `0 failures`.

- [ ] **Step 5: Commit**

```
rtk git add kernel/sys/hotkeys.h kernel/sys/hotkeys.c test/test_hotkeys.c test/test_main.c host/CMakeLists.txt
rtk git commit -m "Hotkeys: a 26-slot Opt+letter table, host-tested"
```

---

### Task 2: Wire hotkeys to NVS and fire them from `global_key`

**Files:**
- Modify: `src/main.c` — `global_key` cases at lines 705-724; init near line 916 (`env_init();`)

**Interfaces:**
- Consumes: `hotkeys_init`, `hotkey_get` from Task 1.
- Produces: nothing new; `Opt+letter` now consults the table from every shell.

- [ ] **Step 1: Add the NVS glue near the other static helpers in `src/main.c`** (before `global_key`)

```c
#include "kernel/sys/hotkeys.h"

/* The hotkey table lives in the same NVS namespace as the environment, as one
 * blob: it is a setting, and `defaults` should wipe it with the others. */
#define HOTKEY_NVS_NS  "cardosenv"
#define HOTKEY_NVS_KEY "hotkeys"

static int hotkey_nvs_load(char *buf, int size) {
  nvs_handle_t h;
  size_t n = (size_t)size;
  if (nvs_open(HOTKEY_NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
  if (nvs_get_blob(h, HOTKEY_NVS_KEY, buf, &n) != ESP_OK) n = 0;
  nvs_close(h);
  return (int)n;
}

static void hotkey_nvs_save(const char *buf, int len) {
  nvs_handle_t h;
  if (nvs_open(HOTKEY_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_blob(h, HOTKEY_NVS_KEY, buf, (size_t)len);
  nvs_commit(h);
  nvs_close(h);
}
```

Check `nvs_flash.h`/`nvs.h` is already included in `src/main.c` (it uses `nvs_open` at line ~200); add `#include "nvs.h"` if not.

- [ ] **Step 2: Initialise after `env_init();` (line ~916)**

```c
  {
    static const HotkeyStore NVS_STORE = { hotkey_nvs_load, hotkey_nvs_save };
    hotkeys_init(&NVS_STORE);
  }
```

- [ ] **Step 3: Replace the hardcoded four-letter case in `global_key`**

Delete the block from the comment `/* A letter runs the app of that name...` through its closing `}` (lines 705-718) and change the `default:` to:

```c
  default:
    /* Any other opt letter is a shortcut if the user bound one. Unbound, it is
     * swallowed rather than passed on: a shortcut that does nothing must not
     * type a letter into whatever has focus. */
    if (k >= KEY_OPT_LETTER('a') && k <= KEY_OPT_LETTER('z')) {
      const char *name = hotkey_get((char)('a' + (k - KEY_OPT_LETTER('a'))));
      if (name && shell_exec(name, NULL) == 0 && ui_shell() == UI_LAUNCHER)
        s_mode = MODE_LAUNCHER;
      return 1;
    }
    return KEY_IS_OPT(k);
```

- [ ] **Step 4: Build the firmware**

`python -m platformio run` — expected: builds clean (warnings-as-errors for implicit declarations, so the include matters).

- [ ] **Step 5: Commit**

```
rtk git add src/main.c
rtk git commit -m "Opt+letter looks up the hotkey table instead of a fixed four"
```

---

### Task 3: `hotkey` console command

**Files:**
- Modify: `src/shellcmd.h` (after `cmd_set` declaration, line ~46)
- Modify: `src/shellcmd.c` (after `cmd_set`, line ~557)
- Modify: `src/main.c` dispatch (line ~184) and help text (line ~135)

**Interfaces:**
- Consumes: `hotkey_get/set/reserved/count`.
- Produces: `void cmd_hotkey(const char *arg);`

- [ ] **Step 1: Declare**

`src/shellcmd.h`:
```c
/* hotkey            -- list the Opt+letter shortcuts
 * hotkey X NAME     -- Opt+X opens NAME
 * hotkey X -        -- unbind X */
void cmd_hotkey(const char *arg);
```

- [ ] **Step 2: Implement in `src/shellcmd.c`** (add `#include "kernel/sys/hotkeys.h"` at the top)

```c
/* --------------------------------------------------------------- hotkey --- */

void cmd_hotkey(const char *arg) {
  char letter;
  const char *name;

  if (!arg || !*arg) {
    char c;
    if (hotkey_count() == 0) { con_write("no shortcuts. hotkey X NAME sets one\n"); return; }
    for (c = 'a'; c <= 'z'; c++) {
      const char *n = hotkey_get(c);
      if (n) con_printf("opt-%c  %s\n", c, n);
    }
    return;
  }

  letter = arg[0];
  if (arg[1] != ' ' && arg[1] != 0) { err("hotkey", "needs a single letter, then a name"); return; }
  name = arg[1] ? arg + 2 : "";
  while (*name == ' ') name++;
  if (!strcmp(name, "-")) name = "";

  switch (hotkey_set(letter, name)) {
  case -1: err("hotkey", "that letter belongs to the system (b w h)"); return;
  case -2: err("hotkey", "needs a letter a-z"); return;
  default: break;
  }
  if (*name) con_printf("opt-%c opens %s\n", letter, name);
  else con_printf("opt-%c cleared\n", letter);
}
```

- [ ] **Step 3: Dispatch and help in `src/main.c`**

After the `set` line:
```c
  else if (!strcmp(line, "hotkey")) cmd_hotkey(arg);
```
Change the help line:
```c
  con_write("shell    env set NAME=VALUE, hotkey X NAME, tab completes\n");
```

- [ ] **Step 4: Build the firmware** — `python -m platformio run`, expect clean.

- [ ] **Step 5: Commit**

```
rtk git add src/shellcmd.h src/shellcmd.c src/main.c
rtk git commit -m "hotkey: list, set and clear Opt+letter shortcuts from the console"
```

---

### Task 4: Folders in the icon table

**Files:**
- Modify: `kernel/ui/icons.h` (`IconKind`, `Icon`, new accessors)
- Modify: `kernel/ui/icons.c` (`scan`, `partition_cli`, `icons_reload`, `icon_bitmap`, `icon_colour`)
- Modify: `kernel/ui/icons_builtin.h` (add `ICON_FOLDER_`)
- Modify: `kernel/ui/desktop.c:517` (a folder icon does nothing on the desktop)

**Interfaces:**
- Produces:
  ```c
  typedef enum { ICON_BUILTIN, ICON_FIRMWARE, ICON_CAPP, ICON_FOLDER } IconKind;
  /* Icon gains: */ int parent;   /* index of the folder entry, or -1 */
  int         icons_in_count(int folder);        /* visible entries at this level; -1 = top */
  const Icon *icons_in_at(int folder, int i);    /* i-th of those */
  int         icon_index(const Icon *ic);        /* back to a flat index */
  ```
  `icons_count()`/`icon_at()` keep their flat meaning (every visible entry at every level, folders included, commands last) so the desktop and by-name lookups are untouched.

- [ ] **Step 1: Header changes**

`kernel/ui/icons.h`: extend the kind enum and struct, add the folder text to the file comment:
```c
 *   NAME/       a folder: one level of subdirectory, scanned the same way
```
```c
typedef enum { ICON_BUILTIN, ICON_FIRMWARE, ICON_CAPP, ICON_FOLDER } IconKind;
```
Add to `Icon` after `cli`:
```c
  int      parent;      /* flat index of the folder this sits in, or -1 */
```
Add after `icon_at`:
```c
/* One level of the tree. `folder` is a flat index of an ICON_FOLDER entry,
 * or -1 for the top. Only visible entries, so a carousel can walk 0..n-1. */
int         icons_in_count(int folder);
const Icon *icons_in_at(int folder, int i);

/* The flat index of an entry, for the calls that take one. -1 if not ours. */
int icon_index(const Icon *ic);
```

- [ ] **Step 2: A folder glyph in `icons_builtin.h`**

```c
/* A folder: tab on the top left, body below. */
static const uint8_t ICON_FOLDER_[32] = {
  0x00, 0x00, 0x00, 0x00, 0x3E, 0x00, 0x41, 0x00,
  0x7F, 0xFE, 0x40, 0x02, 0x40, 0x02, 0x40, 0x02,
  0x40, 0x02, 0x40, 0x02, 0x40, 0x02, 0x40, 0x02,
  0x40, 0x02, 0x7F, 0xFE, 0x00, 0x00, 0x00, 0x00,
};
```

- [ ] **Step 3: Scan one level down in `icons.c`**

Change `scan`'s signature and body. It gains a `parent` argument; directories at the top level become folder entries and are then scanned with `parent` set:

```c
static void scan(const char *dir, int bin_only, int parent) {
  FsDir d;
  FsEntry e;

  if (fs_opendir(dir, &d) != 0) return;

  while (s_nicon < MAX_ICONS && fs_readdir(&d, &e) == 1) {
    size_t n = strlen(e.name);
    Icon *ic = &s_icon[s_nicon];
    if (e.name[0] == '.') continue;      /* the stamp file, and anything like it */

    memset(ic, 0, sizeof *ic);
    ic->parent = parent;
    snprintf(ic->path, sizeof ic->path, "%s/%s", dir, e.name);

    if (e.is_dir) {
      /* One level: a folder at the top is an entry and is scanned; a folder
       * inside a folder is ignored, and so is the colour-icon store. */
      if (bin_only || parent >= 0 || strcmp(e.name, "icons") == 0) continue;
      ic->kind = ICON_FOLDER;
      ic->slot = -1;
      snprintf(ic->name, sizeof ic->name, "%s", e.name);
      s_nicon++;
      scan(ic->path, 0, s_nicon - 1);
      continue;
    }
    /* ...the existing .bin / .capp / .app branches, unchanged... */
```
Keep the rest of the loop exactly as it is. `fs_readdir` is re-entrant across two open directories (one `FsDir` per call); if it turns out not to be on the device, collect subdirectory names into a small local array first and scan them after `fs_closedir`.

Update the two callers in `icons_reload`:
```c
  scan(ICONS_DIR, 0, -1);
  scan(FIRMWARE_DIR, 1, -1);
```

- [ ] **Step 4: `partition_cli` must keep `parent` indices valid**

The stable partition moves entries, so a `parent` that was a flat index would dangle. Rewrite it to remap:

```c
/* Stable partition: visible entries keep their order, commands move to the
 * end keeping theirs. Parents are flat indices, so they are remapped through
 * the same move. */
static void partition_cli(void) {
  Icon tmp[MAX_ICONS];
  int  newpos[MAX_ICONS];
  int i, n = 0;

  for (i = 0; i < s_nicon; i++) if (!s_icon[i].cli) { newpos[i] = n; tmp[n++] = s_icon[i]; }
  s_nvisible = n;
  for (i = 0; i < s_nicon; i++) if (s_icon[i].cli)  { newpos[i] = n; tmp[n++] = s_icon[i]; }
  for (i = 0; i < s_nicon; i++) {
    if (tmp[i].parent >= 0) tmp[i].parent = newpos[tmp[i].parent];
    s_icon[i] = tmp[i];
  }
}
```

- [ ] **Step 5: Level accessors and index, after `icon_at`**

```c
int icons_in_count(int folder) {
  int i, n = 0;
  for (i = 0; i < s_nvisible; i++) n += s_icon[i].parent == folder;
  return n;
}

const Icon *icons_in_at(int folder, int i) {
  int k;
  for (k = 0; k < s_nvisible; k++) {
    if (s_icon[k].parent != folder) continue;
    if (i-- == 0) return &s_icon[k];
  }
  return NULL;
}

int icon_index(const Icon *ic) {
  if (!ic || ic < s_icon || ic >= s_icon + s_nicon) return -1;
  return (int)(ic - s_icon);
}
```

- [ ] **Step 6: Icons for folders**

In `icon_bitmap`, before the `ICON_FIRMWARE` line:
```c
  if (ic->kind == ICON_FOLDER) return ICON_FOLDER_;
```
`icon_colour` already loads `/desktop/icons/NAME.cic` by name, so `Games.cic` works with no change. `icon_app` returns NULL for a folder already (falls through).

- [ ] **Step 7: Desktop ignores folders**

`kernel/ui/desktop.c` in `launch_icon` after the firmware line (517):
```c
  if (ic->kind == ICON_FOLDER) return;    /* the desktop is flat; the launcher has folders */
```
The desktop still lists every app flat, which is what it did before folders existed.

- [ ] **Step 8: Build the firmware** — `python -m platformio run`, expect clean.

- [ ] **Step 9: Commit**

```
rtk git add kernel/ui/icons.h kernel/ui/icons.c kernel/ui/icons_builtin.h kernel/ui/desktop.c
rtk git commit -m "Icons: one level of folders under /desktop, in the same flat table"
```

---

### Task 5: Folders on the carousel

**Files:**
- Modify: `kernel/ui/launchui.c` — statics (line 55), `wrap`, `kind_word`, `paint_pips`, `paint_carousel`, `launch_with`, `move`, `launchui_init`, `launchui_run`, `launchui_run_path`, `launchui_key`, `shell_keys`, `flush`, and the mouse click handler (find `launch(` calls below line 440)

**Interfaces:**
- Consumes: `icons_in_count`, `icons_in_at`, `icon_index`, `ICON_FOLDER` from Task 4.

- [ ] **Step 1: Level state**

Add after `s_sel`:
```c
static int      s_folder = -1;    /* flat index of the open folder, or -1 for the top */
```

Replace `wrap`:
```c
static int wrap(int i) {
  int n = icons_in_count(s_folder);
  if (n <= 0) return 0;
  return ((i % n) + n) % n;
}

/* The entry at carousel position i, at the current level. */
static const Icon *at(int i) { return icons_in_at(s_folder, i); }
```

- [ ] **Step 2: Painting**

`kind_word` gains:
```c
  case ICON_FOLDER:   return "folder";
```
In `paint_carousel`, replace `int n = icons_count();` with `int n = icons_in_count(s_folder);`, replace `icon_at(s_sel)` with `at(s_sel)`, and change the three `paint_icon(...)` calls to pass flat indices: `paint_icon(icon_index(at(wrap(s_sel - 1))), ...)`, `paint_icon(icon_index(at(wrap(s_sel + 1))), ...)`, `paint_icon(icon_index(at(s_sel)), ...)`.

The empty case becomes level-aware:
```c
  if (n == 0) {
    if (s_folder >= 0) {
      draw_text_scaled(28, 46, "nothing here", 2, C_TITLE_FG, C_DESKTOP);
      draw_text(28, 74, "escape goes back up", C_DESK_DIM, C_DESKTOP);
      return;
    }
    /* ...existing "no apps" text... */
  }
```

Inside a folder, the note line shows the path. Where `k` is computed:
```c
    char where[48];
    const char *k;
    if (s_note[0]) k = s_note;
    else if (s_folder >= 0) {
      const Icon *f = icon_at(s_folder);
      snprintf(where, sizeof where, "%s / %s", f ? f->name : "?", kind_word(ic));
      k = where;
    } else k = kind_word(ic);
```

`paint_pips(n)` is already passed `n`; no change.

- [ ] **Step 3: Enter a folder, go up**

`launch_with` takes a carousel position today; make it take a flat index and add the folder case at the top:
```c
static void open_folder(int flat) {
  s_folder = flat;
  s_sel = 0;
  s_note[0] = 0;
  s_dirty = 1;
  flush();
}

static void close_folder(void) {
  const Icon *f = icon_at(s_folder);
  int i, n;
  s_folder = -1;
  /* Land on the folder that was just left, not on the first icon. */
  s_sel = 0;
  n = icons_in_count(-1);
  for (i = 0; i < n; i++) if (icons_in_at(-1, i) == f) { s_sel = i; break; }
  s_note[0] = 0;
  s_dirty = 1;
  flush();
}
```
In `launch_with(int i, const char *args)`, `i` becomes a **flat** index (`icon_at(i)` is already what it calls). Add before the firmware branch:
```c
  if (ic->kind == ICON_FOLDER) { open_folder(i); return; }
```
and change `launch`:
```c
static void launch(int pos) { launch_with(icon_index(at(pos)), NULL); }
```
In `launchui_key`, the carousel `KEY_ESC` case:
```c
  case KEY_ESC:
    if (s_folder >= 0) { close_folder(); return 0; }
    desktop_set_autostart(0); return 1;     /* to the console */
```
and `KEY_ENTER`/`' '`: `if (icons_in_count(s_folder)) launch(s_sel);`.

`move`: replace `icons_count()` with `icons_in_count(s_folder)`.

- [ ] **Step 4: `launchui_run` / `launchui_run_path` land in the right folder**

Both set `s_sel = i;` on a flat index today. Replace each with a helper that also sets the level:
```c
/* Point the carousel at a flat entry: its folder becomes the level and its
 * position within it the selection, so Escape from the app lands on it. */
static void select_flat(int flat) {
  const Icon *ic = icon_at(flat);
  int i, n;
  s_folder = ic ? ic->parent : -1;
  s_sel = 0;
  n = icons_in_count(s_folder);
  for (i = 0; i < n; i++) if (icons_in_at(s_folder, i) == ic) { s_sel = i; break; }
}
```
and each `s_sel = i;` in those two functions becomes `select_flat(i);`. In `launchui_run`, the fall-through `launch_with(i, args)` already takes a flat index. A CLI entry has no position at any level; `select_flat` leaves `s_sel = 0`, which is fine because a command never hosts.

`launchui_init` sets `s_folder = -1;` beside `s_sel = 0;`.

- [ ] **Step 5: Mouse**

Find the click handler (`launchui_mouse_apply`, below line 440): any `launch(s_sel)` stays (it is a position); any `icons_count()` used to decide whether a click is on an icon becomes `icons_in_count(s_folder)`. Read the function before editing — it is short.

- [ ] **Step 6: Help text**

`shell_keys()` carousel string gains `escape\tup a level, or the console\n` in place of `escape\tthe console\n`.

- [ ] **Step 7: Build, flash, try**

`python -m platformio run -t upload --upload-port COM3`. On the device (via Files or a card reader): make `/desktop/Games`, move `mines.capp` and `pinball.capp` in, press `r`. Expect: a "Games" folder icon; Enter shows Mines/Pinball with "Games / app" under the name; Escape returns to the Games icon; `run mines` from the console still works and Escape from it lands inside Games; `Opt+M` still opens Mines.

- [ ] **Step 8: Commit**

```
rtk git add kernel/ui/launchui.c
rtk git commit -m "Launcher: folders are subdirectories of /desktop, one level deep"
```

---

### Task 6: Bind a hotkey from the carousel

**Files:**
- Modify: `kernel/ui/launchui.c` — `launchui_key`, `shell_keys`, `flush`

**Interfaces:**
- Consumes: `hotkey_set`, `hotkey_reserved`, `hotkey_get`, `hotkey_count`.

- [ ] **Step 1: Bind mode**

Add a static `static int s_binding;` and include `kernel/sys/hotkeys.h`. In `launchui_key`, after the help-panel block and before the arrow remap:

```c
  /* Bind mode: `k` on an app, then the letter. Two keys rather than a chord,
   * because every Opt chord already means "open" -- including here. */
  if (s_binding) {
    const Icon *ic = at(s_sel);
    s_binding = 0;
    if (key >= 'A' && key <= 'Z') key = (uint8_t)(key + 32);
    if (key >= 'a' && key <= 'z' && ic) {
      int r = hotkey_set((char)key, ic->name);
      if (r == 0) snprintf(s_note, sizeof s_note, "opt-%c opens %s", key, ic->name);
      else snprintf(s_note, sizeof s_note, "opt-%c belongs to the system", key);
    } else {
      s_note[0] = 0;
    }
    s_dirty = 1;
    flush();
    return 0;
  }
```
In the carousel `switch`:
```c
  case 'k': case 'K': {
    const Icon *ic = at(s_sel);
    if (!ic || ic->kind == ICON_FOLDER || ic->kind == ICON_FIRMWARE) {
      snprintf(s_note, sizeof s_note, "only an app can have a shortcut");
    } else {
      s_binding = 1;
      snprintf(s_note, sizeof s_note, "press a letter for %s", ic->name);
    }
    s_dirty = 1;
    flush();
    return 0;
  }
```
`launchui_init` and `enter` clear `s_binding = 0;`.

- [ ] **Step 2: Bindings in the help panel**

`shell_keys()` for the carousel adds `k\tbind opt-letter to this app\n`. Then `flush()` appends the current table: build the help text into a static buffer instead of passing the literal:

```c
static const char *shell_keys(void) {
  static char buf[512];
  char c;
  size_t n;

  if (s_app) return "escape\tback to the launcher\nctrl-h\tclose this\n";

  snprintf(buf, sizeof buf,
           "arrows\tmove along the row\nenter\topen\nk\tbind opt-letter to this app\n"
           "r\treload the app list\nd\tswitch to the desktop\n"
           "escape\tup a level, or the console\nctrl-h\tclose this\n");
  n = strlen(buf);
  for (c = 'a'; c <= 'z' && n < sizeof buf - 40; c++) {
    const char *name = hotkey_get(c);
    if (!name) continue;
    n += (size_t)snprintf(buf + n, sizeof buf - n, "opt-%c\t%s\n", c, name);
  }
  return buf;
}
```

- [ ] **Step 3: Build, flash, try**

Highlight Pinball, press `k`, press `p`: note says "opt-p opens Pinball". `Ctrl+H` lists it. Press `Opt+P` from the console: Pinball opens. `hotkey` in the console lists it. `k` then `b`: "opt-b belongs to the system". Reboot: still bound.

- [ ] **Step 4: Commit**

```
rtk git add kernel/ui/launchui.c
rtk git commit -m "Launcher: k then a letter binds Opt+letter to the highlighted app"
```

---

### Task 7: `update apps` finds an app inside a folder

**Files:**
- Modify: `kernel/net/update.c:111-115` and `:124-132`

**Interfaces:**
- Consumes: `icons_total`, `icon_at`, `ICON_CAPP`.

- [ ] **Step 1: A path lookup by file stem**

Add above `update_check` (the function containing line 111):
```c
#include "kernel/ui/icons.h"   /* already included: ICONS_DIR comes from it */

/* Where NAME.capp lives on the card, folders included; the top level if it
 * is not there at all. Matched on the file name, not the app's own name,
 * because that is what the manifest is keyed by. */
static void capp_path(const char *name, char *out, size_t size) {
  char want[40];
  int i;
  snprintf(want, sizeof want, "/%s.capp", name);
  for (i = 0; i < icons_total(); i++) {
    const Icon *ic = icon_at(i);
    size_t pl, wl = strlen(want);
    if (!ic || ic->kind != ICON_CAPP) continue;
    pl = strlen(ic->path);
    if (pl >= wl && strcmp(ic->path + pl - wl, want) == 0) {
      snprintf(out, size, "%s", ic->path);
      return;
    }
  }
  snprintf(out, size, "%s/%s.capp", ICONS_DIR, name);
}
```

- [ ] **Step 2: Use it**

Line 113:
```c
    capp_path(out->m.app[i].name, path, sizeof path);
```
Lines 131-132:
```c
  capp_path(a->name, dst, sizeof dst);
  snprintf(tmp, sizeof tmp, "%s.new", dst);
```
Note `icons_total()` is 0 until the launcher has loaded once; `update_check` is reached from the console, which may not have. Add at the top of `update_check`, before the loop: `if (icons_total() == 0) icons_reload();` — the same guard `launchui.c`'s `need_icons` uses.

- [ ] **Step 3: Build, flash, try**

With `mines.capp` inside `/desktop/Games`, edit a comment in `apps/mines.c`, run `build_apps.py`, then `update` on the device: it reports mines stale, `update apps` writes `/desktop/Games/mines.capp` (check with `ls /desktop/Games` — no new copy at the top).

- [ ] **Step 4: Commit**

```
rtk git add kernel/net/update.c
rtk git commit -m "update apps: replace an app where it lives, folders included"
```

---

### Task 8: Docs

**Files:**
- Modify: `CLAUDE.md` — the launcher paragraph under "Where things stand"

- [ ] **Step 1: Two sentences**

After the launcher bullet's description add:
```
  Subdirectories of `/desktop` are folders (one level); Enter goes in, Escape
  comes out. `k` then a letter binds `Opt+letter` to the highlighted app;
  `hotkey` in the console does the same, and the table is in NVS.
```

- [ ] **Step 2: Commit**

```
rtk git add CLAUDE.md
rtk git commit -m "CLAUDE.md: folders and hotkeys"
```
