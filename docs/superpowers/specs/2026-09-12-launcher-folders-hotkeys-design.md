# Launcher folders and configurable Opt hotkeys

Approved 2026-09-12.

## Folders

A folder is a real subdirectory of `/desktop`, one level deep. Chosen over a
virtual grouping file because there is nothing to invent: the Files app
already moves files, and a folder that is a directory looks like one on a PC.

### Scanning (`kernel/ui/icons.c`)

- `scan()` no longer skips directories. A subdirectory of `/desktop` whose
  name does not start with `.` and is not `icons` (the colour-icon store)
  becomes an `ICON_FOLDER` entry, then its contents are scanned with
  `parent` set to that entry's index. Directories inside a folder are
  ignored.
- `Icon` gains `int parent` (-1 at top level). Everything stays in the one
  flat `s_icon[]` table so every existing by-name lookup -- `launchui_run`,
  `run`, voice `open`, `update apps` -- finds an app wherever it sits.
- New accessors: `icons_in_count(folder)` and `icons_in_at(folder, i)` walk
  the visible (non-CLI) entries whose `parent == folder`; `-1` is the top.
  `partition_cli()` keeps CLI entries behind the visible ones as today.
- A folder's icon is `ICON_FOLDER_` from `icons_builtin.h`, or
  `/desktop/icons/NAME.cic` if present, like any other entry.

### Carousel (`kernel/ui/launchui.c`)

- `s_folder` (-1 = top) and `s_sel` index into `icons_in_*`.
- The kind line says `folder` for a folder; inside one, the note line reads
  `Games / Mines` so the level is always visible.
- Enter on a folder enters it with `s_sel = 0`. Escape inside a folder goes
  up; Escape at the top still goes to the console.
- An empty folder paints "nothing here".
- `launchui_run` and `launchui_run_path` set `s_folder` to the app's parent
  so returning from the app lands where it lives.

### Update

`kernel/net/update.c` installs a `.capp` at the path the icon table already
has for that file name, so an app moved into a folder is updated in place.
An app not on the card at all lands at the top level as today. The manifest
format is unchanged.

## Hotkeys

`Opt+letter` launches an app from any shell, over any app, including inside
the launcher and inside a folder. The binding is configurable rather than the
four-entry table in `src/main.c`.

### Store (`kernel/sys/hotkeys.c`, portable, host-tested)

- 26 slots, `a`..`z`, each an app name of up to 15 characters, saved as one
  NVS blob under key `hotkeys`. Loaded once at boot.
- First boot (no blob) seeds today's table: `t` Todo, `s` Stocks, `e` Edit,
  `m` Mines.
- Reserved letters cannot be bound and `hotkey_set` refuses them:
  `b` (Bluetooth), `w` (WiFi), `h` (help). The list lives in `hotkeys.c` so
  the launcher, the console and the tests agree.
- API: `hotkeys_init`, `hotkey_get(letter)` -> name or NULL,
  `hotkey_set(letter, name)` -> 0 / -1 reserved / -2 bad letter, NULL or
  empty clears, `hotkey_reserved(letter)`.

### Firing (`src/main.c`)

`global_key` handles every `KEY_OPT_LETTER` not claimed by the system by
looking up `hotkey_get` and calling `shell_open_app(name)` -- the path voice
and `run` use. An unbound letter does nothing.

### Binding in the launcher

On the carousel with an app highlighted, `k` puts the launcher in bind mode:
the note line says `press a letter for Mines`. The next letter binds it and
the note confirms `opt-x opens Mines`; a reserved letter says so; Escape or
anything else cancels. A folder or firmware entry cannot be bound. `Ctrl+H`
in the launcher appends the current bindings to the key list.

### Console

`hotkey` lists bindings; `hotkey t Todo` sets; `hotkey t -` clears. Same
store, same validation, same messages.

## Testing

- `test/test_hotkeys.c`: seeding, set/get/clear, reserved letters, bad
  letters, name truncation, round trip through the save/load hooks.
- Icons and the launcher are device code (they read the card); the folder
  walk is exercised on hardware. `icons_in_*` are kept simple enough that
  a wrong answer is visible on the carousel.

## Out of scope

Nested folders, moving apps from the launcher, and per-folder ordering.
