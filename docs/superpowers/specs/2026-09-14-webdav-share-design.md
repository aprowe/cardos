# Share — the card as a network drive over WebDAV

Approved 2026-09-14. Records the decisions; the code says how.

## What

`share` puts the SD card on the LAN as a WebDAV server, so the PC mounts it
as a drive letter (`net use X: http://192.168.1.23/` on Windows, Connect to
Server in Finder, `gio mount dav://` on Linux) and files move by dragging.
The card stays readable and writable by CardOS throughout: no card-pulling,
no mode the rest of the OS has to stop for.

The use it exists for is the development loop — a `.capp`, a document, a
`claude.token` dropped onto `/desktop` from Explorer — and the occasional
pull the other way. It is a thing you turn on and off, not a service that
is always listening.

## Why WebDAV and not SMB

SMB is what "network drive" means on Windows, and it was the first question.
Modern Windows and macOS have SMB1 off, so it would have to be SMB2, and SMB2
requires NTLMv2 authentication (MD4, HMAC-MD5, RC4), signing negotiation and
a state machine that runs to several thousand lines of careful C against a
Wireshark trace. WebDAV is HTTP with six extra verbs, needs no crypto, and
every desktop OS mounts it natively. It gives up appearing in Network
Neighbourhood; the device shows its URL instead.

FTP was the cheaper option still, and fails the test: Explorer browses
`ftp://` but will not map it as a drive.

## Decisions

| | Decision | Why |
|---|---|---|
| Protocol | WebDAV class 1 (RFC 4918) with a fake `LOCK`/`UNLOCK`, advertising `DAV: 1,2` and `MS-Author-Via: DAV` on `OPTIONS` | Windows' Mini-Redirector wants class 2 advertised before it will write, and sends `LOCK` before `PUT` from some applications. A lock that always succeeds with an opaque token and is never enforced satisfies it; there is one client. |
| Port | 80 | Windows maps `http://IP/` without fuss and is fussy about `http://IP:8080/`. Nothing else on the device listens. |
| Root | The whole card at `/` | The dev loop touches `/desktop`, `/cache`, `/claude.token` and the root; a sub-tree would need an exception list that grows forever. |
| Authentication | None | Windows blocks Basic auth over plain HTTP by default, and HTTPS on this device costs 34 KB of headroom per handshake and a certificate the PC would have to trust. Same boundary as `webproxy.py`: the LAN. Mitigated by being a mode — on while you want it, off otherwise — not by a password. |
| Connections | One at a time, keep-alive, listen backlog of 4 | Windows opens two to four connections in parallel; the extras wait in the backlog and are served in turn. One connection's buffers are what the heap has room for. A second concurrent client would be the second TLS session all over again. |
| Where it runs | Its own FreeRTOS task, created on `share on`, deleted on `share off`, like `httpq.c` | Blocking socket code is a fraction of the size of a state machine driven from the 5 ms tick, and Esc stays responsive while a file is being written. The task never touches the display (the `bg.c` rule): it posts one-line log entries to a queue the shell drains. Stack is heap, ~8 KB, spent only while sharing. |
| Card access | Through `fs.h`, from the second task, with a mutex added in `fs_fat.c` around the open-file slot table | FatFs is built reentrant (`CONFIG_FATFS_TIMEOUT_MS`), so the VFS layer is safe from two tasks; the only unguarded thing is `s_open[]`, eight pointers. Going around `fs.h` to the VFS directly would work today and break the abstraction the file exists to keep. |
| Timestamps | `FsStat` gains `mtime`; `getlastmodified` and `creationdate` both come from it | FAT keeps one useful timestamp. Files written from the PC get the device's clock, which is NTP or 1980 — Windows tolerates either. |
| Names | FAT long names, percent-encoded UTF-8 in hrefs, decoded on the way in; `..` and anything `path_normalize` rejects → 403 | The card is code page 437 ANSI on the API, so non-ASCII names are best-effort and documented as such. Traversal is the one input the server must not trust. |
| PROPFIND | `Depth: 0` and `1` served, `infinity` → 403; the seven properties Windows and Finder actually read; the response streamed per entry via `fs_readdir`, never assembled | The properties: `resourcetype`, `getcontentlength`, `getlastmodified`, `creationdate`, `displayname`, `getcontenttype`, `getetag` (size + mtime). Anything else requested comes back in a 404 `propstat`, which is what clients expect. Streaming keeps a 300-file `/desktop` from needing 300 × ~400 bytes of XML in RAM. |
| PROPPATCH | 207 saying every property was set; nothing is | Windows sets Win32 attributes after every write. Refusing makes Explorer report the copy failed after the bytes are already on the card. |
| Transfers | `GET`/`PUT` streamed through one 4 KB buffer; `Content-Length` required on `PUT`, chunked → 411; `Expect: 100-continue` honoured; `Range` ignored (200 with the whole file, which the RFC permits) | The 1 KB chunk `http.c` uses is sized for TLS on a busy heap; this runs without TLS and 4 KB halves the SD round trips per file. Files larger than the card's free space fail at the write, and `PUT` answers 507. |
| Other verbs | `HEAD`, `MKCOL`, `DELETE` (recursive for folders), `MOVE` (rename), `COPY` for files; `COPY` of a folder → 403; unknown → 405 with `Allow` | These are what Explorer and Finder issue for the things people do in them. Folder copy within the share is the one operation that will fail, and it is documented in the app. |
| Windows' 50 MB limit | Documented, not worked around | The WebClient service refuses files over `FileSizeLimitInBytes` (50 MB by default) on the *client*. Nothing on the card is that large today; the registry key is in the Share app's help text. |
| Discovery | None; the device prints and shows `http://IP/` | mDNS `_webdav._tcp` would get Finder's sidebar and nothing on Windows, for a component of its own. Later, if ever. |
| Turning it on | Console: `share` starts it and prints the URL, `share off` stops it, `share` again reports. App: **Share** (`apps/share.c`, `CAPP_NEEDS_NET`) shows the URL large, a live log of requests, and stops the server when you leave it | The console form outlives the command because the dev loop is "start it, go do things"; the app form is a mode with a visible exit because the launcher is the daily shell and a share left running by accident should not be possible from there. Both call the same `share_start`/`share_stop`. |
| API | `share_start`, `share_stop`, `share_status`, `share_take_log` added to the app table, version 23; every `.capp` rebuilt by `tools/build_apps.py` as always | The socket work belongs in the kernel; the app is a hundred lines of UI over four calls, which is the pattern Files and Web already follow. |
| Memory guard | `share_start` refuses below 40 KB free heap, in words, like `http.c` does | Task stack 8 KB, 4 KB I/O buffer, 2 KB request line and headers, 1 KB for a response header and one PROPFIND entry: ~15 KB while sharing, measured before the commit, plus what lwIP allocates per connection. Refusing up front leaves the device able to say why. |

## Shape

```
kernel/net/dav.c      portable: parse a request line and headers, decode and
kernel/net/dav.h      check a path, format one PROPFIND <response>, RFC 1123
                      dates, the method table. Talks to files through a small
                      callback struct so the host suite runs it against a fake
                      tree. No sockets, no FreeRTOS.
kernel/net/share.c    device-only: the task, the listening socket, one
kernel/net/share.h    connection at a time, streaming GET/PUT through fs.h,
                      the log queue. Glues dav.c to lwIP and fs.h.
kernel/fs/fs_fat.c    a mutex around s_open[]; mtime in FsStat.
src/main.c            the `share` command.
kernel/app/capp.h     four entries, version 23.
apps/share.c          the app.
```

`dav.c` is the part with logic and is written to run on the host; `share.c`
is the part with sockets and is deliberately thin.

## What it costs the rest of the OS

Nothing when off: no task, no socket, no buffers. When on, ~15 KB of heap
and whatever SD bandwidth the PC is using, contended fairly with the shell
through FatFs' own lock. A `.capp` being overwritten while it runs is fine —
the loader copies the whole ELF into RAM at load and never reads the file
again. The launcher's icon cache reads `/desktop` on its next repaint and
sees the new file.

## Out of scope

Authentication and HTTPS; more than one connection at a time; `Range`;
`If-*` conditional requests; real locking; folder `COPY`; mDNS or NetBIOS
discovery; USB mass storage (its own, smaller spec, for the wired case);
anything that makes the share survive the Share app being closed.

## Testing

- Host (`test/test_dav.c`): request parsing including a header split across
  reads; path decoding and every traversal shape rejected; `Depth` handling;
  one `PROPFIND` response for a file and for a folder against a fake tree,
  compared to a fixture; RFC 1123 date formatting on and off DST; the
  `LOCK` token round trip; every method in the table answered.
- Device (`tools/test_share.py`): a Python WebDAV client run against the
  device's IP — `OPTIONS`, `PROPFIND` at both depths, `MKCOL`, `PUT` and
  `GET` of a 1 MB file compared byte for byte, `MOVE`, `DELETE`, a traversal
  attempt refused — and prints the transfer rate, which goes in the commit
  message.
- By hand, once: map the drive in Explorer on Windows 11 Home, copy a
  `.capp` in, launch it on the device; connect in Finder if a Mac is nearby.
