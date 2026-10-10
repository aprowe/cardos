# The server's general-purpose store

2026-10-09. Asked for by the owner when Jar Factory's server steps came up:
"a general purpose redis server ... so we don't need new Python code for every
service". The server had grown one module per feature (toggl, notes, photos,
music, midi, msg, m5hub), each with its own JSON file, cache and routes. New
features keep their data in one store through routes every app shares; a new
server module is written only for what truly needs code on the server.

Four pieces, all in `server/`, none Jar-specific:

| Module | What | Routes |
|---|---|---|
| `kv.py` | values, counters, queues in sqlite | `/kv/*`, `/q/*` |
| `people.py` | accounts as friends see them, last seen | `/people`, `/people/name` |
| `ask.py` | Claude, answering JSON of a given shape | `/ask` |
| `sign.py` | the server's P-256 signing key | `/sign/pubkey` |

Not Redis: one sqlite3 file (stdlib, `CARDOS_STATE/kv.sqlite3`, 0600, WAL) is
durable, needs no second process on the droplet, and the load is a few
devices. One connection behind one lock.

## Namespaces

| `ns=` | Whose | Written by |
|---|---|---|
| `me` | the signed-in person's | them |
| `me/NAME` | the person's data for app NAME | them |
| `app/NAME` | app NAME's, shared by everyone (a game's world) | anyone signed in |
| `srv/NAME` | the server's, readable by everyone | server code only (403 from a route) |

NAME is `[a-z0-9_-]{1,24}`. Without accounts the server is one person (`-`).
Keys are `[A-Za-z0-9._:/@+-]{1,128}`: no whitespace, so listings are lines.
`app/` is writable by every signed-in person: what must not be forged goes in
`srv/` (or is signed).

## Limits

| | |
|---|---|
| A value or a message | 16 KB (`VALUE_MAX`) |
| Per person | 1 MB of keys + values + queued messages they wrote (`CARDOS_KV_QUOTA`); server code counts against no one |
| A queue | 1000 messages waiting |
| `/kv/list` | 200 lines by default, 1000 at most |
| `/q/peek` | 8 messages by default, 50 at most, 32 KB of bodies (always at least one) |
| `/ask` | 20 a day per person through the route, 50 a day for server callers (one count; `CARDOS_ASK_ROUTE_DAILY`, `CARDOS_ASK_DAILY`), 4 KB prompt, 2 KB schema, 16 KB answer |

A TTL (seconds) is optional on a value, a counter and a message; nothing
expires without one. Expired rows are invisible at once and deleted at the
next write a minute later.

## The wire format

All routes are `device_or_dash` (the device's bearer or the dashboard's
cookie) except `/sign/pubkey`, which is open. Writes are POST, with the
arguments in the query string and a value or message as the raw body. A
refusal is one line, `error WHY\n`, and the status says which:

| Status | Means |
|---|---|
| 400 | a bad argument (namespace, key, queue name, number, missing `upto`) |
| 403 | not signed in; another person's queue that has not allowed you; `srv/` written from a route |
| 404 | no such key; no such person; no such ask id (or not yours) |
| 409 | `incr` on a value that is not a decimal integer |
| 413 | a value or message over 16 KB |
| 429 | `/ask`: the day's asks are used |
| 503 | `/ask` on a server without Claude; `/sign/pubkey` without `cryptography` |
| 507 | over the person's quota; the queue is full |

### Values

    GET  /kv/get?ns=NS&key=K               200, the value's bytes exactly (application/octet-stream)
    POST /kv/put?ns=NS&key=K[&ttl=S]       body: the value -> "ok\n"
    POST /kv/del?ns=NS&key=K               "ok 1\n" deleted, "ok 0\n" was not there
    GET  /kv/list?ns=NS[&prefix=P][&max=N] one line a key, in key order:
                                           KEY \t SIZE \t EXPIRES \n   (EXPIRES unix s, 0 never)
    POST /kv/incr?ns=NS&key=K[&by=N][&ttl=S]
                                           "N\n", the new value. Missing is 0; by may be negative;
                                           ttl sets the expiry, otherwise an existing one is kept.
                                           /kv/get reads a counter as its decimal text.

### Queues

A queue belongs to a person and has a name, `[a-z0-9][a-z0-9._-]{0,31}`.
Anyone may push to their own; another person only if the owner has allowed
them on that queue. Messages stay until the owner acks them (or their TTL
ends); ids are unique across the server and only grow.

    POST /q/push?q=Q[&to=PERSON][&ttl=S]   body: the message -> "ok ID\n"  (to: yourself if absent)
    GET  /q/peek?q=Q[&after=ID][&max=N]    your queue's oldest messages with id > after (default 0).
                                           Each is a header line, then the body, then a newline:
                                             ID \t FROM \t AT \t SIZE \n  <SIZE bytes> \n
                                           FROM is the pusher's account name ("-" for the server),
                                           AT unix seconds. Nothing waiting: an empty body.
    GET  /q/len?q=Q                        "N\n", your messages waiting
    POST /q/ack?q=Q&upto=ID                "ok N\n": your messages with id <= ID gone
    GET  /q/allow?q=Q                      who may push to your queue Q, one name a line
    POST /q/allow?q=Q&who=NAME[&off=1]     "ok\n"; NAME allowed (off=1: no longer). NAME "*" is
                                           everyone signed in.

Peek does not take: read, act, then ack up to the last id handled, so a
device that dies half way reads the same messages again. The allowlist is
itself a value, `q.allow/Q` in the owner's `me` namespace (one name a line),
and counts against their quota.

### People

    GET  /people        one line a person, by name:  NAME \t DISPLAY \t LAST_SEEN \n
                        DISPLAY is the account name until they choose one; LAST_SEEN
                        unix seconds, 0 never. Without accounts: empty.
    POST /people/name   body: a display name -> "ok\n"  (one line, 16 characters;
                        empty goes back to the account name)

Last seen is recorded for every authenticated request, written at most once
a minute per person.

### Asking Claude for a shape

Server code calls `ask.ask_shape(chat, prompt, schema, user=..., limit=...)`:
the schema goes into the prompt, the first JSON value is read out of the
answer, checked against the schema, and on a misfit asked once more with the
reasons. The schema language is a strict subset of JSON Schema (`type`,
`enum`, `const`, `properties`, `required`, `additionalProperties`, `items`,
`minItems`, `maxItems`, `minLength`, `maxLength`, `pattern`, `minimum`,
`maximum`); any other keyword is refused rather than ignored.

    POST /ask           body: line 1 the schema as one line of JSON, then the prompt -> "ID\n"
    GET  /ask?id=ID     "pending\n" | "ok\n" + the value as one line of JSON + "\n" | "error WHY\n"

POST then poll, as `/midi/compose` and `/chat` are, because an answer takes
up to a few minutes and the device's loop must not wait on it.

### Signing

    GET  /sign/pubkey   130 hex digits and "\n": the 65-byte uncompressed P-256 point
                        (04 || X || Y). Open.

`sign.sign(data)` returns 64 bytes, r || s big-endian, ECDSA over SHA-256 of
the bytes. A device pins the public key and checks with the raw point and
the raw numbers. The key is `CARDOS_STATE/sign_key.pem`, made on first use;
replacing it orphans every signature already given out. No route signs bytes
for a client.

## How Jar Factory will use it (steps 5 and 6)

- **Friends**: `/people` for the list and last online; a friend code is the
  account name, the 8-character name is `/people/name`. "Both add each other
  before gifts flow" is each allowing the other on `jar.gifts`
  (`/q/allow?q=jar.gifts&who=sam`).
- **Gifts**: a small server function (the one place that needs code) takes the
  item from the sender's `me/jar` store, re-signs it with provenance, and
  pushes it to the friend's `jar.gifts` queue as the server, so the device
  never pushes an item it could have forged. The device confirms the sender's
  loss only when that call answers. Thank-yous are plain `/q/push` to the
  sender's `jar.thanks`.
- **Daily stock**: server code calls `ask_shape` with the item schema, checks
  what the schema cannot (sprites), signs each item, and keeps the batch in
  `srv/jar` or the person's `me/jar` with a TTL of a day; item numbers come
  from `kv.store().incr("srv/jar", "next_id")` (server code; a route may not
  write `srv/`).
