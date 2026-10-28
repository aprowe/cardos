"""The Traitors' round-table talk (server/traitors.py), with Claude stubbed.

    python -m server.tests.test_traitors
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from server import ask, traitors  # noqa: E402

BODY = """round 2
seat 2 Ingrid traitor 4 4
seat 3 Corvin faithful 2 2
person 0 Alex
person 4 Agatha
gone 5 Lucan banished-faithful
gone 6 Esme murdered
log Round 1: Morwen led Silas and Ingrid; 1 lead coin; spoiled.
log Lucan was banished; Faithful.
nonsense line
seat x bad
"""


class Parse(unittest.TestCase):
    def test_the_situation_is_read_and_junk_ignored(self):
        s = traitors.parse(BODY)
        self.assertEqual(s["round"], 2)
        self.assertEqual([p["name"] for p in s["speakers"]], ["Ingrid", "Corvin"])
        self.assertEqual(s["speakers"][0]["role"], "traitor")
        self.assertEqual(s["speakers"][0]["vote"], 4)
        self.assertEqual(len(s["people"]), 2)
        self.assertEqual(len(s["gone"]), 2)
        self.assertEqual(len(s["log"]), 2)

    def test_the_prompt_names_each_target_and_keeps_secrets_rules(self):
        s = traitors.parse(BODY)
        p = traitors.prompt_for(s)
        self.assertIn("Ingrid (seat 2) is secretly a Traitor", p)
        self.assertIn("banish Agatha", p)                  # Ingrid's vote
        self.assertIn("Corvin (seat 3) is Faithful", p)
        self.assertIn("banish Ingrid", p)                  # Corvin's vote
        self.assertIn("Lucan (banished, was Faithful)", p)
        self.assertIn("Nobody may state anyone's secret role", p)
        schema = traitors.schema_for(s)
        ask.check_schema(schema)
        self.assertEqual(schema["properties"]["lines"]["minItems"], 2)


class Answer(unittest.TestCase):
    def test_lines_come_back_in_order_and_drawable(self):
        s = traitors.parse(BODY)
        value = {"lines": [{"seat": 3, "text": "Ingrid’s crew came back — spoiled.\nOdd."},
                           {"seat": 2, "text": "x" * 300}]}
        out = traitors.answer(value, s)
        lines = out.splitlines()
        self.assertEqual(lines[0].split("\t")[0], "2")
        self.assertEqual(len(lines[0].split("\t")[1]), traitors.LINE_MAX)
        self.assertEqual(lines[1], "3\tIngrid's crew came back -- spoiled. Odd.")

    def test_a_run_that_fails_is_an_error_not_a_crash(self):
        s = traitors.parse(BODY)
        saved = ask.ask_shape

        def boom(*a, **k):
            raise RuntimeError("no Claude")
        ask.ask_shape = boom
        try:
            traitors._jobs.put("j1", ("pending", None))
            traitors._run("j1", None, s, None)
            self.assertEqual(traitors._jobs.get("j1"), ("error", "no Claude"))
        finally:
            ask.ask_shape = saved

    def test_a_good_run_keeps_the_lines(self):
        s = traitors.parse(BODY)
        saved = ask.ask_shape
        ask.ask_shape = lambda *a, **k: {"lines": [{"seat": 2, "text": "Agatha."}, {"seat": 3, "text": "Ingrid."}]}
        try:
            traitors._jobs.put("j2", ("pending", None))
            traitors._run("j2", None, s, None)
            self.assertEqual(traitors._jobs.get("j2"), ("ok", "2\tAgatha.\n3\tIngrid.\n"))
        finally:
            ask.ask_shape = saved


if __name__ == "__main__":
    unittest.main()
