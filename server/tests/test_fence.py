"""The fence round a non-owner's Build turn: server/fence.py and its hook."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

from server import fence

HOOK = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "fence_hook.py")


def git(root, *a):
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t"] + list(a),
                   cwd=root, check=True, capture_output=True)


class Fence(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.state = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.root, True)
        self.addCleanup(shutil.rmtree, self.state, True)
        os.makedirs(os.path.join(self.root, "apps", "icons"))
        os.makedirs(os.path.join(self.root, "kernel"))
        self.put("apps/pet.c", "owner's pet\n")
        self.put("apps/petsim.h", "owner's header\n")
        self.put("kernel/main.c", "kernel\n")
        self.put("apps/folders.txt", "# folders\npet       Games\ntimer     Plan\n")
        git(self.root, "init", "-q")
        git(self.root, "add", "-A")
        git(self.root, "commit", "-q", "-m", "base")

    def put(self, r, text):
        with open(os.path.join(self.root, r), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)

    def read(self, r):
        with open(os.path.join(self.root, r), encoding="utf-8") as f:
            return f.read()

    def may(self, r, user="britney"):
        return fence.may_write(self.state, self.root, user, os.path.join(self.root, r), claim=True)[0]

    def test_new_apps_are_theirs_and_nothing_else_is(self):
        self.assertFalse(self.may("kernel/main.c"))
        self.assertFalse(self.may("apps/pet.c"))            # the owner's app
        self.assertFalse(self.may("apps/petsim.h"))         # and its header
        self.assertFalse(self.may("server/app.py"))
        self.assertFalse(self.may("tools/make_color_icons.py"))
        self.assertFalse(self.may("../outside.c"))
        self.assertFalse(self.may("apps/chess_board.h"))   # no chess app yet
        self.assertTrue(self.may("apps/chess.c"))          # a new app: claimed
        self.assertTrue(self.may("apps/chess_board.h"))    # and now its parts
        self.assertTrue(self.may("apps/icons/chess.txt"))
        self.assertTrue(self.may("apps/folders.txt"))      # line by line, later
        self.assertEqual(fence.apps_of(self.state, "britney"), ["chess"])
        # somebody else cannot take it, nor its parts
        self.assertFalse(self.may("apps/chess.c", user="sam"))
        self.assertFalse(self.may("apps/chess_extra.h", user="sam"))
        self.assertIn("britney", fence.fence_prompt(self.state, "britney"))
        self.assertIn("chess", fence.fence_prompt(self.state, "britney"))

    def test_after_the_turn_everything_outside_goes_back(self):
        self.assertTrue(self.may("apps/chess.c"))
        self.put("apps/chess.c", "her game\n")
        self.put("kernel/main.c", "hacked\n")
        self.put("apps/pet.c", "hers now?\n")
        self.put("apps/sneaky.c", "never claimed\n")
        self.put("apps/folders.txt", "# folders\npet       Make\ntimer     Plan\nchess     Games\n")
        back = fence.enforce(self.state, self.root, "britney",
                             {"apps/chess.c", "kernel/main.c", "apps/pet.c",
                              "apps/sneaky.c", "apps/folders.txt"})
        self.assertEqual(self.read("apps/chess.c"), "her game\n")
        self.assertEqual(self.read("kernel/main.c"), "kernel\n")
        self.assertEqual(self.read("apps/pet.c"), "owner's pet\n")
        self.assertFalse(os.path.exists(os.path.join(self.root, "apps/sneaky.c")))
        # her line kept, the owner's put back
        self.assertEqual(self.read("apps/folders.txt"),
                         "# folders\npet       Games\ntimer     Plan\nchess     Games\n")
        self.assertIn("kernel/main.c", back)

    def test_the_hook(self):
        env = dict(os.environ, FENCE_USER="britney", FENCE_STATE=self.state, FENCE_ROOT=self.root)

        def call(tool, **inp):
            return subprocess.run([sys.executable, HOOK], input=json.dumps(
                {"tool_name": tool, "tool_input": inp}), text=True, env=env,
                capture_output=True)
        r = call("Edit", file_path=os.path.join(self.root, "kernel", "main.c"))
        self.assertEqual(r.returncode, 2)
        self.assertIn("refused", r.stderr)
        self.assertEqual(call("Write", file_path=os.path.join(self.root, "apps", "maze.c")).returncode, 0)
        self.assertEqual(fence.apps_of(self.state, "britney"), ["maze"])
        self.assertEqual(call("Read", file_path=os.path.join(self.root, "kernel", "main.c")).returncode, 0)
        self.assertEqual(call("Read", file_path=os.path.join(self.state, "app_owners.json")).returncode, 2)
        self.assertEqual(call("Bash", command="ls").returncode, 2)
        self.assertEqual(call("Glob", pattern="**/*.c").returncode, 0)
        # without the fence in the environment, nothing is allowed
        env.pop("FENCE_USER")
        self.assertEqual(call("Write", file_path=os.path.join(self.root, "apps", "maze.c")).returncode, 2)


if __name__ == "__main__":
    unittest.main()
