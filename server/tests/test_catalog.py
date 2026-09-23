"""The command catalog read out of built apps, as the server gets it.

Against real binaries: the point is that the struct offsets in
tools/build_apps.py agree with kernel/app/capp.h as the Xtensa compiler lays
it out, and only a real .capp can say. Run tools/build_apps.py first.

    python -m server.tests.test_catalog
"""
import os, sys, unittest
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import build_apps

TODO = os.path.join(ROOT, "build", "apps", "todo.capp")
MINES = os.path.join(ROOT, "build", "apps", "mines.capp")


@unittest.skipUnless(os.path.isfile(TODO), "run tools/build_apps.py first")
class Catalog(unittest.TestCase):

    def test_todo_declares_its_commands(self):
        cmds = {c["id"]: c for c in build_apps.read_commands(TODO)}
        self.assertEqual(sorted(cmds), ["add", "done", "list", "sync"])
        self.assertEqual(cmds["add"]["params"],
                         [{"name": "text", "type": "text", "about": "what the task says"}])
        self.assertTrue(cmds["sync"]["net"])
        self.assertFalse(cmds["add"]["net"])

    def test_gui_only_actions_are_not_commands(self):
        ids = [c["id"] for c in build_apps.read_commands(TODO)]
        for gui in ("tick", "delete", "lists", "all", "print"):
            self.assertNotIn(gui, ids)

    def test_an_app_with_no_commands_has_an_empty_list(self):
        self.assertEqual(build_apps.read_commands(MINES), [])

    def test_the_line_matches_the_devices(self):
        cmds = {c["id"]: c for c in build_apps.read_commands(TODO)}
        # The same text kernel/app/cmdline.c's cmdline_catalog_line makes,
        # which test/test_cmdline.c pins on the device side.
        self.assertEqual(build_apps.catalog_line("todo", cmds["add"]),
                         "todo add text:text # add a task to the current list")
        self.assertEqual(build_apps.catalog_line("todo", cmds["sync"]),
                         "todo sync net # sync every list with Google")


if __name__ == "__main__":
    unittest.main(verbosity=1)
