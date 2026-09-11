#!/usr/bin/env python
"""Force the Cardputer back to CardOS, whatever it is currently running.

The bootloader picks an app by reading the `otadata` partition. Erasing it
leaves nothing valid to read, so the bootloader falls back to the `factory`
partition -- which is CardOS. Nothing else is touched: ota_0 keeps whatever
guest is in it, and the SD card is untouched.

This is the escape hatch. The normal route home is simply to press reset: a
guest is booted pending-verify and never confirms itself, so the bootloader
rolls back on its own. Use this when that has not happened -- a guest that
somehow marked itself valid, or a selection made while rollback was not armed.

Borrowed wholesale from the sibling CardLaunch project's back-to-launcher.py,
which is where this trick was worked out.

    python tools/back-to-cardos.py [COM3]
"""
import glob
import os
import subprocess
import sys

OTADATA_OFFSET = 0xE000   # see partitions.csv
OTADATA_SIZE = 0x2000

port = sys.argv[1] if len(sys.argv) > 1 else "COM3"

candidates = glob.glob(
    os.path.expanduser("~/.platformio/packages/tool-esptoolpy*/esptool.py"))
if not candidates:
    sys.exit("esptool.py not found under ~/.platformio/packages "
             "-- is PlatformIO installed?")

cmd = [
    sys.executable, candidates[0],
    "--chip", "esp32s3",
    "--port", port,
    "erase_region", hex(OTADATA_OFFSET), hex(OTADATA_SIZE),
]
print("erasing otadata so the bootloader falls back to factory (CardOS)")
print(" ", " ".join(cmd))
raise SystemExit(subprocess.call(cmd))
