# Remote update — apps and the OS over WiFi

Approved 2026-09-11. Records the decisions; the code says how.

## What

The device pulls new builds from `tools/webproxy.py` on the PC — the same
process, port and token that carry the Claude terminal and the web browser.
Two kinds of thing: `.capp` app binaries, written to `/desktop` on the card;
and CardOS itself, written to an OTA slot and booted with rollback.

Triggered two ways, the same code under both: the console command `update`,
and the Claude terminal, which checks after every answer lands and offers
`/update` when something is newer. The point of the second is the loop the
terminal exists for — "make the flippers stronger" ends with the new
pinball.capp on the device, not on the PC.

## Decisions

| | Decision | Why |
|---|---|---|
| Source | The proxy on the LAN, not a public URL | It already serves the device, it already has the build directory, and it needs no release process or TLS. A URL is a string; this can change later. |
| Identity | Content hashes, not version numbers | Nothing to bump and nothing to forget. Firmware: the ELF SHA-256 every image carries in its `esp_app_desc_t`, which the device reads from itself and the proxy reads from the file. Apps: FNV-1a 32 over the file, which `icons.c` already uses for the blob stamp. |
| Manifest | Plain text, one line per item: `firmware SHA SIZE`, `app NAME HASH SIZE` | The device has no JSON parser and no reason to grow one. |
| App install | Download to `NAME.capp.new`, verify size and hash, rename over | A failed download leaves the old app runnable. |
| Firmware install | Download to `/update/firmware.bin` on the card, then the existing SD→partition copy in `launcher.c` | Reuses code that already validates structurally, verifies the SHA in `esp_ota_end`, and arms rollback. Costs ~10 s over streaming, and leaves a firmware on the card. |
| Where firmware lands | The OTA slot not currently running (`esp_ota_get_next_update_partition`) | `factory` stays USB-only and is the recovery image. Two OTA slots alternate, so the OS can update any number of times. Guests use the same rule, so a guest never overwrites the running OS. |
| Partition table | factory 1.75 MB, ota_0 2.94 MB, ota_1 2.94 MB, spiffs ~300 KB | The firmware is 1.59 MB; a slot needs 2 MB with room to grow. `spiffs` was 3.3 MB and CardOS never used it; Arduino guests only need it to exist and mount. First three entries unchanged, as the table's own comment demands. |
| USB flash wins | At boot, running from an OTA slot with a `factory` image that is newer by build date/time → set boot to `factory` and restart | A USB flash writes `factory` but otadata still points at the OTA slot, so without this the freshly flashed build never runs. |
| Trust | LAN plus the shared token | The same boundary that already allows editing the repository. No signing. |

## Out of scope

Public URLs, delta updates, updating the bootloader, updating NVS or the
partition table itself.

## Testing

- Host: manifest parser and stale-diff (`test/test_update.c`); the proxy's
  three routes against a fixture build directory (`tools/test_update.py`).
- Device, measured and put in the commit message: time to update apps, time
  to update firmware, and a rollback — a firmware that aborts before marking
  itself valid must give way to the previous one on the next reset.
