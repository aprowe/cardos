# CardOS app launcher — booting existing Cardputer firmware

Status: drafted 2026-09-10. Sub-project 1b, alongside the kernel core.

CardOS should be able to boot the Cardputer firmware you already have —
downloaded `.bin` images from M5Burner, M5Launcher, Bruce, Nemo, or anything
else built for this board — without modifying them.

This is **chain-booting**, not app loading. It is worth being blunt about the
difference up front, because the spec's `rom_map()` and sub-project 3's
on-device compiler are about running code *inside* CardOS, and this is not
that.

## The hard constraint

The ESP32-S3 runs one app image at a time. There is no process model, no
memory protection between apps, and an app image's flash-resident code is
mapped through the MMU at boot by the second-stage bootloader. There is no
mechanism by which CardOS could host a foreign firmware as a task.

**So booting a guest app means CardOS exits.** The guest owns the chip until
the next reset. Anything CardOS wants to survive the handover has to be in
flash or on the SD card before it reboots.

What makes this work at all: an ESP-IDF app image is not tied to the flash
offset it was built for. The bootloader maps its DROM and IROM segments to
fixed virtual addresses regardless of where the image physically sits. That is
exactly why OTA can alternate between two partitions at different offsets, and
it is why an unmodified `.bin` can be dropped into our guest slot and booted.

## Decisions

| Decision | Choice | Why |
|---|---|---|
| Mechanism | Copy image from SD into a second app partition, set boot partition, reboot | The only thing the hardware supports. Stock ESP-IDF OTA APIs, no custom bootloader. |
| Return path | One-shot boot via OTA rollback | The guest is foreign and will never cooperate to hand control back. Rollback needs no cooperation: see below. |
| Flash split | 2 MB CardOS / 4 MB guest / 1.875 MB data | 4 MB fits essentially every existing Cardputer firmware; 2 MB leaves CardOS room for the on-device compiler in sub-project 3. |
| Validation | Structural check on the host, cryptographic check by `esp_ota_end()` | Catches a truncated SD copy before spending four seconds and 4 MB of flash writes, without shipping our own SHA-256. |
| Re-flash avoidance | Cache a fingerprint of what is in the guest slot | Re-copying 4 MB on every launch is slow and pointless when nothing changed. |

## Partition table

```
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
otadata,  data, ota,     0xF000,   0x2000,
phy_init, data, phy,     0x11000,  0x1000,
cardos,   app,  ota_0,   0x20000,  0x200000,
guest,    app,  ota_1,   0x220000, 0x400000,
storage,  data, spiffs,  0x620000, 0x1E0000,
```

2 MB + 4 MB + 1.875 MB + headroom = 8 MB exactly. App partitions are 64 KB
aligned as the bootloader requires.

CardOS lives in `ota_0` and the guest goes in `ota_1`; both must be OTA
subtypes for rollback to work. With `otadata` empty on a fresh flash the
bootloader selects `ota_0`, so a virgin device comes up in CardOS.

`storage` exists mainly for **guest compatibility**. A guest that uses NVS,
SPIFFS or LittleFS reads the partition table at `0x8000` — which is *ours*, not
the one it was built against. If a guest expects a data partition we do not
declare, it will fail at startup. Guests that only touch the SD card are
unaffected. This is the most likely source of "app X won't run" reports and
should be the first thing checked.

## How the one-shot boot works

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` must be on.

1. CardOS validates the image, then `esp_ota_begin` / `esp_ota_write` /
   `esp_ota_end` copies it from SD into the `guest` partition.
2. `esp_ota_set_boot_partition(guest)` marks it `ESP_OTA_IMG_NEW` — pending
   verification.
3. `esp_restart()`.
4. The bootloader runs the guest in pending-verify state.
5. The guest never calls `esp_ota_mark_app_valid_cancel_rollback()`, because
   it has no idea it is a guest.
6. On the **next reset**, the bootloader sees the pending-verify flag still
   set, marks the guest aborted, and rolls back to `ota_0` — CardOS.

So: run an app, press reset, you are home. No cooperation from the guest, no
custom bootloader, no key-held-at-boot GPIO reading.

**The matching obligation:** CardOS itself boots pending-verify after any OTA
update of CardOS, so CardOS must call
`esp_ota_mark_app_valid_cancel_rollback()` early in its own startup. If it
does not, CardOS will roll itself back too, and the failure looks like a
device that will not stay updated.

A consequence worth stating plainly: **a guest can never be made permanent.**
That is the intended behaviour for a launcher, but it means CardOS is not a
way to permanently install other firmware. Use M5Burner for that.

## Image validation

Two layers, because they catch different things at different costs.

**Structural, before writing anything** — portable C, host-testable, no
crypto. Reads the 24-byte `esp_image_header_t` and the segment table:

| Check | Rejects |
|---|---|
| `magic == 0xE9` | Not an ESP32 image at all — a text file, an ELF, a zip |
| `chip_id == 0x0009` | An image for the ESP32, S2, C3 — wrong chip, would hard-fault |
| `segment_count` in 1..16 | Corrupt header |
| Segment table walks to a length ≤ the file size | **A truncated SD copy** — the most likely real failure |
| Computed image length ≤ guest partition size | An image too big for the 4 MB slot, before wasting the write |

**Cryptographic, by `esp_ota_end()`** — verifies the appended SHA-256 over the
whole image. We do not reimplement this; ESP-IDF already does it correctly and
refuses to finalise a bad image.

While parsing the header we also read `esp_app_desc_t`, which sits immediately
after the first segment header and carries the project name, version, build
date and IDF version. The launcher shows those, so `apps` lists real names
rather than filenames.

## Re-flash avoidance

Copying 4 MB from SD to flash costs several seconds at the ~1 MB/s the SD-SPI
bus actually delivers on this board, and is pure waste when the same app is
launched twice. CardOS records a fingerprint of what currently occupies the
guest slot — source path, file size, and a CRC-32 of the image — in NVS. If
the requested app matches the fingerprint, skip straight to setting the boot
partition.

The CRC is over the file on SD, not the flash, so a rebuilt app with the same
size is still detected as different.

## Shell commands

| | |
|---|---|
| `apps` | List `.bin` files in `/cardos/apps/`, with name and version from the app descriptor, size, and whether each currently occupies the guest slot |
| `boot <name>` | Validate, copy if needed, set the boot partition, and reboot into it |
| `bootinfo` | Which partition is running, what is in the guest slot, and the rollback state |

`boot` warns and asks for confirmation, because it terminates CardOS and
anything running in it. It is the one shell command that does not return.

## What is host-testable

The image parser and validator are portable C over a byte buffer, with no
ESP-IDF dependency, so they get real unit tests on the PC against synthetic
headers — valid, truncated, wrong-chip, absurd segment counts. That matters
more than usual here: the failure mode of a bad image reaching
`esp_ota_set_boot_partition` is a device that boot-loops, and the recovery is
a USB reflash.

Everything else — the OTA write, the partition lookup, the reboot — is a thin
wrapper over ESP-IDF calls and is verified on the device.

## Success criteria

1. `apps` lists real firmware images from the SD card with their project names
   and versions read out of the image.
2. `boot <app>` runs an unmodified third-party Cardputer firmware.
3. Pressing reset afterwards returns to CardOS, with no key held and no
   cooperation from the guest.
4. A truncated or wrong-chip image is refused by `boot` with a clear message,
   without touching the guest partition.
5. Launching the same app twice does not re-copy it.
6. CardOS survives its own OTA update — it marks itself valid and does not
   roll itself back.

## Deliberately not in scope

Running a guest app *inside* CardOS, in any form. Passing state to a guest.
Returning to CardOS without a reset. Permanently installing a guest. Secure
boot or flash encryption, either of which would prevent booting unsigned
third-party images at all and so is incompatible with the entire point.
