"""PlatformIO pre-script: a build after a commit or a tag carries the new version.

ESP-IDF reads the firmware's version (`git describe`, into esp_app_desc --
bootinfo, About, the boot banner, the update manifest) when CMake
configures, and PlatformIO only configures again when it sees CMakeLists.txt
change: CMake's own CMAKE_CONFIGURE_DEPENDS is not consulted. So a build
after a commit went on stamping the old version (a device said v0.10.0
running v0.11.0; one built after e8a8709 said v0.15.1-1-g15c29c8). Here,
before every build: if `git describe` differs from the last build's, touch
CMakeLists.txt, which PlatformIO does notice.
"""
import os
import subprocess

Import("env")  # noqa: F821 - provided by PlatformIO

root = env.subst("$PROJECT_DIR")  # noqa: F821
build = env.subst("$BUILD_DIR")  # noqa: F821
try:
    now = subprocess.run(["git", "describe", "--tags", "--always", "--dirty"], cwd=root,
                         capture_output=True, text=True, timeout=20).stdout.strip()
except Exception:  # noqa: BLE001 - no git: nothing to keep fresh
    now = ""
if now:
    stamp = os.path.join(build, "cardos_version.txt")
    try:
        with open(stamp, encoding="utf-8") as f:
            was = f.read().strip()
    except OSError:
        was = None
    if was != now:
        os.utime(os.path.join(root, "CMakeLists.txt"))
        os.makedirs(build, exist_ok=True)
        with open(stamp, "w", encoding="utf-8") as f:
            f.write(now + "\n")
        print("version: %s (was %s): configuring again" % (now, was or "unknown"))
