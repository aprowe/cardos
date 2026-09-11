/* Google OAuth, the part that runs on the device. Device-only.
 *
 * The interactive half does not happen here. A Cardputer has no browser and no
 * way to show a consent screen, so the sign-in is done once on a PC by
 * tools/google_auth.py, which ends with a refresh token. That token and the
 * client credentials are pushed to the device over serial, and from then on
 * this is all that is needed: exchange the refresh token for an access token
 * when the old one expires, and hand it to whoever is making a request.
 *
 * Google's device-code flow would avoid the PC, but it is offered only to the
 * "TVs and limited input devices" client type and does not cover the Tasks
 * scope. Pretending otherwise would mean building a flow that cannot work.
 *
 * WHERE THE SECRETS LIVE, PLAINLY: the client secret and the refresh token are
 * kept in NVS, in the clear. Flash encryption is not enabled on this board, so
 * anyone holding it can read them, and the refresh token is enough to read and
 * write that Google account's tasks until it is revoked. Revoke it at
 * myaccount.google.com/permissions if the board is lost.
 */
#ifndef CARDOS_GAUTH_H
#define CARDOS_GAUTH_H

#include <stdint.h>

#define GAUTH_ID_MAX     128
#define GAUTH_SECRET_MAX 64
#define GAUTH_REFRESH_MAX 256
#define GAUTH_TOKEN_MAX  400

/* Stored once, by the host tool. A NULL or empty value clears that field. */
int  gauth_set(const char *client_id, const char *client_secret,
               const char *refresh_token);
void gauth_forget(void);
int  gauth_configured(void);

/* A valid access token, refreshing if the cached one has expired or is about
 * to. NULL if there are no credentials or the exchange failed; gauth_status
 * then says why. Blocking -- it is an HTTPS round trip. */
const char *gauth_token(void);

const char *gauth_status(void);

#endif /* CARDOS_GAUTH_H */
