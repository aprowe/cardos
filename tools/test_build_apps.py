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


OVL = os.path.join(OUT, "ovltest.capp")

INIT = """#include "kernel/app/capp.h"
CAPP_OVERLAY(1) static int counter = 5;
const CappInfo capp_info = { CAPP_API_VERSION, CAPP_CLI, "ovlinit" };
int capp_main(const CardApi *api, int argc, char **argv) {
  (void)api; (void)argc; (void)argv;
  return counter++;
}
"""

INFO = """#include "kernel/app/capp.h"
CAPP_OVERLAY(1) static char helpbuf[64];
const CappInfo capp_info = { CAPP_API_VERSION, CAPP_CLI, "ovlinfo", { 0 }, helpbuf };
int capp_main(const CardApi *api, int argc, char **argv) {
  (void)api; (void)argc; (void)argv;
  helpbuf[0] = 'x';
  return helpbuf[1];
}
"""

FIVE = """#include "kernel/app/capp.h"
CAPP_OVERLAY(4) static char spare[64];
const CappInfo capp_info = { CAPP_API_VERSION, CAPP_CLI, "ovlfive" };
int capp_main(const CardApi *api, int argc, char **argv) {
  (void)api; (void)argc; (void)argv;
  spare[0] = 'x';
  return spare[1];
}
"""


@unittest.skipUnless(os.path.isfile(OVL), "run tools/build_apps.py first")
class Overlays(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def fixture(self, name, text):
        src = os.path.join(self.tmp, name + ".c")
        with open(src, "w", encoding="utf-8") as f:
            f.write(text)
        return src

    def test_the_overlays_overlap(self):
        secs, _ = build_apps.sections(OVL)
        ovl = {s["name"]: s for s in secs if s["name"].startswith(".ovl")}
        self.assertEqual(sorted(ovl), [".ovl0", ".ovl1"])
        self.assertEqual(ovl[".ovl0"]["addr"], ovl[".ovl1"]["addr"])
        for s in ovl.values():
            self.assertEqual(s["type"], build_apps.SHT_NOBITS)
        data = [s for s in secs if s["name"] == ".data"][0]
        self.assertGreaterEqual(ovl[".ovl0"]["addr"], data["addr"] + data["size"])
        self.assertEqual(ovl[".ovl0"]["addr"] % 16, 0)

    def test_the_budget_counts_the_larger_overlay(self):
        lay = build_apps.layout(OVL)
        self.assertEqual(lay["ovl"], {0: 20480, 1: 6000})
        start = min(lay["ovl_addr"]) - build_apps.DATA_BASE
        self.assertEqual(lay["block"], start + 20480)
        self.assertLess(lay["block"], lay["data"] + 20480 + 6000)

    def test_an_initialised_overlay_variable_is_refused(self):
        with self.assertRaises(SystemExit):
            build_apps.compile_link(self.fixture("ovlinit", INIT), self.tmp)

    def test_capp_info_pointing_into_an_overlay_is_refused(self):
        elf = build_apps.compile_link(self.fixture("ovlinfo", INFO), self.tmp)
        with self.assertRaises(SystemExit):
            build_apps.check_layout(elf, "ovlinfo")

    def test_an_overlay_past_three_is_refused(self):
        elf = build_apps.compile_link(self.fixture("ovlfive", FIVE), self.tmp)
        with self.assertRaises(SystemExit):
            build_apps.check_layout(elf, "ovlfive")

    def test_an_overlay_with_bytes_in_the_file_is_refused(self):
        d = build_apps.DATA_BASE
        secs = [{"name": ".data", "type": build_apps.SHT_PROGBITS, "addr": d, "size": 0x40},
                {"name": ".ovl1", "type": build_apps.SHT_PROGBITS, "addr": d + 0x40, "size": 100}]
        with self.assertRaises(SystemExit):
            build_apps.check_overlays(secs, "fake")

    def test_overlays_at_two_addresses_are_refused(self):
        d = build_apps.DATA_BASE
        secs = [{"name": ".data", "type": build_apps.SHT_PROGBITS, "addr": d, "size": 0x40},
                {"name": ".ovl0", "type": build_apps.SHT_NOBITS, "addr": d + 0x40, "size": 100},
                {"name": ".ovl1", "type": build_apps.SHT_NOBITS, "addr": d + 0x50, "size": 100}]
        with self.assertRaises(SystemExit):
            build_apps.check_overlays(secs, "fake")


if __name__ == "__main__":
    unittest.main(verbosity=1)
