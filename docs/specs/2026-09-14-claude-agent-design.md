# A Claude of its own: the on-device agent, and Build

**Status:** approved 2026-09-14.

## What this is

Two apps where there is one today.

- **Build** is the app currently called Claude: a terminal to Claude Code
  running in this repository on the PC, through `webproxy.py`. It edits
  CardOS. It is renamed, and nothing else about it changes.
- **Claude** is new: a general-purpose Claude client that talks to
  `api.anthropic.com` directly over the device's own HTTPS, with no proxy in
  the path, and that can *do things on the device* — open an app, add a task,
  make a note — through the same fixed vocabulary of verbs the voice path
  already validates.

The second is the design problem. The first is a rename.

## Decisions already made

| Decision | Choice | Why |
|---|---|---|
| Where the agent loop runs | In the kernel, `kernel/sys/agent.c`, driven from the shell's tick | On this device "open Todo" means Todo takes the screen. In the launcher the Claude app stops running the moment its first useful tool call succeeds. Voice already solved this by living in the kernel; the agent does the same. |
| What the app is | A terminal over the agent: transcript and a prompt line, nothing else | The app can be unloaded, replaced on screen, or never opened, and the conversation carries on. |
| How the model acts | Anthropic tool use, one tool per verb the device already has | The voice design's argument — *an LLM choosing between a few verbs is useful; an LLM handing a device a string to run is not* — is what tool use is. Every tool maps onto a kernel call that a keyboard could have made. |
| Model | `claude-opus-5`, `output_config.effort: "low"`, adaptive thinking left on | A pocket chat wants the answer quickly; low effort on Opus 5 is more than a 40-column screen can use, and it consolidates tool calls. Thinking is not disabled: on Opus 5 that makes the model write tool calls into visible text. |
| Streaming | No | SSE parsing buys nothing on a 16-line screen, and the busy badge already says a request is in flight. |
| Key | `/claude.key` on the card, read at first use | Same place and shape as `/claude.token`. Never in NVS, never in the firmware, never sent anywhere but `api.anthropic.com`. |
| Where the conversation lives | On the card: `/cache/claude/history.json` is the `messages` array's contents, verbatim | Every request resends the conversation, and RAM is the one thing this board has none of. The card is the framebuffer for screenshots; it is the context window here. |
| How a request is sent | Built as a file — a constant head, the history file, a constant tail — and posted from the file | `http_post_file` streams from the card already. Nothing larger than a 512-byte window is ever in RAM for the body. |
| How a reply arrives | Streamed to `/cache/claude/reply.json`, then scanned from the file | `http_download_ex` already does this for pages and firmware. The reply is read through the same window, and the assistant `content` array is copied from it into the history file byte for byte. |
| History cap | 16 KB, oldest turn dropped whole | A policy about latency and cost now, not a RAM limit: 16 KB of input is a few seconds of upload on this radio and a few cents. Raise it when it hurts. |
| Reply size | `max_tokens` 1024 | Sixteen lines of forty columns. A system prompt asks for short plain text with no markdown, because nothing here can draw it. |
| Headers | `http_request`'s `bearer` parameter becomes `auth`: with a `:` in it, sent verbatim as header lines; without, a bearer token | One string, no new parameters, and `httpq` and `api->http_start` pass it through unchanged. API version 22 → 23. |
| Where the answer shows | In the Claude app if it is on screen; otherwise as the voice overlay's result line, briefly | The agent finishes while Todo is showing. The user asked for something and should see that it happened without going back to find out. |

## The memory problem, stated for this feature

With both radios up there are ~120 KB of heap and a TLS session takes ~40 KB
of it while a request runs. The agent adds almost nothing to that, because
the conversation never comes into RAM whole:

| | where | RAM |
|---|---|---|
| history | `/cache/claude/history.json` | — |
| request body | `/cache/claude/request.json`, written by concatenation, posted from the file | one 512-byte window |
| reply | `/cache/claude/reply.json`, streamed in, scanned in place | one 512-byte window |
| the tool definitions and system prompt | a constant string in flash | — |
| the transcript the app paints | the last ~2 KB of what was said, in the kernel | 2 KB |

`httpq` gains a second mode for this — body from a file, reply to a file —
beside the string mode every other app uses. One request at a time, as now.
The cost moved to the card: a turn writes and reads a few tens of kilobytes,
which is tens of milliseconds, against a request that takes seconds.
`mem` during a request is still the measurement that decides whether any of
this is true.

## The tools

Each tool is one verb the kernel already executes; the model supplies only
the choice and the argument. Results are one line of text, and they say what
happened in the words a person would use, so the model can recover
("no app called notes" → it tries `edit`).

| tool | input | executes | result |
|---|---|---|---|
| `open` | `app` | `shell_open_app` | `opened Todo. actions: add, tick, delete, sync` — or `no app called X. apps: …` |
| `action` | `id` | `capprun_action_invoke` on the focused app | `done` — or `Todo has no action X. actions: …` |
| `type` | `text` | `input_text`, as keystrokes | `typed` — or `nothing here is taking text` |
| `key` | `name` (escape, enter, up, down, left, right) | `shell_feed_key` | `pressed enter` |
| `shell` | `which` (launcher, desktop, console) | `shell_switch` | `desktop` |
| `brightness` | `percent` | `display_set_brightness` | `brightness 40%` |
| `wifi` | `on` (bool) | `wifi_connect_saved` / `wifi_stop` | the status |

`open`'s result lists the app's action table because that is how the model
learns what it can do next. `action` needs the focused app, which the shell
does not expose today: `ShellOps` gains `running_app()`, returning the
`AppDef` on screen in the launcher or the focused window on the desktop.

"Make a new note called shopping with milk and eggs" is then `open(edit)` →
`action(new)` → `type("milk\neggs")` → `action(saveas)` →
`type("shopping.txt")` → `key(enter)`: six round trips, each ~5 s, each
shown in the transcript as `→ opened Edit` as it happens. Slow, honest, and
nothing a keyboard could not have done.

`kernel/sys/voice.c`'s `run_command` moves into `kernel/sys/agent.c` as
`agent_execute`, so voice and the agent run verbs through one function.
Voice's line-based vocabulary (`rpc.c`) stays as its wire format; a tool call
is converted to the same `RpcCmd` and executed the same way. `RPC_ACTION` is
added to the vocabulary so voice gets `action` too.

## The wire

`POST https://api.anthropic.com/v1/messages`, headers `x-api-key`,
`anthropic-version: 2023-06-01`, `content-type: application/json`. Body:

```json
{ "model": "claude-opus-5", "max_tokens": 1024,
  "output_config": { "effort": "low" },
  "system": "…short, plain text, no markdown, you can act on the device…",
  "tools": [ …the seven, constant… ],
  "messages": [ …history… ] }
```

The history file *is* the inside of the `messages` array: complete message
objects, comma-separated, the model's own bytes. An assistant turn that
called a tool has `tool_use` blocks whose `id`, `name` and `input` must go
back verbatim, and thinking blocks whose signature must go back unchanged on
the same model. Copying the reply's `content` array out of the reply file
and into the history file is the only representation that cannot get this
wrong. The user's typed text is appended as a text block; a tool result as a
`tool_result` block naming the `tool_use_id`. Dropping the oldest turn is
copying the file from its second message onward — a turn is dropped whole,
and never a `tool_use` without the `tool_result` that answers it.

The reply is scanned through a window, not parsed: `"stop_reason"`, then
each block's `"type"`, and for `text` the string (unescaped, into the
transcript), for `tool_use` the `id`, `name` and the one string or number
argument. The same style Todo and Calendar use on Google's JSON, for the
same reason. An `error.message` is shown as the answer.

## The loop

```
agent_ask(text)        user text → history, request started
agent_tick()           from the shell loop:
  poll httpq
  reply arrives:
    stop_reason tool_use → execute each tool, append tool_results,
                           start the next request, show "→ result" lines
    end_turn            → append the text, show it, done
    error               → show error.message, drop the last user turn
```

Ten tool rounds in one ask is the cap; after that the agent stops and says
so. `agent_new()` clears history (`/new` in the app). `agent_busy()`,
`agent_lines()` and a change counter let the app paint the transcript
without owning it.

## The app

`apps/claude.c` becomes the terminal over the agent, keeping Build's UI —
transcript with scrollback, a prompt line that always takes text, so voice
reaches it — minus `/update` and the proxy. It declares `CAPP_NEEDS_NET`.
With no `/claude.key` it says so and how to fix it, and takes no input.

`apps/build.c` is today's `claude.c`, renamed: name **Build**, a hammer on
the speech bubble, `Net/build.capp`. `/claude.token` keeps its name — it is
the proxy's token, and `webproxy.py` and `update.c` read it too.

## Testing

Host tests (`test/test_agent.c`), portable C with no device dependencies:

- the history file: appending a user turn, a tool result, and a reply's
  `content` array copied through a window; dropping the oldest turn whole
  when over the cap, never splitting a tool_use from its tool_result
- scanning a reply through a window: text blocks, tool_use blocks with
  string and number inputs, `error.message`, `stop_reason` — including
  strings that straddle a window boundary
- the file operations behind a `FileOps` table so the tests run on host
  files and the device runs on the card
- converting a tool call to an `RpcCmd` and rejecting unknown tools
- `rpc_parse` of the new `action` verb

Device: a real key, "what's 2+2", "open todo", the shopping-list sentence,
`mem` before and during a request, both radios up.

## Not doing

Streaming. Images. Tool calls the device cannot already do from a keyboard.
Choosing models on the device. Routing "Carlos" voice commands through this
agent instead of the proxy's session — the plumbing now allows it, and it is
the obvious next step, but it is a separate change.
