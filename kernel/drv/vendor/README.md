# Vendored from ESP-IDF

`esp_hid_gap.c` / `esp_hid_gap.h` are copied unmodified (except where noted
below) from ESP-IDF's `examples/bluetooth/esp_hid_host/main/`.

SPDX-FileCopyrightText: 2017-2023 Espressif Systems (Shanghai) CO LTD
SPDX-License-Identifier: Apache-2.0

They are an example rather than a component, so there is nothing to depend on
-- but they are also the correct, tested implementation of BLE GAP scanning
and NimBLE host bring-up for HID, which is fiddly and easy to get subtly
wrong. Copying it is better engineering than writing a worse version.

Local change: `CONFIG_EXAMPLE_SSP_ENABLED` comes from the example's own
Kconfig, which we do not have. It only guards Bluetooth Classic secure simple
pairing, and the ESP32-S3 has no Classic radio at all, so it is defined to 0.
