# Accounts: more than one person on one server

2026-10-06. Approved in conversation: two people now, at most three devices
for the foreseeable future. No database.

## What it is

- `CARDOS_STATE/accounts.json`: users (name, PBKDF2 password hash, admin
  flag) and devices (a SHA-256 of the device's token, its owner, a label).
- Every request is somebody's: a device by its bearer token, a browser by
  its cookie (which now names the user and is signed with that user's
  password hash, so changing a password signs that user out everywhere).
  `accounts.current()` is that user for the rest of the request, held in a
  thread-local -- the server is one thread a request.
- Each user's files live under `CARDOS_STATE/users/<name>/`: `google.json`,
  `toggl.json`, `ids.json`, `daily.json`, `notes/`, `photos/`, `music/`.
  In-memory caches of the same (Google's access token and short ids,
  Toggl's responses, Dashboard Link's job queue) are kept per user.
- Shared: the Chat room (`chat.json`), app and firmware updates, voice,
  rendering, the Claude helpers (talk about a document, MIDI, daily) --
  which run on the owner's Claude login.
- Admin only: the repo-editing Claude terminal and Build (`/chat*`), the
  Claude sign-in on the dashboard.

## Migration

On start, with no `accounts.json`, the server makes one: the owner
(`CARDOS_OWNER`, default `alex`) with `DASH_PASSWORD` as the password, an
admin, and `CARDOS_TOKEN` as the owner's first device. The owner's files
move into `users/<owner>/`. Nothing on any device changes.

With no `accounts.json` at all -- a laptop server, the test suite --
everything behaves exactly as before: one user, files at the top.

## The dashboard

Sign-in asks for a name as well as the password. A Devices card lists
yours, makes a token for a new one (shown once: it goes in
`/config/claude.token` on that device's card) and removes one. An Accounts
card, for an admin, adds and removes people. Everyone can change their own
password.

A second person's Google sign-in needs their address added as a test user
on the OAuth consent screen while it is in Testing.
