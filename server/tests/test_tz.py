"""/tz: the device's time zone, as the POSIX rule its TZ needs.

The geolocation service is stubbed -- a test that depends on a stranger's
server and on where the test runs is not a test. The zone database is real.

    python -m server.tests.test_tz
"""
import os, sys, threading, unittest, urllib.request
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))             # the repository root

from server import app, tz
from server import chat as chatmod
from http.server import ThreadingHTTPServer


class Posix(unittest.TestCase):
    """The rule comes from the zone file's footer, not a table of ours."""

    def test_known_zones(self):
        self.assertEqual(tz.posix_rule("America/Los_Angeles"), "PST8PDT,M3.2.0,M11.1.0")
        self.assertEqual(tz.posix_rule("Europe/London"), "GMT0BST,M3.5.0/1,M10.5.0")
        self.assertEqual(tz.posix_rule("Asia/Tokyo"), "JST-9")

    def test_nonsense_is_none(self):
        self.assertIsNone(tz.posix_rule("Not/AZone"))
        self.assertIsNone(tz.posix_rule("../../etc/passwd"))
        self.assertIsNone(tz.posix_rule(""))


class WhoAsked(unittest.TestCase):

    def test_private_and_loopback_mean_this_server(self):
        for ip in ("192.168.1.183", "10.0.0.5", "127.0.0.1", "172.16.3.4", "", None):
            self.assertIsNone(tz.public_ip(ip), ip)

    def test_public_is_kept(self):
        self.assertEqual(tz.public_ip("75.50.125.137"), "75.50.125.137")


class Route(unittest.TestCase):

    def setUp(self):
        self.asked = []

        def fake_zone(ip):
            self.asked.append(ip)
            return {"75.50.125.137": "America/Los_Angeles",
                    None: "Europe/London",
                    "8.8.8.8": "Not/AZone"}.get(ip)
        tz.zone_for_ip = fake_zone
        tz.CACHE.clear()
        app.Handler.chat = chatmod.ChatService(claude="stub")
        app.Handler.store = None
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), app.Handler)
        self.base = "http://127.0.0.1:%d" % self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()

    def get(self, real_ip=None):
        req = urllib.request.Request(self.base + "/tz")
        if real_ip:
            req.add_header("X-Real-IP", real_ip)       # what nginx sets
        return urllib.request.urlopen(req, timeout=5).read().decode()

    def test_the_callers_zone_as_a_rule(self):
        self.assertEqual(self.get("75.50.125.137"),
                         "tz PST8PDT,M3.2.0,M11.1.0\nzone America/Los_Angeles\n")

    def test_a_lan_caller_gets_where_the_server_is(self):
        self.assertEqual(self.get(),                   # 127.0.0.1, no header
                         "tz GMT0BST,M3.5.0/1,M10.5.0\nzone Europe/London\n")
        self.assertEqual(self.asked, [None])

    def test_asked_once_per_address(self):
        self.get("75.50.125.137")
        self.get("75.50.125.137")
        self.assertEqual(self.asked, ["75.50.125.137"])

    def test_a_zone_with_no_rule_is_an_error_line(self):
        self.assertTrue(self.get("8.8.8.8").startswith("error "))

    def test_no_answer_is_an_error_line(self):
        self.assertTrue(self.get("1.2.3.4").startswith("error "))


if __name__ == "__main__":
    unittest.main(verbosity=1)
