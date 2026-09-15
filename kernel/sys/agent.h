/* Claude, on the device. Device-only.
 *
 * A conversation with the model at api.anthropic.com, over the device's own
 * HTTPS with no proxy in the path, that can also *do things here*: open an
 * app, run one of its actions, type into it. The loop lives in the kernel
 * and runs from the shell's tick, because "open Todo" means Todo takes the
 * screen -- an app that owned this loop would stop running the moment its
 * first useful tool call succeeded. Voice already lives here for the same
 * reason. apps/claude.c is a terminal over this, nothing more.
 *
 * What the model may do is a fixed list of tools, each one a verb the device
 * already had a way to do from the keyboard: the same list voice commands
 * are validated against (kernel/sys/rpc.h), and agent_execute is the one
 * function both go through. The model supplies the choice and an argument;
 * the device supplies everything else.
 *
 * Nothing about the conversation is held in RAM. See kernel/sys/chatlog.h.
 */
#ifndef CARDOS_AGENT_H
#define CARDOS_AGENT_H

#include <stddef.h>

#include "kernel/sys/rpc.h"

void agent_init(void);

/* Is there a key on the card? Without one there is nothing to talk to. */
int agent_has_key(void);

/* Say something. 0 if the request started; -1 if one is already running,
 * -2 with no key, -3 with no card, -4 if the request could not be written
 * or started. */
int agent_ask(const char *text);

/* Forget the conversation. */
void agent_new(void);

/* A request is in flight, or tools are being run. */
int agent_busy(void);

/* From the shell loop, every pass. Polls the request, runs the tools the
 * model asked for, starts the next round, and takes the toast down. */
void agent_tick(void);

/* ---- for the terminal ------------------------------------------------ */

/* The transcript, as lines: "> what you said", "what it said", and
 * "-> what a tool did". The last ~2 KB; older lines fall off the front.
 * `generation` changes whenever it does, so a painter can compare. */
unsigned    agent_generation(void);
const char *agent_transcript(void);

/* One short line about what is happening now, or "" when nothing is. */
const char *agent_status(void);

/* The terminal calls this every tick it is on screen, so an answer that
 * arrives while it is showing goes to it and not to an overlay. */
void agent_seen(void);

/* ---- shared with voice ----------------------------------------------- */

/* Run one command of the vocabulary. `result` gets one line about what
 * happened, in words a person -- or a model -- can act on. */
void agent_execute(const RpcCmd *c, char *result, size_t cap);

#endif /* CARDOS_AGENT_H */
