"""The device's Claude app through the server: server/agent.py."""
import io
import json
import os
import unittest
import urllib.error

from server import agent, claudeauth


class FakeResp(io.BytesIO):
    status = 200

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


class Agent(unittest.TestCase):
    def setUp(self):
        self.env = {k: os.environ.pop(k) for k in ("ANTHROPIC_API_KEY", "CLAUDE_CODE_OAUTH_TOKEN")
                    if k in os.environ}
        self.addCleanup(os.environ.update, self.env)
        real = claudeauth.saved_token
        claudeauth.saved_token = lambda: ""
        self.addCleanup(setattr, claudeauth, "saved_token", real)

    def test_no_login_says_so(self):
        status, body = agent.forward(b"{}")
        self.assertEqual(status, 503)
        self.assertIn("dashboard", json.loads(body)["error"]["message"])

    def test_it_forwards_with_the_servers_login(self):
        seen = {}

        def opener(req, timeout):
            seen["url"], seen["headers"], seen["data"] = req.full_url, dict(req.header_items()), req.data
            return FakeResp(b'{"content":[{"type":"text","text":"hi"}]}')
        os.environ["CLAUDE_CODE_OAUTH_TOKEN"] = "sk-ant-oat01-xyz"
        self.addCleanup(os.environ.pop, "CLAUDE_CODE_OAUTH_TOKEN", None)
        status, body = agent.forward(b'{"model":"m"}', opener=opener)
        self.assertEqual(status, 200)
        self.assertEqual(seen["url"], agent.API)
        self.assertEqual(seen["headers"]["Authorization"], "Bearer sk-ant-oat01-xyz")
        self.assertEqual(seen["headers"]["Anthropic-beta"], "oauth-2025-04-20")
        sent = json.loads(seen["data"])
        self.assertEqual(sent["model"], "m")
        self.assertEqual(sent["system"][0]["text"], agent.IDENTITY)     # a login needs it
        # a system prompt of its own is kept, after it, and not doubled
        agent.forward(json.dumps({"system": "be brief"}).encode(), opener=opener)
        self.assertEqual([b["text"] for b in json.loads(seen["data"])["system"]],
                         [agent.IDENTITY, "be brief"])
        agent.forward(seen["data"], opener=opener)
        self.assertEqual(len(json.loads(seen["data"])["system"]), 2)
        # an API key wins
        os.environ["ANTHROPIC_API_KEY"] = "sk-ant-api-1"
        self.addCleanup(os.environ.pop, "ANTHROPIC_API_KEY", None)
        agent.forward(b"{}", opener=opener)
        self.assertEqual(seen["headers"]["X-api-key"], "sk-ant-api-1")

    def test_an_api_error_comes_back_as_it_was(self):
        def opener(req, timeout):
            raise urllib.error.HTTPError(agent.API, 429, "busy", {}, io.BytesIO(b'{"error":"slow down"}'))
        os.environ["ANTHROPIC_API_KEY"] = "k"
        self.addCleanup(os.environ.pop, "ANTHROPIC_API_KEY", None)
        self.assertEqual(agent.forward(b"{}", opener=opener), (429, b'{"error":"slow down"}'))


if __name__ == "__main__":
    unittest.main()
