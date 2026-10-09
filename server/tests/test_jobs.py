"""server/jobs.py: what is kept between requests, and for how long.

    python -m server.tests.test_jobs
"""
import os
import sys
import unittest
from unittest import mock
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from server import accounts, jobs


class Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


class TableTest(unittest.TestCase):

    def setUp(self):
        self.clock = Clock()
        p = mock.patch("server.jobs.time.time", self.clock)
        p.start()
        self.addCleanup(p.stop)
        self.addCleanup(accounts.set_current, None)

    def test_an_entry_goes_after_its_ttl(self):
        t = jobs.Table(ttl=60)
        t.put("a", 1)
        self.clock.t += 59
        self.assertEqual(t.get("a"), 1)
        self.clock.t += 2
        self.assertIsNone(t.get("a"))               # expired, though not swept yet
        t.put("b", 2)                               # a put sweeps
        self.assertNotIn("a", t)
        self.assertEqual(len(t), 1)

    def test_touch_keeps_it(self):
        t = jobs.Table(ttl=60)
        t.put("s", "session")
        for _ in range(5):
            self.clock.t += 50
            self.assertEqual(t.get("s", touch=True), "session")
        t.put("x", 0)
        self.assertIn("s", t)

    def test_cap_drops_the_oldest(self):
        t = jobs.Table(ttl=1e9, cap=3)
        for k in "abcd":
            self.clock.t += 1
            t.put(k, k)
        self.assertNotIn("a", t)
        self.assertEqual(len(t), 3)

    def test_one_persons_entries_are_not_anothers(self):
        t = jobs.Table(ttl=60)
        accounts.set_current("sam")
        t.put("j", "hers")
        accounts.set_current("alex")
        self.assertIsNone(t.get("j"))
        self.assertIsNone(t.pop("j"))
        self.assertIn("j", t)
        accounts.set_current("sam")
        self.assertEqual(t.pop("j"), "hers")
        self.assertNotIn("j", t)

    def test_a_thread_finishing_keeps_the_owner_and_restarts_the_clock(self):
        t = jobs.Table(ttl=60)
        t.put("j", "pending", owner="sam")
        self.clock.t += 50
        self.assertEqual(t.get("j", owner=jobs.ANY), "pending")
        self.assertTrue(t.replace("j", "done"))
        self.clock.t += 50
        self.assertEqual(t.get("j", owner="sam"), "done")
        self.assertIsNone(t.get("j", owner=None))
        t.remove_if(lambda owner, v: owner == "sam")
        self.assertFalse(t.replace("j", "late"))    # forgotten meanwhile: not put back
        self.assertNotIn("j", t)


class LRUTest(unittest.TestCase):

    def test_least_recently_used_goes_first(self):
        c = jobs.LRU(2)
        c["a"] = 1
        c["b"] = 2
        self.assertEqual(c.get("a"), 1)             # a is the recent one now
        c["c"] = 3
        self.assertNotIn("b", c)
        self.assertEqual((c["a"], c.get("c"), len(c)), (1, 3, 2))
        self.assertIsNone(c.get("b"))
        c.clear()
        self.assertEqual(len(c), 0)


class ChatJobs(unittest.TestCase):

    def test_an_answer_nobody_reads_does_not_stay(self):
        from server import chat
        clock = Clock()
        with mock.patch("server.jobs.time.time", clock):
            c = chat.ChatService(claude="stub")
            c.jobs.put(1, {"state": "done", "reply": "x" * 3000, "user": None}, owner=None)
            clock.t += chat.JOB_TTL + 1
            c.jobs.put(2, {"state": "pending", "user": None}, owner=None)
            self.assertNotIn(1, c.jobs)
            self.assertEqual(c.poll(1), ("error", "no such request"))


if __name__ == "__main__":
    unittest.main()
