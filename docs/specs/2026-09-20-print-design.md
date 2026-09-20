# Paper: printing from the device over Bluetooth

Approved 2026-09-20; built and verified on hardware the same day.

## The printer

A "TinyPrint" thermal printer from Amazon, which is an **X6h**: the "cat
printer" family (GB01/GB02/GT01/MX06/X6h; the chip vendor's string in the
advertisement is `JLAISDK`). The protocol was reverse-engineered by the
community years ago and was confirmed against this unit from a PC before any
device code was written (`~/Projects/printer/PROTOCOL.md`, `testprint.py`):

- GATT service `AE30`; commands to `AE01` (write-without-response), status
  notifications on `AE02`. It advertises `AF30`, not `AE30`.
- Packet: `51 78 CMD 00 LEN 00 DATA.. CRC8 FF`, CRC8 poly 0x07 over DATA.
- `A2` draws one row: 384 pixels, 48 bytes, bit set = black, **LSB of each
  byte is the leftmost pixel**.
- A job: `A3` status, `A4 32` quality, `A6` lattice start, `AF` energy,
  `BE 00` image mode, `BD 20` speed, `A1` feed; N rows; `A1` feed, `A6`
  lattice end, `A3` status.
- No pairing, no encryption. The link only ever offers a 23-byte MTU.

## Decision: print from the device, not the PC

The proxy could have rendered with real fonts and printed over the PC's
radio, matching how web and voice work. The user chose the device: the
Cardputer already has a NimBLE central for HID, and a printer that works in
a pocket is the point of a pocket computer. The cost is the pixel font
(scaled) and one more Bluetooth link.

## Where it lives

| | |
|---|---|
| `kernel/sys/printdoc.c` | the wire format and the document renderer; portable, host-tested |
| `kernel/drv/btprint.c` | the BLE client: scan, connect, discover, write, notify |
| `kernel/sys/printq.c` | the job: one at a time, on its own task; the config file |
| `kernel/drv/bthid.c` | exports `bt_radio_up()`; otherwise untouched |

`CONFIG_BT_NIMBLE_MAX_CONNECTIONS` is 3 so a mouse, a keyboard and the
printer can all be connected.

## The document: a tiny markup

`api->print(doc)` takes line-oriented text, deliberately markdown-shaped:

```
# Heading          3x font (18x24), bold, ruled under
## Subheading      2x bold
[ ] task           drawn checkbox (16 px), 2x text
[x] done           ticked box
---                rule
plain text         2x text (12x16), 29 columns, word-wrapped
                   blank line = 8 px gap
```

Nothing prints below 2x: the 6x8 screen font at 203 dpi is unreadable at
1x. Margins are 16 px. Rows render one at a time from a block buffer the
height of one wrapped line, so the document string is the only allocation.

## The job

`printq_print_doc` copies the document and starts a task (6 KB stack) that
reads `/config/printer.txt` (address, address type, name), connects with a
15 s timeout, discovers `AE30`/`AE01`/`AE02`, subscribes, streams the
prologue, every row and the epilogue, waits 300 ms for the status
notification, and disconnects. Its one line of status is `printq_status()`:
"connecting", "printing 40%", "printed", "print failed: out of paper".

The job takes a **row source** (`PrintRowFn`), not a document: the markup
renderer is one source. A pre-rendered 1-bpp bitmap from a file or from the
PC is the next one, and needs nothing here to change.

Writes are paced at 10 bytes/ms whatever the MTU makes a chunk -- the rate
the PC probe printed cleanly at. A flat delay per 20-byte chunk was the
first version and made a 206-row page take 14 s; pacing by bytes made it
2.2 s with no rows lost. NimBLE returning `ENOMEM` (mbufs exhausted) is a
wait, never a drop.

## App side

- API 25: `print(doc)` (0 / -1 busy / -2 no printer / -3 no memory) and
  `print_status()`.
- **fn-p is print**, in every app that can. Paper is a window operation,
  so it lives on fn with close and help. An app opts in by giving an action
  `CAPP_KEY_PRINT` in its table; `tr_key` matches it like any chord, the
  toolbar menu shows "Print  fn-p", and the help panel lists it. The
  desktop's keyboard-mouse toggle moved from fn-p to fn-k for this.
- **Todo** prints the list on screen (open tasks as boxes, then done ones
  under a rule, ticked, then a dated footer) or, from the overview, every
  list under its own heading. `p` also works, like its other letters.
- **Edit** prints the buffer as it is, so a note with headings and tasks
  prints as the preview shows it.
- **Console**: `print` (status), `print scan`, `print use N`, `print forget`,
  `print test`, `print FILE`.

## Measured

With WiFi and Bluetooth both up and a job just finished: 89 KB heap free,
low water 80 KB. A test page: connect and discover in about 1 s, 206 rows
in 2.2 s. Flash 68.8%.

## Not done

- Images: a bitmap row source and a way to get a pre-rendered image onto
  the device (the proxy, or a file on the card). The job is shaped for it.
- Energy/darkness as a setting; the TinyPrint default (12000) is hard-coded.
- The `AE3A` service and `AE10` characteristic are untouched; probably
  firmware update.
