"""Calendar and Tasks through the server, against a Google that is a dict.

What the device relies on: the line formats, every page gathered, Google's
status passed through (a 404 stays a 404 -- Todo drops an edit to a task
deleted in a browser on exactly that), and a login that refreshes itself.

    python -m server.tests.test_google
"""
import json
import os
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from http.server import ThreadingHTTPServer
from server import app, dash, google
from server import chat as chatmod


class FakeGoogle:
    """Enough of Google: a token endpoint, events in two pages, lists, tasks."""

    def __init__(self):
        self.calls = []
        self.tasks = {"L1": [{"id": "t1", "title": "Bins", "status": "needsAction"},
                             {"id": "t2", "title": "Tab\there", "status": "completed"}]}
        self.refreshes = 0

    def __call__(self, method, url, body=None, headers=None, form=False):
        self.calls.append((method, url, body))
        if url == google.TOKEN_URL:
            self.refreshes += 1
            if body["refresh_token"] == "bad":
                return 400, {"error": "invalid_grant"}
            return 200, {"access_token": "AT%d" % self.refreshes, "expires_in": 3600}
        assert headers and headers["Authorization"].startswith("Bearer AT")
        if url.startswith(google.CAL) and method == "GET":
            if "fields=defaultReminders" in url:
                return 200, {"defaultReminders": [{"method": "email", "minutes": 60},
                                                  {"method": "popup", "minutes": 30}]}
            if "pageToken=p2" in url:
                return 200, {"items": [{"id": "e2", "summary": "Dinner",
                                        "start": {"date": "2026-09-30"},
                                        "end": {"date": "2026-10-01"},
                                        "reminders": {"useDefault": False, "overrides": [
                                            {"method": "popup", "minutes": 10}]}}]}
            return 200, {"nextPageToken": "p2", "items": [
                {"id": "e1", "summary": "Interview",
                 "start": {"dateTime": "2026-09-29T13:00:00-07:00"},
                 "end": {"dateTime": "2026-09-29T14:00:00-07:00"}}]}
        if url.startswith(google.CAL) and method in ("POST", "PATCH"):
            return 200, {"id": url.rsplit("/", 1)[-1] if method == "PATCH" else "new1"}
        if "/users/@me/lists" in url and method == "GET":
            return 200, {"items": [{"id": "L1", "title": "Chores"}, {"id": "L2", "title": "Packing"}]}
        if "/users/@me/lists" in url and method == "POST":
            return 200, {"id": "L3", "title": body["title"]}
        if "/lists/L1/tasks" in url and method == "GET":
            return 200, {"items": self.tasks["L1"]}
        if "/lists/L1/tasks/gone" in url:
            return 404, {"error": {"message": "Not Found"}}
        if "/lists/L1/tasks" in url and method == "POST":
            return 200, {"id": "t9"}
        if "/lists/L1/tasks/" in url and method in ("PATCH", "DELETE"):
            return 200 if method == "PATCH" else 204, {} if method == "PATCH" else None
        return 404, {"error": {"message": "no such thing"}}


def sign_in(refresh):
    """What the dashboard's sign-in leaves (dash.get_google_callback)."""
    dash.save_creds({"client_id": "cid", "client_secret": "secret", "refresh_token": refresh})
    google.forget_access()


class Routes(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        os.environ["CARDOS_STATE"] = self.dir
        google._access.update(token=None, until=0)
        google._ids = None
        sign_in("good")
        self.g = FakeGoogle()
        google.http = self.g
        app.Handler.chat = chatmod.ChatService(claude="stub")
        app.Handler.store = None
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), app.Handler)
        self.base = "http://127.0.0.1:%d" % self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()

    def req(self, method, path, body=None):
        r = urllib.request.Request(self.base + path, method=method,
                                   data=body.encode() if body is not None else None)
        try:
            with urllib.request.urlopen(r, timeout=5) as resp:
                return resp.status, resp.read().decode()
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode()

    def test_events_every_page_as_lines(self):
        s, text = self.req("GET", "/calendar/events?from=2026-09-29T00:00:00Z&to=2026-11-28T00:00:00Z")
        self.assertEqual(s, 200)
        e1, e2 = google.short_ids(["e1", "e2"])
        self.assertEqual(text,
            e1 + "\t2026-09-29T13:00:00-07:00\t2026-09-29T14:00:00-07:00\tInterview\n" +
            e2 + "\t2026-09-30\t2026-10-01\tDinner\n")

    def test_events_with_their_reminders_when_asked(self):
        s, text = self.req("GET", "/calendar/events?from=2026-09-29T00:00:00Z&to=2026-11-28T00:00:00Z&remind=1")
        e1, e2 = google.short_ids(["e1", "e2"])
        self.assertEqual(text,
            e1 + "\t2026-09-29T13:00:00-07:00\t2026-09-29T14:00:00-07:00\tInterview\t30\n" +
            e2 + "\t2026-09-30\t2026-10-01\tDinner\t10\n")

    def test_event_add_and_change(self):
        s, text = self.req("POST", "/calendar/event",
                           "start=2026-09-30T17:00:00Z\nend=2026-09-30T18:00:00Z\nsummary=Gym")
        self.assertEqual((s, text), (200, google.short_ids(["new1"])[0] + "\n"))
        sent = [c for c in self.g.calls if c[0] == "POST" and c[1] == google.CAL][0][2]
        self.assertEqual(sent, {"summary": "Gym", "start": {"dateTime": "2026-09-30T17:00:00Z"},
                                "end": {"dateTime": "2026-09-30T18:00:00Z"}})
        e1 = google.short_ids(["e1"])[0]
        s, text = self.req("POST", "/calendar/event?id=" + e1,
                           "start=2026-09-30\nend=2026-10-01\nsummary=All day")
        self.assertEqual((s, text), (200, e1 + "\n"))
        self.assertTrue(self.g.calls[-1][1].endswith("/e1"))
        self.assertEqual(self.g.calls[-1][2]["start"], {"date": "2026-09-30"})

    def test_a_long_id_is_short_on_the_device_and_long_at_google(self):
        long = "_60q30c1g60o30e1i60o4ac1g60rj8gpl88rj2c1h84s34h9g60s30c1g" * 3 + "_20260930T003000Z"
        s = google.short_ids([long])[0]
        self.assertEqual(len(s), 12)
        self.assertEqual(google.short_ids([long])[0], s)          # the same every time
        google._ids = None                                         # a restart
        self.assertEqual(google.long_id(s), long)                  # read back from ids.json
        self.req("POST", "/calendar/event?id=" + s, "start=2026-09-30\nend=2026-10-01")
        self.assertTrue(self.g.calls[-1][1].endswith("/" + long))

    def test_an_id_it_never_gave_out_goes_as_it_is(self):
        # An edit queued on a device before the ids were short.
        self.req("POST", "/calendar/event?id=e1", "start=2026-09-30\nend=2026-10-01")
        self.assertTrue(self.g.calls[-1][1].endswith("/e1"))

    def test_lists_and_tasks(self):
        self.assertEqual(self.req("GET", "/todo/lists"), (200, "L1\tChores\nL2\tPacking\n"))
        self.assertEqual(self.req("GET", "/todo/tasks?list=L1"),
                         (200, "t1\t0\tBins\nt2\t1\tTab here\n"))
        self.assertEqual(self.req("POST", "/todo/task?list=L1", "Milk"), (200, "t9\n"))
        self.assertEqual(self.req("PATCH", "/todo/task?list=L1&id=t1", "done=1"), (200, "ok\n"))
        self.assertEqual(self.g.calls[-1][2], {"status": "completed"})
        self.assertEqual(self.req("DELETE", "/todo/task?list=L1&id=t1"), (200, "ok\n"))
        self.assertEqual(self.req("POST", "/todo/lists", "Groceries"), (200, "L3\n"))

    def test_max_keeps_the_open_tasks(self):
        self.g.tasks["L1"] = [{"id": "d%d" % i, "title": "done %d" % i, "status": "completed"}
                              for i in range(5)] + [{"id": "o1", "title": "x" * 90,
                                                     "status": "needsAction"}]
        s, text = self.req("GET", "/todo/tasks?list=L1&max=2")
        self.assertEqual(text, "o1\t0\t%s\nd0\t1\tdone 0\n" % ("x" * 60))

    def test_googles_404_is_the_devices_404(self):
        s, text = self.req("PATCH", "/todo/task?list=L1&id=gone", "done=1")
        self.assertEqual(s, 404)
        self.assertTrue(text.startswith("error "))

    def test_the_login_is_refreshed_once_and_kept(self):
        self.req("GET", "/todo/lists")
        self.req("GET", "/todo/lists")
        self.assertEqual(self.g.refreshes, 1)

    def test_a_bad_login_says_what_fixes_it(self):
        sign_in("bad")
        s, text = self.req("GET", "/todo/lists")
        self.assertEqual(s, 401)
        self.assertIn("/dash", text)

    def test_missing_arguments_are_a_400(self):
        self.assertEqual(self.req("GET", "/calendar/events?from=x")[0], 400)
        self.assertEqual(self.req("GET", "/todo/tasks")[0], 400)


if __name__ == "__main__":
    unittest.main(verbosity=1)


class Reminders(unittest.TestCase):
    def test_which_minutes(self):
        d = [{"method": "popup", "minutes": 30}, {"method": "popup", "minutes": 10}]
        self.assertEqual(google.popup_minutes(None, d), 10)                       # the default, soonest
        self.assertEqual(google.popup_minutes({"useDefault": True}, d), 10)
        self.assertEqual(google.popup_minutes({"useDefault": False, "overrides": [
            {"method": "popup", "minutes": 45}]}, d), 45)
        self.assertEqual(google.popup_minutes({"useDefault": False}, d), -1)      # none set
        self.assertEqual(google.popup_minutes(None, [{"method": "email", "minutes": 5}]), -1)
