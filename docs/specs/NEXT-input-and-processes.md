# Input sources, tasks, and what a process manager should be

Not a spec. Answering the question you asked: *"I assume keyboard, mouse,
windows all on different processes that can be polled outside of just some main
loop like it is? not too sure."*

## The short answer

They want to be separate **event sources**, not separate processes — and one of
them already is.

The BLE mouse does not run in the main loop today. Its reports arrive on
NimBLE's host task, get decoded there, and are pushed into a ring that the UI
loop drains. That is already the architecture you are describing, for one
device. The keyboard is still polled inline, and the UI is still a single loop.

What is missing is not processes. It is **one queue that every source posts
to**, so the UI stops polling and starts waiting.

## Why the UI should stay single-threaded

It is tempting to give windows their own tasks. Resist it, for a specific
reason: the compositor has no back buffer. Painting goes straight to the panel
over SPI, and the damage list, the z-order and the panel itself are all shared
mutable state. Two tasks painting concurrently would need a lock around the
entire drawing path, at which point they are not concurrent at all — they are
one thread with extra steps and a new class of bug.

The standard shape, and the right one here:

- **Many producers, one consumer.** Input sources run wherever they must; the
  UI owns the display and drains events.
- Producers do the minimum: decode bytes into an event, post it, return. The
  BLE callback already obeys this, which is why it must not touch the display.

## What that looks like concretely

One `QueueHandle_t` carrying a tagged union:

```c
typedef enum { EV_KEY, EV_MOUSE, EV_TICK, EV_BT_STATE, EV_APP } EventKind;
```

- **Keyboard** becomes a small task scanning the matrix at ~100 Hz and posting
  `EV_KEY` only on a change. It is a 74HC138 strobe and seven GPIO reads — a
  task costs 1132 bytes measured, which is affordable for getting the scan off
  the UI's back.
- **Mouse** posts `EV_MOUSE` from the NimBLE callback instead of a private
  ring. Same work, one less mechanism.
- **A timer** posts `EV_TICK` for the clock and the cursor blink, replacing the
  `esp_timer_get_time()` comparisons sprinkled through the loop.
- **The UI loop** becomes `xQueueReceive(..., portMAX_DELAY)` — it sleeps when
  nothing is happening instead of spinning at 5 ms, which is also the only way
  this device will ever have decent battery life.

Coalescing stays and gets easier: drain the queue non-blocking after the first
event, merge consecutive `EV_MOUSE`, then paint once. That is what the ring
does now, done in one place for every source.

## Bluetooth keyboard

Mostly free once the above exists. A BLE keyboard is the same HOGP client as
the mouse — scan, connect, bond, find the notify-capable report
characteristics, subscribe — with a different decoder at the end. The boot
keyboard report is eight bytes: modifiers, a reserved byte, then up to six
keycodes, which must be diffed against the previous report to produce press and
release events.

So `kernel/drv/btmouse.c` should become `bthid.c` with two decoders behind it.
The connection, bonding, descriptor-hunting and Protocol-Mode machinery — every
part that cost real debugging — is identical and should not be written twice.

Two things to know before starting:

- HID keycodes are **not** ASCII. The boot report carries usage codes and a
  modifier bitmap; translating to characters means a keymap, and that is where
  a second layout hides if you ever want a non-US keyboard.
- A HOGP device with both a keyboard and a mouse (or a keyboard with a
  trackpad) presents several report characteristics on one connection. The
  loader already subscribes to all of them and hands over the attribute handle
  with each report, so routing by handle is the natural fit.

## What a process manager should actually manage

`ps` currently lists one entry, because CardOS's scheduler has policy but no
context switch, and the measurement that killed the hand-written switch stands:
a FreeRTOS task with a 1 KB stack costs 1132 bytes, so owning the switch saves
about 92 bytes per task.

So the honest version is: **let FreeRTOS do the switching and keep CardOS's
scheduler as the thing that knows what a task is *for*.** A CardOS process then
has a name, an owning window, a state, and a FreeRTOS handle — and `ps` becomes
worth reading, because it can show:

- which window a task belongs to, and which app it is running
- its stack high-water mark, from `uxTaskGetStackHighWaterMark`, which is the
  number that tells you whether 1 KB was enough
- its memory: the handle-table blocks it owns, which the memory manager already
  tracks per task since lock ownership was added
- whether it is the UI task, an input source, or a loaded app

And `kill` becomes real, with the lock-release path already built: when a task
dies, `sched_exit` hands back the memory locks it held, which is exactly what
stops a dead app pinning blocks forever.

## Suggested order

1. The event queue, and the keyboard as its first task. Nothing else changes,
   and the UI loop starts sleeping.
2. Move the mouse onto it; delete the private ring.
3. Generalise `btmouse` into `bthid` and add the keyboard decoder plus a keymap.
4. Rebuild `ps`/`kill` on FreeRTOS handles with the information above.

Steps 1 and 2 are worth doing before loadable apps land, because a loaded app
wanting input is a fourth producer, and it should arrive to a queue that
already exists rather than be bolted onto a polling loop.
