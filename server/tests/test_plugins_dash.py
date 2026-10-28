"""What a plugin may give the dashboard (server/plugins.py): DASH_VIEWS and
DASH_CARDS are checked when the server starts, read from the plugin's own
folder, and dash_state() fills in /dash/api/state under its name.

    python -m server.tests.test_plugins_dash
"""
import os
import sys
import tempfile
import types
import unittest
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from server import plugins


def fake(**kw):
    """A plugin module in a folder of its own, with dash/v.html and v.js."""
    d = tempfile.mkdtemp()
    os.makedirs(os.path.join(d, "dash"))
    for n, text in (("v.html", "<p>hi</p>"), ("v.js", "dashPlugin('x',{});")):
        with open(os.path.join(d, "dash", n), "w") as f:
            f.write(text)
    return types.SimpleNamespace(__file__=os.path.join(d, "__init__.py"), **kw)


VIEW = {"id": "thing", "title": "Thing", "html": "dash/v.html", "js": "dash/v.js"}


class Check(unittest.TestCase):

    def test_a_good_view_and_card(self):
        plugins._check_dash("x", fake(DASH_VIEWS=[VIEW],
                                      DASH_CARDS=[dict(VIEW, id="card", view="accounts")]))

    def test_refused(self):
        for why, mod in (
                ("bad id", fake(DASH_VIEWS=[dict(VIEW, id="Bad Id")])),
                ("ours", fake(DASH_VIEWS=[dict(VIEW, id="accounts")])),
                ("no title", fake(DASH_VIEWS=[dict(VIEW, title="")])),
                ("missing", fake(DASH_VIEWS=[dict(VIEW, js="dash/none.js")])),
                ("outside", fake(DASH_VIEWS=[dict(VIEW, html="../../etc/passwd")])),
                ("card's page", fake(DASH_CARDS=[dict(VIEW, view="server")]))):
            with self.assertRaises(ValueError, msg=why):
                plugins._check_dash("x", mod)


class Parts(unittest.TestCase):

    def setUp(self):
        self.addCleanup(setattr, plugins, "_loaded", plugins._loaded)

    def test_views_in_order_cards_and_a_taken_id(self):
        plugins._loaded = {
            "b": fake(DASH_VIEWS=[dict(VIEW, id="late", order=70), dict(VIEW, id="early", order=5)]),
            "c": fake(DASH_VIEWS=[dict(VIEW, id="late")],
                      DASH_CARDS=[dict(VIEW, id="card", view="accounts")])}
        p = plugins.dash_parts()
        self.assertEqual([(v["plugin"], v["id"]) for v in p["views"]], [("b", "early"), ("b", "late")])
        self.assertEqual(p["views"][0]["html"], "<p>hi</p>")
        self.assertEqual(p["views"][0]["short"], "Thing")
        self.assertEqual([(c["plugin"], c["id"], c["view"]) for c in p["cards"]],
                         [("c", "card", "accounts")])

    def test_dash_states(self):
        def boom():
            raise RuntimeError("no")
        plugins._loaded = {"good": fake(dash_state=lambda: {"n": 1}), "bad": fake(dash_state=boom),
                           "none": fake(), "session": fake(dash_state=lambda: 1)}
        st = plugins.dash_states(skip={"session": {}})
        self.assertEqual(st["good"], {"n": 1})
        self.assertIn("no", st["bad"]["error"])
        self.assertNotIn("none", st)
        self.assertNotIn("session", st)


if __name__ == "__main__":
    unittest.main()
