# Dashboard Link

_Moved from CLAUDE.md on 2026-10-27._

**Dashboard Link: the device from the dashboard** (2026-10-01; was Remote
Files). While `apps/dashlink.c` (Net) is open, the dashboard can browse
and change the card and run console lines on the device. The droplet
cannot reach the device, so the device polls `/files/poll`, one request
carrying the last answer in and the next job out (`server/files.py` has
the protocol). File data is base64 in 3 KB chunks because the app HTTP
layer is text; an upload goes to NAME.part and is committed at the end. A
console line (`POST /dash/term`) is the `sh` job: `api->shell` (API 34)
runs it through the console's own pipeline with `con_capture` on, so its
output fills a buffer and nothing is drawn over the app on screen; lines
that would take the screen (launch, desk, run, ...) are refused, and so is
an app on PATH that wants it. Closed, nothing is reachable.
