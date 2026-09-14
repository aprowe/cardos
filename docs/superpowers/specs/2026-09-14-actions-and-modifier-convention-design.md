# One action table: modifiers, menus, help, and a scripting surface

**Status:** proposed 2026-09-14.

## The problem, stated by the machine rather than by taste

Three modifiers, and only one of them currently means anything consistent.

**Opt already works.** `opt-1/2/3` switch shells, `opt-0/9` are brightness,
`opt-b`/`opt-w` bring the radios back, `opt-h` lists them, and `opt-<letter>`
runs whatever the user bound with `hotkey`. These have codes of their own
(0xA0..0xD9) precisely so they survive being typed *inside* an app -- they are
how you leave one. Opt is the OS layer and needs no change.

**Ctrl is contested, today, on the device.** The apps use it for their own
commands and the desktop takes four chords out from under them:

| chord | the desktop does | the app wanted |
|---|---|---|
| `ctrl-S` | open the Start menu | Edit, IDE: **save** |
| `ctrl-P` | keyboard-driven pointer | Edit: **preview**, IDE: **console** |
| `ctrl-W` | close the window | -- |
| `ctrl-F` | fullscreen | -- |

Two of those are live collisions. In a desktop window, ctrl-S does not save.
That is the bug underneath the request.

**Fn is nearly unused.** It turns `; , . /` into the arrow cluster and does
nothing else. It is the only free modifier on the board.

## The convention

| modifier | scope | owned by | examples |
|---|---|---|---|
| **Ctrl** | the app | whatever has focus | save, run, sync, preview |
| **Fn** | the window | the shell | close, fullscreen, minimise, next window, help, Start |
| **Opt** | the OS | the kernel | switch shell, brightness, radios, user hotkeys |

Read outwards: ctrl acts on the document, fn on the frame around it, opt on
the machine around that. A key never means two things at once, and an app can
take every ctrl chord it likes without the shell flinching.

**The shells give up ctrl entirely.** `ctrl-S`/`P`/`W`/`F` become
`fn-s`/`fn-p`/`fn-w`/`fn-f`. `ctrl-h` for help becomes `fn-h` (it is already a
special case -- see KEY_HELP, which exists because ctrl-h collides with
Backspace, a collision that stops existing under fn).

### Key codes

Fn chords need codes of their own for the same reason opt's do: they must
survive being typed inside a text field. The space above 0xA0 is already
carved up, and 0xE0 is free:

```c
#define KEY_FN_LETTER(c) ((uint8_t)(0xE0 + ((c) - 'a')))   /* 0xE0..0xF9 */
#define KEY_IS_FN(k)     ((k) >= 0xE0)
```

`KEY_IS_OPT` becomes a range (`0xA0..0xD9`) rather than `>= 0xA0`, which it
can no longer be.

Fn with `; , . /` stays the arrow cluster. Arrows are not a chord anyone
thinks of as one, and taking them away to free four letters would cost more
than it bought.

## The second half: one table per app

The convention is worth little if it lives in comments. Right now an app
states its actions **three times**:

1. `capp_info.help` -- a string of `"key\tmeaning\n"` lines
2. the `switch` in `app_key`
3. the `TbMenu`/`TbItem` tables added for the menu bar

Three descriptions of one set of facts, and nothing checks they agree. The
menu bar already proved the shape: an app declaring `{label, action}` is one
field away from declaring everything.

```c
typedef struct {
  const char *id;      /* stable, machine-readable: "save", "sync", "run.vm" */
  const char *label;   /* shown to a person: "Save" */
  uint8_t     key;     /* the ctrl chord, or 0 for menu-only */
  const char *menu;    /* which menu it hangs under, or NULL */
  uint8_t     flags;   /* CAPP_ACT_HIDDEN, CAPP_ACT_DESTRUCTIVE, ... */
} CappAction;
```

The app registers one array and implements one `do_action(const char *id)`.
From that single table, four things fall out and cannot drift apart:

- **The key handler.** The shell matches a ctrl chord against the table before
  offering the key to the app as text. An app stops writing a switch.
- **The menu bar.** `toolbar.h` reads the table instead of a parallel one, and
  a menu item shows the chord beside its label for free -- which is how people
  learn shortcuts.
- **The help screen.** `help_paint` renders the table rather than a
  hand-written string, so `capp_info.help` stops being a thing to forget to
  update. The fn and opt rows come from the kernel, which knows them.
- **A scripting surface.** `id` is stable and machine-readable, so
  `api->action_list()` and `api->action_invoke(id)` expose exactly what a
  person can do to anything that can call a function: the console (`todo
  sync`), the voice verbs in `kernel/sys/rpc.c`, or a model. The existing
  voice design already argues this point -- *"An LLM choosing between seven
  verbs is useful; an LLM handing a device a string to run is not"* -- and an
  action table is that list of verbs, per app, written once.

That last one is the reason to do this now rather than later. The alternative
is a bespoke command surface per app, bolted on afterwards, describing the
same actions a fourth time.

### What it removes

`apps/todo.c`, `calendar.c`, `edit.c` and `ide.c` currently share, by copy:
a `do_action` switch, two or three `TbMenu` tables, a key switch that maps
chords onto the same actions, and a help string repeating all of it. Four
apps × four descriptions. After: four apps × one table each, and the
duplicated *mechanism* moves into `toolbar.h` and the kernel.

## Migration

The API version moves once (**v22**), and every `.capp` rebuilds -- routine.
An app that registers no action table keeps working exactly as it does now:
the shell falls back to `capp_info.help` and the app's own key switch. That
matters because Mines, Pinball, Claude, Web, Photo, Stocks, Screen, Files,
Explorer, cat and grep should not all have to change on the same day.

Order:
1. Key codes and the fn range; shells move their four chords off ctrl. This
   alone fixes the live `ctrl-S` collision and is worth shipping by itself.
2. `CappAction` plus `api->action_*`; `toolbar.h` reads the table.
3. Convert Todo, Calendar, Edit, IDE -- the four that already have actions.
4. Help screen renders from the table, with fn/opt rows supplied by the kernel.
5. The console and the voice verbs gain `action_invoke`.

Steps 1 and 2 are independent; 3 onwards are one app at a time.

## What this does not do

- **No user-remappable ctrl chords.** `hotkeys.c` already does that for opt,
  where it belongs: rebinding "save" per app is a preference nobody has asked
  for and a table to store per app.
- **No chords on the modifier keys themselves.** Shift stays shift.
- **No mouse gestures in the table.** Clicks are positional; actions are not.
