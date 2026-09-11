# CardOS app launcher — booting existing Cardputer firmware

Status: drafted 2026-09-10; revised the same day against the sibling
CardLaunch project, which already solved this and paid for the lessons.

CardOS should be able to boot the Cardputer firmware you already have —
`cardlet.bin`, `clock.bin`, `slideshow.bin`, `sdfiles.bin`, and anything else
built for this board — without modifying them.

This is **chain-booting**, not app loading. Worth being blunt about up front,
because the spec's `rom_map()` and sub-project 3's on-device compiler are about
running code *inside* CardOS, and this is not that.

## Prior art, and why this document changed

`~/Projects/cardputer/boot/launcher` (CardLaunch) does exactly this job and
works. Its partition table, its tools, and the comments in its source are the
record of what went wrong on the way. Three of this document's original
decisions were wrong and have been corrected against it; each is called out
below rather than quietly fixed, because the reasoning matters more than the
answer.

## The hard constraint

The ESP32-S3 runs one app image at a time. There is no process model, no
memory protection between apps, and an app's flash-resident code is mapped
through the MMU at boot by the second-stage bootloader. There is no mechanism
by which CardOS could host a foreign firmware as a task.

**Booting a guest means CardOS exits.** The guest owns the chip until the next
reset.

What makes it work at all: an ESP-IDF app image is not tied to the flash offset
it was built for. The bootloader maps its DROM and IROM segments to fixed
virtual addresses regardless of where the image physically sits. That is why
OTA can alternate between partitions at different offsets, and why an
unmodified `.bin` can be dropped into our guest slot and booted.

## Decisions

| Decision | Choice | Why |
|---|---|---|
| Mechanism | Copy from SD into a second app partition, set boot partition, reboot | The only thing the hardware supports. Stock ESP-IDF OTA APIs, no custom bootloader. |
| Partition table | **Adopt CardLaunch's `partitions_cardputer.csv` verbatim** | Corrected. The guests were built against that table and read the table they *find*. Diverging is the difference between an app that runs and one that fails at startup for no visible reason. |
| CardOS lives in | **`factory`, not an OTA slot** | Corrected. `factory` is never overwritten, and it is what the bootloader falls back to when otadata is unreadable — which gives a recovery path that needs nothing from the guest. |
| Return path | One-shot boot via rollback, **armed by hand** | Corrected. `esp_ota_set_boot_partition()` does not by itself leave the image pending-verify; the `ota_state` field has to be written. See below. |
| Escape hatch | Erase otadata over USB | Bootloader falls back to `factory`. Works no matter what the guest did. |
| Validation | Structural on the host, cryptographic by `esp_ota_end()` | Catches a truncated copy or a full-flash dump before spending seconds and megabytes of flash writes. |

## Partition table

Taken from CardLaunch unchanged, and `partitions.csv` in this repo is a copy
with the reasoning attached:

```
nvs,        data, nvs,      0x9000,   0x5000,
otadata,    data, ota,      0xE000,   0x2000,
factory,    app,  factory,  0x10000,  0x1C0000,
ota_0,      app,  ota_0,    0x1D0000, 0x300000,
phy_init,   data, phy,      0x4D0000, 0x1000,
spiffs,     data, spiffs,   0x4D1000, 0x32F000,
```

CardOS gets 1.75 MB in `factory`; the guest slot is 3 MB. The largest existing
app is `cardlet.bin` at 1.35 MB, so 3 MB is ample. CardOS currently builds to
310 KB, leaving room for the on-device compiler.

Three things about this table are load-bearing:

**The first three entries are at conventional offsets on purpose.** The
Arduino platform flashes `boot_app0.bin` at a hardcoded `0xE000` and the app at
`0x10000` whatever the table says. CardLaunch found that moving them produced a
bootloader reset loop, because the app landed in a data partition and `factory`
stayed empty. CardOS is ESP-IDF and not itself subject to that, but the guests
are Arduino builds and must land where their own build expects.

**The data partition must be called `spiffs`.** That is the label Arduino's
LittleFS looks for. It holds a LittleFS image regardless of the name. Rename it
and every Arduino guest that stores anything fails to mount — the original
version of this spec called it `storage`, which would have broken exactly that.

**`nvs` is `0x5000`, not `0x6000`.** Same reasoning: match the table the guests
were built against.

## Getting back to CardOS

Two independent routes, neither needing the guest to cooperate — which matters,
because a foreign firmware has no idea it is a guest.

### 1. One-shot boot, and the part that is easy to get wrong

`esp_ota_set_boot_partition()` points the bootloader at the guest, but it does
**not** leave the image in pending-verify state on its own. The original
version of this spec asserted that it did. It does not; the `ota_state` field
in the live otadata entry has to be written to `ESP_OTA_IMG_NEW` afterwards.

The mechanics, from CardLaunch's `armRollback()`:

- otadata holds two `esp_ota_select_entry_t` records, one per 4 KB sector.
- `esp_ota_set_boot_partition` has just bumped one of them. The live one is
  whichever has the higher sequence number; an unwritten entry reads
  `0xFFFFFFFF`.
- The CRC in that record covers `ota_seq` alone, so `ota_state` can be changed
  without recomputing it.
- `ESP_OTA_IMG_NEW` is `0x0`, and flash bits only ever go 1 → 0, so the field
  can be written **in place with no erase**. This is essential: erasing otadata
  here would destroy the selection just made.

The guest then runs, never calls `esp_ota_mark_app_valid_cancel_rollback()`
because it has never heard of it, and the next reset rolls back to `factory`.
Run an app, press reset, you are home.

Requires `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`.

**The matching obligation:** CardOS boots pending-verify after any OTA update of
itself, so it must call `esp_ota_mark_app_valid_cancel_rollback()` early in
startup or it will roll *itself* back. That failure looks like a device that
refuses to stay updated.

### 2. Erase otadata

With nothing valid to read, the bootloader falls back to `factory` — CardOS.
Nothing else is touched: `ota_0` keeps its app and the SD card is untouched.
This is the escape hatch when a guest has wedged things, and it is a host-side
tool (`tools/back-to-cardos.py`, mirroring CardLaunch's `back-to-launcher.py`):

```
esptool.py --chip esp32s3 --port COM3 erase_region 0xE000 0x2000
```

## Known risk: bootloader version

CardLaunch's notes record that a custom bootloader built from a different
ESP-IDF version **boots but crashes the WiFi stack**. CardLaunch avoids this by
being an Arduino project itself, so it ships the same stock Arduino bootloader
its guests expect — which already has rollback enabled.

CardOS is ESP-IDF by necessity (the radio blobs), so its bootloader is IDF's,
built from a different version than the Arduino guests were built against.
**This is the single largest unknown in the launcher plan.** It may be fine;
the bootloader's job is mostly generic. But the symptom to watch for is a guest
that boots and then dies the moment it touches WiFi, and the first test should
be `cardlet.bin`, which uses WiFi heavily.

If it does break, the fallback is to flash the Arduino bootloader alongside
CardOS rather than IDF's. That needs verifying on hardware before it is
promised.

## Image validation

Two layers, catching different things at different costs.

**Structural, before writing anything** — portable C, host-tested, no crypto:

| Check | Rejects |
|---|---|
| `magic == 0xE9` | Not an ESP32 image at all |
| `chip_id == 0x0009` | Built for the ESP32, S2 or C3 — would hard-fault |
| `segment_count` in 1..16 | Corrupt header |
| Segment table walks to a length ≤ file size | **A truncated SD copy** — the likeliest real failure |
| File no more than 8 KB longer than the image | **A full-flash dump** — see below |
| Image ≤ 3 MB | Too big for `ota_0`, before wasting the write |

The full-flash-dump check is an improvement on CardLaunch, which guards with a
first-byte `0xE9` test and a comment saying that catches dumps. It does not: a
full-flash dump begins with the *bootloader* image, and that starts `0xE9` too.
What actually distinguishes them is that the contained image is a tiny fraction
of the file. The 8 KB tolerance leaves room for a secure-boot signature block.

**Cryptographic, by `esp_ota_end()`** — verifies the appended SHA-256. Not
reimplemented here; ESP-IDF already does it and refuses to finalise a bad
image.

The parser also reads `esp_app_desc_t` for the project name and version, so
listings read `cardlet 1.4` rather than `cardlet.bin`.

## Where apps live

`/cardos/apps/` per this project's layout, **and `/firmware/`**, which is where
CardLaunch already keeps them. Supporting both means a card that works with
CardLaunch works with CardOS unchanged.

## Re-flash avoidance

Copying 3 MB from SD to flash costs several seconds at the ~1 MB/s the SD-SPI
bus delivers, and is waste when the same app is launched twice. CardOS records
a fingerprint of what occupies `ota_0` — source path, size, CRC-32 of the file
— in NVS, and skips the copy when it matches. The CRC is over the file on SD,
so a rebuilt app of identical size is still detected as different.

## A caution about uploads

CardLaunch has `tools/keep_otadata.py`, which strips `boot_app0.bin` from the
upload command because PlatformIO flashes it at `0xE000` on every upload,
silently repointing the bootloader. CardOS's ESP-IDF build produces
`ota_data_initial.bin` rather than `boot_app0.bin`, and whether PlatformIO
flashes it on upload **needs checking on the first real flash**. If it does,
the same treatment is required, or every `pio run -t upload` will quietly reset
which app is selected.

## Shell commands

| | |
|---|---|
| `apps` | List `.bin` files in `/cardos/apps` and `/firmware`, with name and version from the app descriptor, size, and validity |
| `boot <name>` | Validate, copy into `ota_0`, arm rollback, reboot into it |
| `bootinfo` | Running partition, what is in `ota_0`, rollback state |

`boot` confirms first, because it terminates CardOS and everything in it. It is
the one command that does not return.

## Success criteria

1. `apps` lists the real firmware on the card with project names and versions.
2. `boot cardlet` runs unmodified third-party Cardputer firmware.
3. Pressing reset afterwards returns to CardOS, with no key held and no
   cooperation from the guest.
4. A truncated, wrong-chip, or full-flash-dump image is refused with a clear
   message, without touching `ota_0`.
5. A guest that uses LittleFS finds its data partition.
6. `cardlet.bin` reaches WiFi without crashing — the bootloader-version risk.
7. Launching the same app twice does not re-copy it.
8. CardOS survives its own OTA update rather than rolling itself back.

## Deliberately not in scope

Running a guest inside CardOS in any form. Passing state to a guest. Returning
without a reset. Permanently installing a guest — use M5Burner. Secure boot or
flash encryption, either of which would prevent booting unsigned third-party
images and so defeats the entire point.
