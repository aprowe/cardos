"""Toggl through the server, against a Toggl that is a dict.

What the device relies on: the line formats, start epochs it can count from,
a start that stops what was running, a total that includes the running
entry, Toggl's refusals said in words -- and few calls, because Toggl
counts them against an hourly quota.

    python -m server.tests.test_toggl
"""
import os
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from http.server import ThreadingHTTPServer
from server import app, toggl
from server import chat as chatmod

TOKEN = "tok"
T0 = "2026-10-01T16:00:00+00:00"
T0_EPOCH = 1790870400


class FakeToggl:
    def __init__(self):
        self.calls = []
        self.running = None
        self.entries = [
            {"id": 1, "description": "Writing", "project_id": 10, "start": "2026-10-01T09:00:00Z",
             "duration": 1800, "workspace_id": 5},
            {"id": 2, "description": "Writing", "project_id": 10, "start": "2026-10-01T10:00:00Z",
             "duration": 600, "workspace_id": 5},
            {"id": 3, "description": "Email", "project_id": None, "start": "2026-10-01T11:00:00Z",
             "duration": 300, "workspace_id": 5},
        ]
        self.status = None                      # force every answer to this

    def __call__(self, method, url, body=None, token=None):
        rel = url[len(toggl.API):]
        self.calls.append((method, rel, body))
        if self.status:
            return self.status, {"error": "no"}
        if token != "good":
            return 403, "Incorrect username and/or password"
        if rel == "/me":
            return 200, {"fullname": "Alex", "default_workspace_id": 5}
        if rel.startswith("/workspaces/5/projects"):
            return 200, [{"id": 10, "name": "CardOS", "color": "#0b83d9"},
                         {"id": 11, "name": "Home"}]
        if rel == "/me/time_entries/current":
            return 200, self.running
        if rel.startswith("/me/time_entries?"):
            # The real one refuses either date alone (2026-10-01).
            if "start_date=" not in rel or "end_date=" not in rel:
                return 400, "start_date and end_date are both required"
            return 200, list(self.entries) + ([self.running] if self.running else [])
        if method == "PATCH" and rel.endswith("/stop"):
            e, self.running = dict(self.running), None
            e["duration"] = 120
            return 200, e
        if method == "PUT" and rel == "/workspaces/5/time_entries/99":
            self.running = dict(self.running, **body)
            return 200, self.running
        if method == "POST" and rel == "/workspaces/5/time_entries":
            self.running = dict(body, id=99, start=T0, workspace_id=5)
            return 200, self.running
        return 404, None


class TogglTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        os.environ["CARDOS_STATE"] = self.dir
        toggl._cache.clear()
        toggl.save({"token": "good", "name": "Alex", "workspace": 5})
        self.t = FakeToggl()
        toggl.http = self.t
        app.Handler.chat = chatmod.ChatService(claude="stub", token=TOKEN)
        app.Handler.store = None
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), app.Handler)
        self.base = "http://127.0.0.1:%d" % self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()

    def req(self, method, path, body=None):
        r = urllib.request.Request(self.base + path, data=body and body.encode(), method=method,
                                   headers={"Authorization": "Bearer " + TOKEN})
        try:
            with urllib.request.urlopen(r, timeout=10) as resp:
                return resp.status, resp.read().decode()
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode()

    def test_idle_with_recent_entries_once_each_newest_first(self):
        s, text = self.req("GET", "/toggl/status")
        self.assertEqual(s, 200)
        self.assertEqual(text, "idle\n"
                               "recent\t\tEmail\t\t\n"
                               "recent\t10\tWriting\tCardOS\t#0b83d9\n"
                               "project\t10\tCardOS\t#0b83d9\n"
                               "project\t11\tHome\t\n")

    def test_start_stops_what_was_running_and_says_when_it_began(self):
        self.t.running = {"id": 7, "description": "Old", "project_id": None, "start": T0,
                          "duration": -T0_EPOCH, "workspace_id": 5}
        s, text = self.req("POST", "/toggl/start", "description=Writing\nproject=10")
        self.assertEqual((s, text), (200, "running\t99\t%d\tWriting\tCardOS\t#0b83d9\n" % T0_EPOCH))
        methods = [(m, r) for m, r, _ in self.t.calls]
        self.assertIn(("PATCH", "/workspaces/5/time_entries/7/stop"), methods)
        self.assertLess(methods.index(("PATCH", "/workspaces/5/time_entries/7/stop")),
                        methods.index(("POST", "/workspaces/5/time_entries")))
        sent = [b for m, r, b in self.t.calls if m == "POST"][0]
        self.assertEqual((sent["project_id"], sent["duration"]), (10, -1))
        s, text = self.req("GET", "/toggl/status")
        self.assertTrue(text.startswith("running\t99\t"))

    def test_status_lists_the_projects_to_start_from(self):
        s, text = self.req("GET", "/toggl/status")
        self.assertTrue(text.endswith("project\t10\tCardOS\t#0b83d9\nproject\t11\tHome\t\n"), text)

    def test_a_project_with_no_description_then_described_later(self):
        s, text = self.req("POST", "/toggl/start", "description=\nproject=11")
        self.assertEqual((s, text), (200, "running\t99\t%d\t\tHome\t\n" % T0_EPOCH))
        s, text = self.req("POST", "/toggl/describe", "description=Gutters")
        self.assertEqual((s, text), (200, "running\t99\t%d\tGutters\tHome\t\n" % T0_EPOCH))
        put = [b for m, r, b in self.t.calls if m == "PUT"][0]
        self.assertEqual(put, {"description": "Gutters"})
        self.t.running = None
        toggl.drop("current")
        self.assertEqual(self.req("POST", "/toggl/describe", "description=x")[1], "idle\n")

    def test_stop(self):
        self.req("POST", "/toggl/start", "description=Email\nproject=")
        self.assertEqual(self.req("POST", "/toggl/stop"), (200, "stopped\t120\tEmail\n"))
        self.assertEqual(self.req("POST", "/toggl/stop"), (200, "idle\n"))

    def test_today_counts_the_running_entry_into_the_total(self):
        self.t.running = {"id": 7, "description": "Now", "project_id": 11,
                          "start": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(time.time() - 60)),
                          "duration": -1, "workspace_id": 5}
        s, text = self.req("GET", "/toggl/today?from=2026-10-01T00:00:00Z&to=2026-10-02T00:00:00Z")
        lines = text.splitlines()
        self.assertEqual(lines[0].split("\t")[1:], ["1800", "Writing", "CardOS"])
        self.assertEqual(lines[-2].split("\t")[2:], ["Now", "Home"])
        total = int(lines[-1].split("\t")[1])
        self.assertTrue(1800 + 600 + 300 + 59 <= total <= 1800 + 600 + 300 + 65, total)

    def test_projects_and_recent_are_not_asked_for_twice(self):
        self.req("GET", "/toggl/status")
        self.req("GET", "/toggl/status")
        rels = [r for _, r, _ in self.t.calls]
        self.assertEqual(sum(r.startswith("/workspaces/5/projects") for r in rels), 1)
        self.assertEqual(sum(r.startswith("/me/time_entries?") for r in rels), 1)

    def test_refusals_say_what_to_do(self):
        toggl.save({"token": "bad", "workspace": 5})
        s, text = self.req("GET", "/toggl/status")
        self.assertEqual(s, 401)
        self.assertIn("/dash", text)
        toggl.save({"token": "good", "workspace": 5})
        self.t.status = 429
        s, text = self.req("GET", "/toggl/status")
        self.assertEqual(s, 429)
        self.assertIn("limit", text)

    def test_no_token_is_a_401(self):
        toggl.forget()
        self.assertEqual(self.req("GET", "/toggl/status")[0], 401)

    def test_check_token_names_the_account(self):
        self.assertEqual(toggl.check_token("good"), ("Alex", 5))
        with self.assertRaises(toggl.TogglError):
            toggl.check_token("bad")

    def test_epochs(self):
        self.assertEqual(toggl.epoch(T0), T0_EPOCH)
        self.assertEqual(toggl.epoch("2026-10-01T16:00:00Z"), T0_EPOCH)


if __name__ == "__main__":
    unittest.main()
