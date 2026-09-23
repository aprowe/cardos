"""The droplet's build step: what a Build turn builds, what gets published,
and that /update serves the store. Claude, the compilers and git are stubbed:
a real turn would edit this repository.

    python tools/test_buildstep.py
"""
import os, shutil, struct, sys, tempfile, threading, unittest, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import buildstep
import chat as chatmod
import updates
import webproxy
from http.server import ThreadingHTTPServer


def firmware_image(tag=b"a"):
    data = bytearray(512)
    struct.pack_into("<I", data, updates.APP_DESC_OFFSET, updates.APP_DESC_MAGIC)
    off = updates.APP_DESC_OFFSET + updates.APP_DESC_SHA_OFFSET
    data[off:off + 32] = (tag * 32)[:32]
    return bytes(data)


def app_image(tag=b"x"):
    return b"\x7fELF" + tag * 60


class Plan(unittest.TestCase):

    def test_changed(self):
        self.assertEqual(buildstep.changed({"a": "1", "b": "2"},
                                           {"a": "1", "b": "3", "c": "4"}),
                         {"b", "c"})
        # Reverted to clean, and deleted while clean, are both changes.
        self.assertEqual(buildstep.changed({"a": "1"}, {}), {"a"})
        self.assertEqual(buildstep.changed({}, {"a": None}), {"a"})

    def test_build_plan(self):
        plan = buildstep.build_plan
        self.assertEqual(plan(set()), (False, False))
        self.assertEqual(plan({"docs/x.md", "CLAUDE.md", "tools/webproxy.py",
                               "test/test_x.c"}), (False, False))
        self.assertEqual(plan({"apps/pinball.c"}), (True, False))
        self.assertEqual(plan({"kernel/drv/speaker.c"}), (True, True))
        self.assertEqual(plan({"apps/pinball.c", "src/main.c"}), (True, True))

    def test_errors_tail_prefers_errors(self):
        log = "\n".join(["noise %d" % i for i in range(50)] +
                        ["apps/p.c:3:1: error: expected ';'"] + ["more"] * 5)
        self.assertIn("expected ';'", buildstep.errors_tail(log))
        self.assertNotIn("noise 1\n", buildstep.errors_tail(log))


class Publish(unittest.TestCase):

    def setUp(self):
        self.store = tempfile.mkdtemp()
        self.out = tempfile.mkdtemp()
        os.makedirs(os.path.join(self.out, "apps"))
        self.fw = os.path.join(self.out, "fw.bin")
        self.apps = os.path.join(self.out, "apps")

    def tearDown(self):
        shutil.rmtree(self.store, ignore_errors=True)
        shutil.rmtree(self.out, ignore_errors=True)

    def put(self, rel, data):
        with open(os.path.join(self.out, rel), "wb") as f:
            f.write(data)

    def test_only_what_differs(self):
        self.put("fw.bin", firmware_image(b"1"))
        self.put("apps/a.capp", app_image(b"a"))
        self.put("apps/b.capp", app_image(b"b"))
        pub = lambda fw: buildstep.publish(self.store, self.fw, self.apps, fw)
        self.assertEqual(sorted(pub(True)), ["a", "b", "firmware"])
        self.assertEqual(pub(True), [])
        self.put("apps/b.capp", app_image(b"B"))
        self.assertEqual(pub(True), ["b"])

    def test_firmware_only_when_asked(self):
        self.put("fw.bin", firmware_image(b"1"))
        self.put("apps/a.capp", app_image())
        self.assertEqual(buildstep.publish(self.store, self.fw, self.apps, False), ["a"])
        self.assertFalse(os.path.exists(os.path.join(self.store, "firmware.bin")))

    def test_refuses_garbage_and_leaves_no_temp(self):
        self.put("apps/bad.capp", b"not an elf")
        with self.assertRaises(ValueError):
            buildstep.publish(self.store, self.fw, self.apps, False)
        self.put("apps/bad.capp", app_image())
        buildstep.publish(self.store, self.fw, self.apps, False)
        self.assertEqual(os.listdir(os.path.join(self.store, "apps")), ["bad.capp"])


class Turn(unittest.TestCase):
    """BuildingChat.run_turn, with everything outside Python stubbed."""

    def setUp(self):
        self.store = tempfile.mkdtemp()
        self.out = tempfile.mkdtemp()
        os.makedirs(os.path.join(self.out, "apps"))
        c = buildstep.BuildingChat(store=self.store, claude="stub",
                                   firmware=os.path.join(self.out, "fw.bin"),
                                   apps_dir=os.path.join(self.out, "apps"))
        self.snaps = [{}, {"apps/p.c": "1"}]
        c.snapshot = lambda: self.snaps.pop(0)
        self.built, self.commits = [], []

        def build(apps, fw):
            self.built.append((apps, fw))
            with open(os.path.join(self.out, "apps", "p.capp"), "wb") as f:
                f.write(app_image())
            return True, "ok"
        c.build = build
        c.commit = lambda msg: (self.commits.append(msg) or "abc1234")
        c._claude = lambda text: "made it so"
        self.c = c

    def tearDown(self):
        shutil.rmtree(self.store, ignore_errors=True)
        shutil.rmtree(self.out, ignore_errors=True)

    def test_no_bash_on_the_droplet(self):
        self.assertNotIn("Bash", self.c.allowed_tools.split(","))
        self.assertIn("Bash", self.c.disallowed_tools)

    def test_builds_publishes_commits(self):
        state, reply = self.c.run_turn("make the flippers stronger")
        self.assertEqual(state, "done")
        self.assertEqual(self.built, [(True, False)])
        self.assertIn("made it so", reply)
        self.assertIn("published: p", reply)
        self.assertIn("/update", reply)
        self.assertTrue(os.path.isfile(os.path.join(self.store, "apps", "p.capp")))
        self.assertEqual(len(self.commits), 1)
        self.assertIn("make the flippers stronger", self.commits[0])

    def test_nothing_changed_nothing_built(self):
        self.snaps[:] = [{}, {}]
        state, reply = self.c.run_turn("what does pinball do?")
        self.assertEqual((state, reply), ("done", "made it so"))
        self.assertEqual(self.built, [])
        self.assertEqual(self.commits, [])

    def test_docs_change_is_committed_not_built(self):
        self.snaps[:] = [{}, {"docs/notes.md": "1"}]
        self.c.run_turn("write that down")
        self.assertEqual(self.built, [])
        self.assertEqual(len(self.commits), 1)

    def test_build_failure_publishes_nothing_but_keeps_the_work(self):
        self.c.build = lambda apps, fw: (False, "apps/p.c:12: error: oops")
        state, reply = self.c.run_turn("break it")
        self.assertIn("build failed", reply)
        self.assertIn("p.c:12", reply)
        self.assertFalse(os.path.exists(os.path.join(self.store, "apps")))
        self.assertEqual(len(self.commits), 1)
        self.assertIn("does not build", self.commits[0])


class StoreServed(unittest.TestCase):

    def setUp(self):
        self.store = tempfile.mkdtemp()
        os.makedirs(os.path.join(self.store, "apps"))
        with open(os.path.join(self.store, "apps", "p.capp"), "wb") as f:
            f.write(app_image())
        webproxy.Handler.chat = chatmod.ChatService(claude="stub", token="t" * 32)
        webproxy.Handler.store = self.store
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), webproxy.Handler)
        self.base = "http://127.0.0.1:%d" % self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        webproxy.Handler.store = None
        shutil.rmtree(self.store, ignore_errors=True)

    def get(self, path):
        req = urllib.request.Request(self.base + path)
        req.add_header("Authorization", "Bearer " + "t" * 32)
        return urllib.request.urlopen(req, timeout=10).read()

    def test_manifest_and_file_from_store(self):
        data = app_image()
        self.assertEqual(self.get("/update").decode(),
                         "app p %08x %d\n" % (updates.fnv1a32(data), len(data)))
        self.assertEqual(self.get("/update/app/p"), data)


if __name__ == "__main__":
    unittest.main(verbosity=1)
