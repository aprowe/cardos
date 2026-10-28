# Notes (Plan)

_Moved from CLAUDE.md on 2026-10-27._

**Its server code is `apps/notes/server/`** (a plugin, server/plugins.py; was `server/notes.py`), with the dashboard's Notes page in `apps/notes/server/dash/`; tests `python -m apps.notes.server.tests.test_notes`.

**Notes** (2026-10-01). Kept on the server (`apps/notes/server`, JSON in
CARDOS_STATE/notes, each with an FNV-1a hash of its text), edited in the
dashboard's Notes view or on the device. `apps/notes/notes.c` (Plan) keeps each
as /home/notes/NAME.md, opened in Edit, and syncs by hash against
/home/notes/.sync: changed on one side goes to the other, changed on both
keeps both (NAME (conflict)), deleted on one side is deleted on the other
unless it changed there. Host-tested over an in-memory card and server
(`apps/notes/test/test_notes.c`). G0 held on its list says a new note; `m` there, or
`n` in Memo, sends a voice memo to /notes/audio, which Whisper turns into a
note. That needed `api->http_upload` (API 36): an app's body is a string,
and a WAV is not.
