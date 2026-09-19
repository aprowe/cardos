# The card layout — a Unix-shaped root

Approved 2026-09-19. Records the decisions; the code says how.

## What

The card root had grown by accretion: `/todo`, `/shots`, `/asm`, `/settings`,
`/calendar.cache`, `/claude.key`, `/claude.token`, `/cardos`, `/update` and a
scatter of `.txt` files, all at the top. This puts every path under one of six
folders with a Unix meaning, so a reader who knows `~/.config` and `/var`
knows where to look, and so `rm -rf /cache` is always safe.

| Folder    | Holds                                                     | Was                                  |
|-----------|-----------------------------------------------------------|--------------------------------------|
| `/sys`    | what the firmware needs and nobody edits: OTA staging, the compiler's sources, chain-boot images | `/cardos`, `/update` |
| `/config` | what a person edits: `hotkeys.txt`, `claude.token`, `claude.key`, `stocks.txt` | `/settings`, root files |
| `/cache`  | rebuildable: logs, renders, transcripts, `calendar.cache`, `todo/*.cache` | `/cache`, root files, `/todo/*.cache` |
| `/home`   | the user's own files: notes, `shots/`, `asm/`              | root files, `/shots`, `/asm`         |
| `/apps`   | the `.capp` binaries in the folders the launcher shows     | `/desktop`                           |
| `/var`    | app state that is neither config nor cache: `todo/lists`   | `/todo/lists`                        |

`/firmware` stays where it is: it predates CardOS and M5Launcher reads it.

## Decisions

| | Decision | Why |
|---|---|---|
| Names | `/apps`, not `/local/<app>` | The launcher *is* the apps tree -- its folders are the tree's folders and `PATH` walks it. One file per app is what makes dragging one in over WebDAV work. |
| `/var` apart from `/cache` | Deleting `todo/lists` loses your lists | "Anything in `/cache` can go" is only true if state lives elsewhere. |
| `/config`, not `/settings` | Matches `~/.config`; nobody had used `/settings` yet | |
| One header | `CAPP_HOME`, `CAPP_APPS`, ... in `capp.h` | Apps cannot read the environment, and the kernel and every app must agree. No API bump: they are macros, not table entries. |
| `HOME=/home` | Edit, Files, Explorer and the IDE open there | The root is system folders now; a person's files are in `/home`. |
| Migration at boot | `fs_migrate_layout()` runs once per boot, before `fs_ensure_layout()`, and renames each old path to its new one when the new one does not exist | Existing cards keep everything. A rename on FAT is a directory-entry edit, so a 1 MB cache moves in a millisecond and a failed move leaves the old path intact. |
| Saved `PATH`/`HOME` | An NVS `PATH` naming `/desktop`, or `HOME=/`, is replaced by the new default at load | The old value would point at nothing. |

## Not done

Apps cannot ask where `/home` is at runtime; the macro is compiled in. If the
layout ever needs to move again it is a rebuild of every app, which is the
same cost as any other `capp.h` change.
