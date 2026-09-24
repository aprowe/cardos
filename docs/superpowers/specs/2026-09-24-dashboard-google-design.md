# The dashboard, and Google sign-in on it

2026-09-24. Replaces `tools/google_auth.py` as the usual way to sign the device
into Google. That tool is still there for the day there is no network.

## Why

`google_auth.py` needed a PC, a USB cable and COM3, and it typed three secrets
into the console. The OAuth consent screen was also left in Testing, so Google
expired every refresh token after 7 days and the whole thing had to be redone
weekly. (Publishing the consent screen fixes the 7 days. It is a Cloud console
setting and has nothing to do with this code.)

## Shape

- **`server/dash.py`**: one service module with its own `ROUTES`.
  - `/dash...` is for a browser. The password is `DASH_PASSWORD` from the
    environment (not the token, so the device's secret is never typed into a
    browser). A session cookie holds `expiry.HMAC(token + password, expiry)`,
    is HttpOnly, Secure and SameSite=Lax, and lasts 30 days; changing either
    secret ends every session. Wrong guesses are serialised at one a second
    and logged with the caller's address.
  - `/google/creds` is for the device, with the usual bearer. It returns client
    id, secret and refresh token, a line each.
  - Neither credential opens the other door.
  - With no `--token` (or, for `/dash`, no `DASH_PASSWORD`), both refuse to
    run (503). Every other route is open on a
    tokenless server; this one never is.
- **Sign-in**: a Google **Web application** client, redirect
  `https://cardos.arowe.net/dash/google/callback`.
  - The request asks for `access_type=offline` and `prompt=consent`, a
    single-use `state` that expires after 10 minutes, and the scopes tasks,
    calendar.events, openid and email.
  - The result goes to `$CARDOS_STATE/google.json` (mode 600).
  - Signing in again revokes the old refresh token. "Sign out" revokes and
    deletes.
- **Status card**: the Claude CLI, session, builds, whisper and render, and the
  `/update` manifest the store is serving.
- **HTTPS**: Google will not redirect a web sign-in to a bare IP or to plain
  HTTP.
  - `tools/deploy_droplet.sh dash [client.json]` adds an nginx site for
    `cardos.arowe.net` exposing only `/dash*` and `/google/creds`, and gets it
    a certificate with certbot.
  - The same command puts the Web client's id and secret into
    `/etc/cardos/env`.
  - `:8080` is unchanged.
- **Device**: `google pull` fetches `$DASH/google/creds` over HTTPS
  (`env DASH`, default `GAUTH_DASH_DEFAULT`) with `/config/claude.token` as
  bearer.
  - It stores the values through `gauth_set`, so they reach NVS and
    `/config/google.txt`.
  - It then asks Google for an access token, to prove the values work.
  - It does not use `env PROXY`, which is plain HTTP, because the refresh
    token must not cross the internet unencrypted.

The device keeps the refresh token rather than asking the server for access
tokens, so Todo and Calendar keep working when the droplet is down.

## Setup, once

1. DNS: an A record `cardos.arowe.net` → `157.245.252.47`.
2. Cloud console, project `local-index-305721`:
   - Create an OAuth client of type Web application, with the redirect URI
     above.
   - Download its JSON.
   - Publish the consent screen (In production).
3. `bash tools/deploy_droplet.sh update` (ships the code), then
   `bash tools/deploy_droplet.sh dash ~/Downloads/client_secret_....json`.
4. Sign in at `https://cardos.arowe.net/dash`, then Sign in with Google.
5. Device: `update os`, then `google pull`.

## Tests

`python -m server.tests.test_dash` covers:

- the cookie: round trip, a different token, expiry, forged expiry, junk
- which door each credential opens
- the Google redirect parameters
- a sign-in against a stubbed Google, followed by a pull
- a bad state, and a state used twice
- a reply with no refresh token
- revoking on re-sign-in and on sign-out
- a tokenless server
