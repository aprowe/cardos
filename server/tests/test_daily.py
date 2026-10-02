"""The day's focus and fun fact: one line each, the same all day.

    python -m server.tests.test_daily
"""
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
from server import app, daily
from server import chat as chatmod

TOKEN = "tok"


class DailyTest(unittest.TestCase):
    def setUp(self):
        os.environ["CARDOS_STATE"] = tempfile.mkdtemp()
        self.calls = 0
        real = daily.generate

        def gen(date, chat):
            self.calls += 1
            return {"focus": "Breathe before replying. (%s)" % date,
                    "fact": "Wombat droppings are cubes."}
        daily.generate = gen
        self.addCleanup(setattr, daily, "generate", real)
        app.Handler.chat = chatmod.ChatService(claude="stub", token=TOKEN)
        app.Handler.store = None
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), app.Handler)
        self.base = "http://127.0.0.1:%d" % self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()

    def req(self, q):
        r = urllib.request.Request(self.base + "/daily" + q,
                                   headers={"Authorization": "Bearer " + TOKEN})
        try:
            with urllib.request.urlopen(r, timeout=10) as resp:
                return resp.status, resp.read().decode()
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode()

    def test_one_line_each_and_the_same_all_day(self):
        self.assertEqual(self.req("?date=2026-10-02&kind=focus"),
                         (200, "Breathe before replying. (2026-10-02)\n"))
        self.assertEqual(self.req("?date=2026-10-02&kind=fact"),
                         (200, "Wombat droppings are cubes.\n"))
        self.assertEqual(self.calls, 1)               # asked once for the day
        self.req("?date=2026-10-03&kind=focus")
        self.assertEqual(self.calls, 2)               # a new day, a new ask

    def test_a_fallback_is_not_kept_for_the_day(self):
        daily.generate = lambda date, chat: (self.__dict__.__setitem__("calls", self.calls + 1)
                                             or daily.fallback(date))
        self.req("?date=2026-10-04&kind=focus")
        self.req("?date=2026-10-04&kind=fact")
        self.assertEqual(self.calls, 2)               # asked again: Claude may be back

    def test_bad_arguments(self):
        self.assertEqual(self.req("?date=tomorrow&kind=focus")[0], 400)
        self.assertEqual(self.req("?date=2026-10-02&kind=joke")[0], 400)

    def test_the_fallback_reads_its_lines_out_of_claudes_answer_or_its_own(self):
        f = daily.fallback("2026-10-02")
        self.assertIn(f["focus"], daily.FOCUS)
        self.assertIn(f["fact"], daily.FACT)


if __name__ == "__main__":
    unittest.main()
