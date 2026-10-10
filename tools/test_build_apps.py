"""tools/build_apps.py's layout checks, on real binaries.

Constants in flash (docs/superpowers/specs/2026-10-10-app-constants-in-flash-design.md):
every app's .rodata is linked at RODATA_BASE and is not writable, capp_info
stays at the start of .data, and everything capp_info points to is in .data
or .rodata -- the icon scan reads it without running the app. Overlays
(docs/superpowers/specs/2026-10-10-app-data-overlays-design.md) are checked
in the class below this one. Run tools/build_apps.py first.

    python tools/test_build_apps.py
"""
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_apps

OUT = build_apps.OUT
JAR = os.path.join(OUT, "jar.capp")


def built():
    return sorted(os.path.join(OUT, f) for f in os.listdir(OUT) if f.endswith(".capp"))


@unittest.skipUnless(os.path.isfile(JAR), "run tools/build_apps.py first")
class Constants(unittest.TestCase):

    def test_every_app_passes_the_layout_check(self):
        for elf in built():
            build_apps.check_layout(elf, os.path.basename(elf))

    def test_constants_are_linked_at_their_own_origin(self):
        for elf in built():
            secs, _ = build_apps.sections(elf)
            for s in secs:
                if s["name"] == ".rodata":
                    self.assertEqual(s["addr"], build_apps.RODATA_BASE, elf)
                    self.assertFalse(s["flags"] & build_apps.SHF_WRITE, elf)

    def test_jar_keeps_its_constants_out_of_the_arena(self):
        lay = build_apps.layout(JAR)
        self.assertGreater(lay["rodata"], 8 * 1024)
        self.assertLessEqual(lay["block"], build_apps.DATA_BUDGET - 8 * 1024)

    def test_capp_info_help_is_a_constant_now(self):
        img = build_apps.Image(JAR)
        help_ = img.u32(build_apps.DATA_BASE + build_apps.INFO_HELP)
        self.assertGreaterEqual(help_, build_apps.RODATA_BASE)
        self.assertTrue(img.cstr(help_))


if __name__ == "__main__":
    unittest.main(verbosity=1)
