# App commands: one API for the GUI, the console, voice and AI

2026-09-23. Decided in conversation; this is the record.

## What it is for

"Carlos, make a todo to fix car" should add a task to Todo -- without Todo
opening, without anyone scripting it -- because Todo said it can do that. The
same thing from the console (`do todo add "fix car"`), from an AI over MCP,
and from Todo's own "New" menu, all running one function. Apps declare what
they can do; the OS keeps the list; everything that wants to ask an app to do
something reads that list.

## What exists, and what is missing

Much of the shape is already here (`kernel/app/capprun.c`, `apps/toolbar.h`):

- `CappAction {id, label, menu, key, action}` tables feed the fn-chords, the
  fn-b menus and the help panel from one list.
- `capprun_action_invoke(def, id)` runs one by id; the console's `do ID` and
  the voice/agent `action` verb (`kernel/sys/agent.c`) call it.

Four things stop that being the API:

1. **No arguments.** "add" cannot say *what*. So a GUI action and an API call
   differ: Todo's New opens its own draft view to gather the text.
2. **Focused app only.** `do` and `action` reach whatever is on screen.
3. **Not readable without running.** The table is installed by `capp_main`
   through `api->ui`, so nothing can list an app's actions without starting it.
4. **Voice does not know.** `server/voice.py`'s prompt lists seven verbs and
   not `action`, and has no catalog of what any app can do.

## The model

**A command is a function with typed arguments and no UI.** A GUI action is a
command plus, when arguments are missing, a way to gather them.

```
todo.add(text)               the command: typed args, no screen, does the work

GUI    New (fn-n) -> OS prompt "text?" -> todo.add("fix car")
CLI    do todo add "fix car"            -> todo.add("fix car")
Voice  "carlos, make a todo..." -> Claude picks from the catalog
                                        -> todo.add("fix car")
MCP    tool todo_add {text}             -> todo.add("fix car")   (later phase)
```

Keys that are not commands -- move the cursor, scroll -- stay plain key
handling and are not exposed. An app with nothing to expose (Mines) declares
no commands and is otherwise untouched.

### The contract (API 30)

```c
typedef struct {
  const char *name;      /* "text" */
  uint8_t     type;      /* CAPP_ARG_TEXT | _INT | _BOOL | _CHOICE */
  const char *about;     /* "what the task says"; for CHOICE, "a|b|c" */
} CappParam;

/* CappAction grows; every existing table still compiles (new fields zero). */
typedef struct {
  const char *id, *label, *menu;
  uint8_t key, action;
  const char      *about;    /* one line for the catalog: "add a task" */
  const CappParam *params;   /* NULL for none */
  uint8_t          nparams;
  uint8_t          cmd;      /* CAPP_CMD_* below; 0 = a GUI action only */
} CappAction;

#define CAPP_CMD_YES   0x01  /* exposed as a command */
#define CAPP_CMD_NET   0x02  /* may need the network: can answer PENDING */
```

`CappInfo` (the descriptor the loader reads without running anything) gains
`const CappAction *commands; uint8_t ncommands;` -- the same table the app
installs for its UI, pointed at twice. That is what makes the catalog
readable without running the app, and what keeps GUI and API from drifting:
there is one table.

`CappUi` gains the handler, and `CardApi` two calls:

```c
/* argv holds the arguments in declaration order, already checked against
 * the types. Text for the caller goes in out. 0 done, <0 failed (out says
 * why), CAPP_CMD_PENDING: finishing from tick, then api->command_done(). */
int (*command)(void *state, int action, int argc, const char *const *argv,
               char *out, size_t n);

int  (*headless)(void);                       /* running with no screen? */
void (*command_done)(int rc, const char *out);
```

### Where a command runs: one instance per app

- **The app is open** (Todo on screen): the command goes to that instance.
  Same login, same cache, same list in memory; nothing loaded twice.
- **It is not**: the OS starts it headless -- the same `capp_main`, with
  `api->headless()` true -- runs the command, and releases the slot. An app's
  start splits into *local* (read the cache: milliseconds, every command needs
  it) and *online* (token, sync: seconds, only the GUI and a real sync). Todo
  already works this way -- sync begins on the first tick, not in
  `capp_main` -- so headless Todo is two small file reads.
- **The card is the source of truth.** Instance memory is a working copy;
  that is what makes a GUI instance and a headless one interchangeable. A
  headless `todo.add` writes the cache and marks the item dirty, exactly as
  an offline add does; the next sync pushes it.
- **PENDING**: a command that needs the network returns it, and the OS keeps
  ticking the (headless) instance until `command_done` or 20 s.
- Headless costs the app's code and data for as long as it runs. Loading can
  fail when executable RAM is fragmented (a 7.6 KB largest block has been
  seen); the command then fails saying so. Keeping an instance warm after a
  command is left out until it is measured.

### The catalog

- **On the device**, the icon scan (`capprun_load`) already loads each app's
  ELF to copy its name and icon; it copies the command table's text too, into
  `/cache/commands.txt`, one command per line -- not RAM. Rewritten when the
  set of apps changes.
- **On the server**, `tools/build_apps.py` reads the same table out of each
  built ELF (pointers are link-time addresses into `.data`, which the build
  can follow) into `build/apps/commands.json`. The server has it without
  asking the device, which is what voice and MCP need.

### The ways in

- **Console**: `do` lists every app's commands; `do todo` one app's;
  `do todo add "fix car"` runs one, with quotes for text and tab completion
  from the catalog.
- **Voice**: the command prompt gets the catalog and one more line form,
  `do APP COMMAND ARGS`. The device checks the line against its own catalog
  -- the app exists, the command exists, the argument count and types match
  -- before anything runs. The LLM still only chooses from a declared list.
- **GUI**: a menu entry or chord for a command with parameters and no
  arguments gets an OS text prompt (an overlay like the file picker), then
  runs the same command. Apps stop drawing their own prompts.
- **MCP** (later): the droplet cannot reach into the device behind a home
  router, so a call waits for the device's next check-in. Its own phase.

## Phases

1. **Contract and catalog.** The types, API 30, `build_apps.py` extraction
   (tested), `/cache/commands.txt` at scan, `do` listing.
2. **Execution.** `capprun_command` (live instance or headless), `headless()`,
   `command()`, PENDING. Todo first: `add(text)`, `list`, `done(title)`. The
   console runs them.
3. **Voice.** The catalog in the command prompt; the `do` line form; checking
   on the device.
4. **GUI prompts.** The OS text prompt; Todo's New uses it and its draft view
   goes.
5. **MCP.** Later.

## Tests

Host: argument checking against a declared table (count, int, bool, choice,
quoting); the `do` line parser; the catalog line format. Python: the ELF
extraction against a real built app. Device: `do todo add "x"` with Todo
closed, then open Todo and see it; the same with Todo open, and no second
load (`mem` before and after).
