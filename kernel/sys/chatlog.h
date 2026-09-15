/* The conversation the Claude agent keeps on the card. Portable.
 *
 * Every request to the model resends the whole conversation, and the RAM on
 * this board cannot hold one. So the conversation is a file -- the inside of
 * the `messages` array, complete message objects, comma-separated, in the
 * model's own bytes -- and a request is that file with a constant head and
 * tail around it, posted from the card. The reply comes back to the card too
 * and is scanned through a window a few hundred bytes wide, never held whole.
 *
 * Why the model's own bytes: an assistant turn that called a tool carries
 * tool_use blocks whose id, name and input must go back exactly, and thinking
 * blocks whose signature must go back unchanged. Copying the reply's content
 * array byte for byte is the one representation that cannot get this wrong.
 *
 * The file operations come through a table so the whole of this runs under
 * the host test suite on ordinary files, where the JSON edge cases can be
 * exercised without a card or a network. See test/test_chatlog.c.
 */
#ifndef CARDOS_CHATLOG_H
#define CARDOS_CHATLOG_H

#include <stddef.h>
#include <stdint.h>

#define CHATLOG_O_READ   0x01
#define CHATLOG_O_WRITE  0x02
#define CHATLOG_O_CREATE 0x04
#define CHATLOG_O_APPEND 0x08
#define CHATLOG_O_TRUNC  0x10
#define CHATLOG_SEEK_SET 0
#define CHATLOG_SEEK_END 2

typedef struct {
  int  (*open)(const char *path, int flags);        /* fd or -1 */
  int  (*read)(int fd, void *buf, size_t n);
  int  (*write)(int fd, const void *buf, size_t n);
  int  (*seek)(int fd, int32_t off, int whence);    /* new position or -1 */
  void (*close)(int fd);
  int  (*remove)(const char *path);
  int  (*rename)(const char *from, const char *to);
} ChatlogOps;

/* `dir` holds history.json and whatever request/reply files the caller
 * names; paths given to the functions below are relative to it. */
void chatlog_init(const ChatlogOps *ops, const char *dir);

/* Forget the conversation. */
void chatlog_clear(void);

/* Bytes of history, 0 if none. */
int chatlog_size(void);

/* A turn of ours: what the user typed, as one text block. */
int chatlog_add_user(const char *text);

typedef struct {
  const char *id;       /* the tool_use id it answers */
  const char *text;     /* what happened, one line */
} ChatToolResult;

/* The answers to a turn's tool calls, as one user turn of tool_result blocks --
 * all of them in one message, which is what the API expects. */
int chatlog_add_tool_results(const ChatToolResult *r, int n);

/* The model's turn: its content array copied out of a reply file, verbatim. */
int chatlog_add_reply(const char *reply_file);

/* Drop the oldest turns until the history is under `cap` bytes. Turns are
 * dropped whole, and the history always starts with a user text turn
 * afterwards, so a tool_use is never left without its tool_result or a
 * tool_result without its tool_use. */
int chatlog_trim(int cap);

/* head + history + tail, into `request_file`. */
int chatlog_write_request(const char *request_file, const char *head, const char *tail);

/* ---- the reply ------------------------------------------------------------ */

typedef enum { CHAT_STOP_END, CHAT_STOP_TOOL, CHAT_STOP_MAX, CHAT_STOP_OTHER } ChatStop;

#define CHAT_TEXT_MAX  1500
/* Eight, up from four: the note recipe alone is six calls in one turn. Calls
 * past this are not executed, but their ids are kept so every one of them
 * can be answered -- the API refuses the whole conversation otherwise. */
#define CHAT_TOOLS_MAX   8
#define CHAT_EXTRA_MAX   8

typedef struct {
  char id[40];
  char name[16];
  char arg[160];        /* the first input value, as text: "todo", "40", "false" */
  int  num;             /* the same as a number, when it was one; a bool is 0/1 */
} ChatToolCall;

typedef struct {
  ChatStop     stop;
  int          error;               /* the reply was an error document */
  char         text[CHAT_TEXT_MAX]; /* the text blocks joined, unescaped -- or the error message */
  int          ntools;
  ChatToolCall tool[CHAT_TOOLS_MAX];
  int          nextra;              /* tool calls past CHAT_TOOLS_MAX: ids only */
  char         extra_id[CHAT_EXTRA_MAX][40];
} ChatReply;

/* Scan a reply file into `out`. 0 on success, -1 if the file could not be
 * read or was not a message. */
int chatlog_scan_reply(const char *reply_file, ChatReply *out);

#endif /* CARDOS_CHATLOG_H */
