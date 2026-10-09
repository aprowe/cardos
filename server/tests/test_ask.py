"""chat.ask_once: the one-shot Claude call, and its five callers.

    python -m server.tests.test_ask
"""
import json
import os
import subprocess
import sys
import unittest
from unittest import mock
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from server import chat, claudeauth, daily, midi, talk, voice


class Run:
    """subprocess.run, standing in: records each command, answers as told."""

    def __init__(self, stdout=None, raise_=None, stderr=""):
        self.cmds = []
        self.stdout = stdout if stdout is not None else json.dumps(
            {"result": "OK", "session_id": "s-9"})
        self.raise_ = raise_
        self.stderr = stderr

    def __call__(self, cmd, **kw):
        self.cmds.append(cmd)
        if self.raise_:
            raise self.raise_
        return subprocess.CompletedProcess(cmd, 0, self.stdout, self.stderr)


class AskOnce(unittest.TestCase):

    def setUp(self):
        self.c = chat.ChatService(claude="claude-cli", model="claude-x")

    def run_with(self, run):
        p = mock.patch("server.chat.subprocess.run", run)
        p.start()
        self.addCleanup(p.stop)
        return run

    def test_the_command(self):
        run = self.run_with(Run())
        self.assertEqual(chat.ask_once(self.c, "hi", 30), ("OK", "s-9"))
        cmd = run.cmds[-1]
        self.assertEqual(cmd[:3], ["claude-cli", "-p", "hi"])
        self.assertEqual(cmd[cmd.index("--allowed-tools") + 1], "")
        self.assertEqual(cmd[cmd.index("--permission-mode") + 1], "dontAsk")
        self.assertNotIn("--model", cmd)            # only when asked for
        self.assertNotIn("--resume", cmd)
        chat.ask_once(self.c, "hi", 30, resume="s-1", system="be brief", model=True)
        cmd = run.cmds[-1]
        self.assertEqual(cmd[cmd.index("--model") + 1], "claude-x")
        self.assertEqual(cmd[cmd.index("--resume") + 1], "s-1")
        self.assertEqual(cmd[cmd.index("--append-system-prompt") + 1], "be brief")

    def test_failures_are_one_error(self):
        for run, why in ((Run(json.dumps({"is_error": True, "result": "Credit balance is too low"})),
                          "Credit balance is too low"),
                         (Run(json.dumps({"result": ""}), stderr="boom"), "boom"),
                         (Run("not json"), "not json"),
                         (Run(raise_=OSError("no such file")), "no such file")):
            self.run_with(run)
            with self.assertRaises(chat.ClaudeError) as e:
                chat.ask_once(self.c, "hi", 30)
            self.assertEqual(e.exception.why, why)
            self.assertFalse(e.exception.timed_out)
        self.run_with(Run(raise_=subprocess.TimeoutExpired("claude", 30)))
        with self.assertRaises(chat.ClaudeError) as e:
            chat.ask_once(self.c, "hi", 30)
        self.assertTrue(e.exception.timed_out)
        with self.assertRaises(chat.ClaudeError):
            chat.ask_once(chat.ChatService(claude=None), "hi", 30)


class Callers(unittest.TestCase):
    """Each keeps its timeout, and only the voice translator its --model."""

    def setUp(self):
        self.c = chat.ChatService(claude="claude-cli", model="claude-x")
        self.run = Run(json.dumps({"result": "FOCUS: one thing\nFACT: a fact"}))
        p = mock.patch("server.chat.subprocess.run", self.run)
        p.start()
        self.addCleanup(p.stop)
        self.timeouts = []
        real = chat.ask_once

        def spy(c, prompt, timeout, **kw):
            self.timeouts.append(timeout)
            return real(c, prompt, timeout, **kw)
        p = mock.patch("server.chat.ask_once", spy)
        p.start()
        self.addCleanup(p.stop)

    def test_timeouts_and_models(self):
        self.assertEqual(daily.generate("2026-10-09", self.c),
                         {"focus": "one thing", "fact": "a fact"})
        midi._claude(self.c, "a song")
        talk._claude(self.c, "a doc", "s-2")
        self.assertEqual(self.run.cmds[-1][-2:], ["--resume", "s-2"])
        claudeauth.status(self.c, fresh=True)
        self.assertNotIn("--model", sum(self.run.cmds, []))
        self.assertEqual(voice.Voice().command("open notes", self.c), "FOCUS: one thing")
        self.assertIn("--model", self.run.cmds[-1])
        self.assertEqual(self.timeouts, [90, 240, 300, 90, 60])

    def test_a_failure_is_what_each_did_with_one(self):
        self.run.stdout = json.dumps({"is_error": True, "result": "Credit balance is too low"})
        self.assertTrue(daily.generate("2026-10-09", self.c)["fallback"])
        with self.assertRaises(RuntimeError):
            midi._claude(self.c, "a song")
        st = claudeauth.status(self.c, fresh=True)
        self.assertEqual((st["ok"], st["why"]), (False, "Credit balance is too low"))
        self.assertEqual(voice.Voice().command("open notes", self.c),
                         "none Credit balance is too low")
        self.run.raise_ = subprocess.TimeoutExpired("claude", 60)
        self.assertEqual(voice.Voice().command("open notes", self.c),
                         "none the translator timed out")


if __name__ == "__main__":
    unittest.main()
