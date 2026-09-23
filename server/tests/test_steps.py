"""A request as a plan and steps, with live status and per-step timeouts.

The streaming runner is tested against a real child process -- a few lines of
Python pretending to be `claude --output-format stream-json` -- because the
idle timeout is about a process going quiet, and a fake that is not a process
cannot go quiet. The planner and the steps are stubbed above that.

    python -m server.tests.test_steps
"""
import json, os, sys, threading, time, unittest, urllib.request
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))             # the repository root

from server import app, chat
from http.server import ThreadingHTTPServer


def fake_claude(events, gap=0.05, hang=0):
    """argv for a child that prints `events` as JSON lines, `gap` apart, then
    sleeps `hang` seconds before exiting."""
    code = ("import json,sys,time\n"
            "for e in json.loads(sys.argv[1]):\n"
            "    print(json.dumps(e), flush=True); time.sleep(%r)\n"
            "time.sleep(%r)\n" % (gap, hang))
    return [sys.executable, "-c", code, json.dumps(events)]


INIT = {"type": "system", "subtype": "init", "session_id": "s-1"}


def tool(name, **inp):
    return {"type": "assistant", "session_id": "s-1",
            "message": {"content": [{"type": "tool_use", "name": name, "input": inp}]}}


def result(text, sid="s-1"):
    return {"type": "result", "subtype": "success", "result": text,
            "session_id": sid, "is_error": False}


class Describe(unittest.TestCase):

    def test_tool_calls_read_as_what_is_happening(self):
        d = chat.describe
        self.assertEqual(d(tool("Read", file_path="/r/kernel/app/capp.h")), "reading capp.h")
        self.assertEqual(d(tool("Edit", file_path="/r/apps/timer.c")), "editing timer.c")
        self.assertEqual(d(tool("Write", file_path="/r/apps/timer.c")), "writing timer.c")
        self.assertEqual(d(tool("Grep", pattern="x")), "searching")
        self.assertEqual(d(tool("Glob", pattern="x")), "searching")
        self.assertIsNone(d(INIT))
        self.assertIsNone(d(result("hi")))


class Stream(unittest.TestCase):

    def test_result_and_session_and_status(self):
        seen = []
        text, sid = chat.run_stream(
            fake_claude([INIT, tool("Read", file_path="a/b.c"), result("done it")]),
            env=None, cwd=None, on_status=seen.append, idle=5, cap=10)
        self.assertEqual((text, sid), ("done it", "s-1"))
        self.assertEqual(seen, ["reading b.c"])

    def test_a_silent_process_is_stopped_by_the_idle_timeout(self):
        t = time.time()
        with self.assertRaises(chat.TurnTimeout) as cm:
            chat.run_stream(fake_claude([INIT], hang=30), env=None, cwd=None,
                            on_status=None, idle=1, cap=20)
        self.assertLess(time.time() - t, 5)
        self.assertIn("quiet", str(cm.exception))

    def test_a_busy_process_is_stopped_by_the_cap(self):
        many = [tool("Grep", pattern="x")] * 200
        with self.assertRaises(chat.TurnTimeout) as cm:
            chat.run_stream(fake_claude(many, gap=0.05), env=None, cwd=None,
                            on_status=None, idle=5, cap=1)
        self.assertIn("longer than", str(cm.exception))

    def test_activity_keeps_a_long_turn_alive(self):
        many = [tool("Grep", pattern="x")] * 15 + [result("finished")]
        text, _ = chat.run_stream(fake_claude(many, gap=0.1), env=None, cwd=None,
                                  on_status=None, idle=1, cap=10)
        self.assertEqual(text, "finished")


class Plan(unittest.TestCase):

    def test_numbered_lines(self):
        self.assertEqual(chat.parse_plan("1. write it\n2) test it\n", "x"),
                         ["write it", "test it"])

    def test_chatter_around_the_list_is_ignored(self):
        self.assertEqual(chat.parse_plan("Sure:\n1. a\n\n2. b\nThat is all.", "x"),
                         ["a", "b"])

    def test_no_list_means_one_step_the_request(self):
        self.assertEqual(chat.parse_plan("I would just do it.", "make it blue"),
                         ["make it blue"])

    def test_too_many_is_cut(self):
        text = "\n".join("%d. s%d" % (i, i) for i in range(1, 12))
        self.assertEqual(len(chat.parse_plan(text, "x")), chat.MAX_STEPS)


class Stepped(chat.ChatService):
    """run_turn over a stubbed planner and stubbed turns."""

    def __init__(self, plan, fail_at=None):
        super().__init__(claude="stub")
        self.plan_steps = plan
        self.fail_at = fail_at
        self.prompts = []

    def _plan(self, text):
        return list(self.plan_steps)

    def _claude(self, text, on_status=None, **kw):
        n = len(self.prompts) + 1
        self.prompts.append(text)
        if on_status:
            on_status("editing timer.c")
        if n == self.fail_at:
            raise chat.TurnTimeout("went quiet for 240 s")
        return "did step %d" % n


class Steps(unittest.TestCase):

    def test_one_step_is_the_request_unchanged(self):
        c = Stepped(["make the flippers stronger"])
        state, reply = c.run_turn("make the flippers stronger")
        self.assertEqual(state, "done")
        self.assertEqual(c.prompts, ["make the flippers stronger"])
        self.assertEqual(reply, "did step 1")

    def test_each_step_is_its_own_turn_and_reports(self):
        c = Stepped(["write skeleton", "add countdown", "add keys"])
        seen = []
        state, reply = c.run_turn("make a timer app", report=seen.append)
        self.assertEqual(state, "done")
        self.assertEqual(len(c.prompts), 3)
        self.assertIn("step 2", c.prompts[1])
        self.assertIn("add countdown", c.prompts[1])
        self.assertIn("make a timer app", c.prompts[1])
        self.assertEqual(seen[0], "planning")
        self.assertIn("step 2/3: editing timer.c", seen)
        self.assertIn("[x] 3. add keys", reply)
        self.assertIn("did step 3", reply)

    def test_a_step_that_times_out_stops_the_rest_and_says_where(self):
        c = Stepped(["a", "b", "c"], fail_at=2)
        state, reply = c.run_turn("x")
        self.assertEqual(state, "error")
        self.assertEqual(len(c.prompts), 2)
        self.assertIn("[x] 1. a", reply)
        self.assertIn("[ ] 3. c", reply)
        self.assertIn("step 2 stopped: went quiet for 240 s", reply)

    def test_a_reset_mid_job_stops_before_the_next_step(self):
        c = Stepped(["a", "b", "c"])
        real = c._claude

        def claude_then_reset(text, **kw):
            out = real(text, **kw)
            c.reset()
            return out
        c._claude = claude_then_reset
        c.run_turn("x")
        self.assertEqual(len(c.prompts), 1)


class PollShowsStatus(unittest.TestCase):

    def setUp(self):
        self.gate = threading.Event()
        gate = self.gate

        class Slow(chat.ChatService):
            def run_turn(self, text, report=None):
                report("step 1/2: reading capp.h")
                gate.wait(5)
                return "done", "ok"

        app.Handler.chat = Slow(claude="stub")
        app.Handler.store = None
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), app.Handler)
        self.base = "http://127.0.0.1:%d" % self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def tearDown(self):
        self.gate.set()
        self.srv.shutdown()
        self.srv.server_close()

    def get(self, path, data=None):
        req = urllib.request.Request(self.base + path, data=data,
                                     method="POST" if data else "GET")
        return urllib.request.urlopen(req, timeout=5).read().decode()

    def test_pending_carries_the_status_line(self):
        jid = self.get("/chat", b"make a timer app").split()[1]
        time.sleep(0.2)
        self.assertEqual(self.get("/chat?id=" + jid), "pending\nstep 1/2: reading capp.h")
        self.gate.set()
        time.sleep(0.2)
        self.assertEqual(self.get("/chat?id=" + jid), "done\nok")


if __name__ == "__main__":
    unittest.main(verbosity=1)
