# The remote proxy: modules, secrets, and a server you can expose

2026-09-18. Approved in outline; this is the design to implement against.

## What is wrong with what exists

`tools/webproxy.py` is 685 lines that mix things which are safe to put on the
public internet with things that must never leave the laptop. The mixing is not
stylistic. Three specific problems:

**The chat endpoint is a remote shell.** `tools/chat.py` runs the Claude Code
CLI with `--permission-mode acceptEdits` and `--add-dir <repo>`, in the repo.
Anything that can reach `POST /chat` can edit this source tree. On a home
network behind a shared token that is a bounded risk the file warns about. On a
host with a public address it is arbitrary file write as a service.

**The device carries an Anthropic API key in the clear.** `kernel/sys/agent.c`
posts straight to `https://api.anthropic.com/v1/messages`, reading the key from
`/claude.key` on the microSD card (`tools/putkey.py` puts it there). The card is
removable and the key is full-account. There is no per-device revocation: losing
one card means rotating the key everywhere. This is the single largest secret
cleanup available and it is why the server exists.

**Secrets arrive as command-line flags.** `--token SECRET` is visible in `ps` to
every user on the box, and in shell history. `/status` reports the repo path and
whether an API key is present, and is only behind the token when a token happens
to be set.

Two features are being removed rather than fixed: the Chrome renderer (`/render`
and everything under it, including the pixel-font pipeline and the RGB565 row
encoder) and the Build app's server half. Neither is wanted, and dropping the
renderer removes the headless-Chrome dependency from the deployable path
entirely.

## Shape

One codebase, a small core, and feature modules that a **profile** selects.

```
proxy/
  core.py          the HTTP server, routing, auth, config, rate limiting
  config.py        environment -> settings, and the refusal to start without them
  modules/
    chat.py        conversational Claude, via the Anthropic API      [server, dev]
    voice.py       whisper: audio in, text out                       [server, dev]
    update.py      manifest, artifact download, artifact upload      [server, dev]
    shot.py        screenshots from the device                       [dev]
    screen.py      the desktop, streamed                             [dev]
    repo.py        Claude Code in this repo, with edit permission    [dev]
```

A profile is a list of module names. `server` loads `chat`, `voice`, `update`.
`dev` loads all of them. **A module outside the profile is never imported**, so
the deployed server has no code path to a route it does not serve. That is the
enforcement; a runtime flag would be a thing to forget to set.

Modules register routes with the core rather than the core knowing about them:

```python
# proxy/modules/voice.py
ROUTES = [("POST", "/voice", handle_voice)]
REMOTE_SAFE = True
```

The core refuses at startup to load a module whose `REMOTE_SAFE` is false into
the `server` profile, so `repo.py` cannot be added to the server profile by a
one-word edit.

## Secrets and identity

**Everything comes from the environment. Nothing comes from a flag.**

| Variable | Used by | Meaning |
|---|---|---|
| `ANTHROPIC_API_KEY` | `chat` | The only place this key exists once the device stops holding one |
| `CARDOS_DEVICE_TOKEN` | all routes | What a device presents. Read-only in effect: it may chat, transcribe and *download* updates |
| `CARDOS_ADMIN_TOKEN` | `update` upload | What a release presents to *push* an artifact |
| `CARDOS_PROFILE` | core | `server` or `dev` |

**Two tokens, not one.** You chose a single shared secret, and for the device
that is what this is. But upload is a different privilege from download: a
device that flashes what the server hands it means whoever can upload can run
code on every device you own. Splitting them costs one more environment variable
and means a stolen SD card cannot push firmware. If the admin token is unset,
upload is not merely denied — the route is not registered.

Rules the core enforces at startup, as failures rather than warnings:

- `server` profile with no `CARDOS_DEVICE_TOKEN`, or one shorter than 32
  characters, is a refusal to start. Today a missing token only prints a notice.
- `chat` with no `ANTHROPIC_API_KEY` is a refusal to start.
- Tokens are never logged, never echoed to `/status`, and never included in an
  error body. `/status` behind the token reports liveness and nothing about the
  host.

Comparison stays `hmac.compare_digest`. Failures answer `401` with
`WWW-Authenticate: Bearer`, not today's `403`.

## The device stops holding an API key

`kernel/sys/agent.c` changes its URL from `api.anthropic.com` to
`$PROXY/v1/messages` and its auth header from the Anthropic key to the device
token. The request body it already builds from the on-card conversation
(`kernel/sys/chatlog.c`) is Anthropic-shaped, so the server can forward it with
its own key and hand back the reply unchanged. The device keeps its conversation
on the card exactly as now.

Consequences, all of them wanted:

- `/claude.key` is deleted from the card and `tools/putkey.py` is retired.
- Revoking one device is deleting one token, not rotating an account key.
- Spend is visible and limitable in one place.
- The device no longer needs to know whether the credential is an API key or an
  OAuth token, which is a distinction `read_key()` currently carries.

`apps/claude.c` keeps talking to `/chat` as it does now.

## Chat, on the server

The `repo` module keeps the Claude Code subprocess for the laptop. The `chat`
module is new and is the Anthropic Python SDK, not a subprocess:

- Model `claude-opus-5`, adaptive thinking, streaming so a long answer cannot
  hit an HTTP timeout.
- No tools, no filesystem, no code execution. The system prompt says what the
  device is; the model answers.
- Conversation state is server-side, keyed by the id the device already receives
  from `POST /chat` and polls with `GET /chat?id=`. A conversation is a list of
  messages with a cap on turns and total tokens, evicted oldest-first, held in
  memory only. `GET /chat/new` drops it, as now.
- The three-call protocol (`POST /chat` -> id, `GET /chat?id=` -> pending or the
  answer, `GET /chat/new`) is unchanged, because it exists so a two-minute
  request does not freeze the device's cooperative loop. That reasoning is
  untouched by where the model runs.

## Update, with upload

Today the manifest is produced by reading the build tree, which a server does not
have. So the server gains a store and the release step becomes a push.

```
GET  /update                 manifest: firmware sha + per-app hash    device token
GET  /update/firmware        the image                                device token
GET  /update/app/<name>      one .capp                                device token
PUT  /update/firmware        upload an image                          admin token
PUT  /update/app/<name>      upload one .capp                         admin token
```

The store is a directory the server owns: artifacts plus a generated
`manifest.json`. An upload is written to a temporary file, hashed, and moved into
place only once complete, so a device fetching mid-upload cannot get half an
image. The manifest is regenerated after each successful move.

**What protects the device from a bad artifact**, in order:

1. The admin token gates who may upload at all.
2. `kernel/app/appimage.c` already refuses an image that is not a valid
   Xtensa-S3 image for this partition, and the host suite covers truncation,
   wrong chip, absurd segment counts and oversize images.
3. The device already checks what it downloaded against the manifest, and this
   design relies on it rather than adding anything: `kernel/net/update.c`
   hashes a downloaded `.capp` into a temporary file and refuses to let it
   replace the live one unless the hash matches (`hash_file(tmp, &hash) != 0
   || hash != a->hash`), and `launcher_boot` verifies the firmware's SHA as it
   lands. A download truncated by a dropped connection therefore cannot reach
   an OTA slot.
4. The existing one-shot rollback home stays: a bad image boots once and falls
   back to `factory`.

Artifact signing is deliberately out of scope. It would mean a key on the device
and a signing step in the release, and with the admin token separate the
remaining exposure is "someone with the admin token can flash your devices",
which is the same trust you already place in your own laptop. Revisit if the
server is ever shared.

## Transport and abuse

The proxy binds `127.0.0.1` and a reverse proxy terminates TLS. The device
already does HTTPS with a certificate bundle, so it needs only
`set PROXY=https://your.host`. The proxy does not terminate TLS itself: that
would mean certificate renewal inside this code, which is a solved problem
outside it.

Rate limiting is per token, in the core rather than per module: a request count
per minute and a rolling cap on tokens sent to the Anthropic API. The device is
a cooperative loop that polls, and a bug in it should not be able to spend a
month of budget. Exceeding the cap answers `429` with `Retry-After`.

Bodies keep the existing length refusal, which already rejects a
`Content-Length` larger than a route could want, with a much larger allowance on
the upload routes.

## What gets tested

The host suite is C and cannot reach Python, so these are Python tests run
beside it, extending the shape `tools/test_chat.py` already uses (a stubbed
agent, driven over the real protocol):

- A `server`-profile app has no route for `/render`, `/screen`, `/shot`, and no
  repo module imported.
- Loading `repo` into the `server` profile is refused at startup.
- Missing or short `CARDOS_DEVICE_TOKEN` in the `server` profile refuses to
  start; missing `ANTHROPIC_API_KEY` with `chat` enabled refuses to start.
- Every route answers `401` without a token, including `/status`.
- The wrong token answers `401`; the right one is accepted; comparison is
  constant-time.
- Upload with the device token is refused; with the admin token it is accepted.
- With no admin token configured, the upload routes do not exist.
- An interrupted upload leaves the previous artifact and manifest intact.
- The manifest hash matches the bytes served.
- A conversation is capped and evicts oldest-first; `/chat/new` clears it.
- Rate limiting answers 429 and recovers.
- No test may find a token or API key in any log line or response body.

The device side (`agent.c` pointing at the proxy) is checked on hardware, since
it is the one part the host suite cannot reach.

## Order of work

1. Split `webproxy.py` into `proxy/core.py` plus modules, with the existing
   behaviour intact under the `dev` profile. Nothing new, nothing removed yet:
   a refactor whose test is that the device still works.
2. Delete the Chrome renderer and the Build half.
3. Config and auth: environment-only secrets, startup refusals, `401`, the
   two-token split, rate limiting.
4. The `chat` module against the Anthropic SDK, and the `server` profile.
5. Update store and upload.
6. Point `kernel/sys/agent.c` at the proxy; delete `/claude.key` and
   `tools/putkey.py`.
7. Deploy notes: reverse proxy, environment file, the release push.

Steps 1 and 2 are safe to do now. Step 6 is the one that changes the device, and
wants the hardware back on USB first.

## Deliberately not done

- Artifact signing, per the reasoning above.
- Per-device keys. One device token, as chosen. The hash-on-the-server design
  would be a small change later if a second person ever gets a device.
- Any sandboxed repo access on the server. The repo-editing Claude stays on the
  laptop, which is the whole point of the split.
- Multi-user anything. There is one owner.
