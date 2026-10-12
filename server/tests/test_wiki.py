"""Wikipedia through the server, against a Wikipedia that is a dict.

What the device relies on: one result a line, an article as a header line
and plain ASCII text with `## ` headings, pages that fit its buffer and
break between paragraphs, the boilerplate sections gone, errors said in
words -- and the same question not asked of Wikipedia twice in a row.

    python -m server.tests.test_wiki
"""
import os
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.parse
import urllib.request
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from http.server import ThreadingHTTPServer
from server import app, wiki
from server import chat as chatmod

TOKEN = "tok"

MOON = ("The Moon is Earth's only natural satellite — it orbits at about "
        "384,400 km.\n"
        "It is the fifth largest moon in the Solar System.\n"
        "\n\n== Name and etymology ==\n"
        "The usual English proper name is “Moon”.\n"
        "\n\n== Physical characteristics ==\n"
        "\n\n=== Size and mass ===\n"
        "Its radius is 1,737 km, with {\\displaystyle {\\frac {1}{4}}} the size of Earth.\n"
        "\n\n=== Empty subsection ===\n"
        "\n\n== See also ==\n"
        "Lunar outline\n"
        "\n\n== References ==\n"
        "Smith, J. (2001).\n"
        "\n\n== Further reading ==\n"
        "A book.\n")


class FakeWiki:
    """Answers the URLs server/wiki.py asks, from a few articles."""

    def __init__(self):
        self.calls = []
        self.articles = {"Moon": MOON}
        self.status = None

    def __call__(self, url):
        self.calls.append(url)
        if self.status:
            return self.status, None
        u = urllib.parse.urlparse(url)
        q = urllib.parse.parse_qs(u.query)
        if u.path == "/w/rest.php/v1/search/page":
            if q["q"][0] == "nothing":
                return 200, {"pages": []}
            return 200, {"pages": [
                {"title": "Moon", "description": "Natural satellite of Earth",
                 "excerpt": "The <span class=\"searchmatch\">Moon</span> is"},
                {"title": "Moon\tlanding", "description": None,
                 "excerpt": "A <span class=\"searchmatch\">Moon</span> landing\nis the arrival"},
            ]}
        if u.path == "/w/api.php":
            title = q["titles"][0]
            if title in self.articles:
                return 200, {"query": {"pages": [{"title": title, "extract": self.articles[title]}]}}
            return 200, {"query": {"pages": [{"title": title, "missing": True}]}}
        if u.path == "/api/rest_v1/page/random/summary":
            return 200, {"title": "Moon", "description": "Natural satellite", "extract": "x"}
        if u.path.startswith("/api/rest_v1/page/summary/"):
            title = urllib.parse.unquote(u.path.rsplit("/", 1)[1])
            if title != "Moon":
                return 404, None
            return 200, {"title": "Moon", "description": "Natural satellite of Earth",
                         "extract": "The Moon is Earth's only natural satellite.\nIt is big."}
        if u.path.startswith("/api/rest_v1/feed/featured/"):
            return 200, {"tfa": {"title": "Moon", "titles": {"normalized": "Moon"},
                                 "description": "Natural satellite",
                                 "extract": "The Moon – featured."}}
        return 404, None


class WikiText(unittest.TestCase):
    def test_headings_are_marked_and_the_boilerplate_is_gone(self):
        blocks = wiki.blocks(MOON)
        self.assertEqual(blocks, [
            "The Moon is Earth's only natural satellite - it orbits at about 384,400 km.",
            "It is the fifth largest moon in the Solar System.",
            "## Name and etymology",
            "The usual English proper name is \"Moon\".",
            "## Physical characteristics",
            "### Size and mass",
            "Its radius is 1,737 km, with the size of Earth.",
        ])

    def test_text_is_ascii_the_font_can_draw(self):
        s = wiki.ascii_text("Café — “naïve” … ½ 中 5 km − 3°")
        self.assertTrue(all(32 <= ord(c) < 127 for c in s), s)
        self.assertEqual(s, "Cafe - \"naive\" ... 1/2 5 km - 3 deg")
        # Brackets that held only what the font cannot draw go too.
        self.assertEqual(wiki.ascii_text("Zhoubi Suanjing (周高), (The Classic) and (周; x)"),
                         "Zhoubi Suanjing, (The Classic) and (x)")

    def test_pages_fit_and_break_between_paragraphs(self):
        paras = ["Paragraph %d " % i + "word " * 150 for i in range(20)]
        pages = wiki.paginate(paras, 2000)
        self.assertGreater(len(pages), 5)
        self.assertEqual("\n".join(pages), "\n".join(p.strip() for p in paras))
        for p in pages:
            self.assertLessEqual(len(p), 2000)
            self.assertTrue(p.startswith("Paragraph"), p[:20])

    def test_a_paragraph_longer_than_a_page_is_cut_between_words(self):
        pages = wiki.paginate(["abcde " * 1000], 1000)
        self.assertGreater(len(pages), 5)
        for p in pages:
            self.assertLessEqual(len(p), 1000)
            self.assertTrue(p.startswith("abcde") and p.endswith("abcde"), p[-10:])

    def test_a_heading_does_not_end_a_page(self):
        paras = ["a" * 900, "## Heading", "b" * 500]
        pages = wiki.paginate(paras, 1000)
        self.assertEqual(pages, ["a" * 900, "## Heading\n" + "b" * 500])


class WikiRoutes(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        os.environ["CARDOS_STATE"] = self.dir
        wiki._cache.clear()
        self.w = FakeWiki()
        wiki.http_json = self.w
        app.Handler.chat = chatmod.ChatService(claude="stub", token=TOKEN)
        app.Handler.store = None
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), app.Handler)
        self.base = "http://127.0.0.1:%d" % self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()

    def req(self, path):
        r = urllib.request.Request(self.base + path, headers={"Authorization": "Bearer " + TOKEN})
        try:
            with urllib.request.urlopen(r, timeout=10) as resp:
                return resp.status, resp.read().decode()
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode()

    def test_search_is_a_result_a_line(self):
        s, text = self.req("/wiki/search?q=moon")
        self.assertEqual(s, 200)
        self.assertEqual(text, "Moon\tNatural satellite of Earth\n"
                               "Moon landing\tA Moon landing is the arrival\n")
        self.assertIn("limit=10", self.w.calls[0])

    def test_search_with_nothing_found_is_empty(self):
        self.assertEqual(self.req("/wiki/search?q=nothing"), (200, ""))
        self.assertEqual(self.req("/wiki/search?q=")[0], 400)

    def test_an_article_is_a_header_then_text(self):
        s, text = self.req("/wiki/page?title=Moon")
        self.assertEqual(s, 200)
        lines = text.splitlines()
        self.assertEqual(lines[0], "page\t1\t1\tMoon")
        self.assertEqual(lines[3], "## Name and etymology")
        self.assertNotIn("References", text)

    def test_a_long_article_comes_a_page_at_a_time(self):
        self.w.articles["Long"] = "\n".join("Paragraph %d " % i + "word " * 200 for i in range(40))
        s, first = self.req("/wiki/page?title=Long")
        total = int(first.split("\n")[0].split("\t")[2])
        self.assertGreater(total, 3)
        self.assertLessEqual(len(first), wiki.PAGE + 100)
        s, last = self.req("/wiki/page?title=Long&page=%d" % total)
        self.assertEqual(last.split("\n")[0], "page\t%d\t%d\tLong" % (total, total))
        self.assertIn("Paragraph 39", last)
        self.assertEqual(self.req("/wiki/page?title=Long&page=%d" % (total + 1))[0], 404)
        # One fetch of the article for all of it.
        self.assertEqual(len([c for c in self.w.calls if "api.php" in c]), 1)

    def test_no_such_article_is_said(self):
        s, text = self.req("/wiki/page?title=Nope")
        self.assertEqual((s, text), (404, "error no article called Nope\n"))

    def test_random_is_the_first_page_of_some_article(self):
        s, text = self.req("/wiki/random")
        self.assertEqual(s, 200)
        self.assertTrue(text.startswith("page\t1\t1\tMoon\n"), text)
        # Random is never cached: the second is a second roll.
        self.req("/wiki/random")
        self.assertEqual(len([c for c in self.w.calls if "random" in c]), 2)

    def test_summary_finds_the_topic_and_gives_its_first_paragraph(self):
        s, text = self.req("/wiki/summary?q=the%20moon")
        self.assertEqual((s, text), (200, "Moon\tNatural satellite of Earth\n"
                                          "The Moon is Earth's only natural satellite. It is big.\n"))
        self.assertEqual(self.req("/wiki/summary?q=nothing")[0], 404)

    def test_featured_is_todays(self):
        s, text = self.req("/wiki/featured")
        self.assertEqual((s, text), (200, "Moon\tNatural satellite\nThe Moon - featured.\n"))

    def test_wikipedia_down_is_said_in_words(self):
        self.w.status = 503
        s, text = self.req("/wiki/search?q=moon")
        self.assertEqual(s, 502)
        self.assertTrue(text.startswith("error Wikipedia said 503"), text)

    def test_the_same_search_is_asked_once(self):
        self.req("/wiki/search?q=moon")
        self.req("/wiki/search?q=moon")
        self.assertEqual(len(self.w.calls), 1)

    def test_a_language_other_than_english(self):
        self.req("/wiki/search?q=lune&lang=fr")
        self.assertTrue(self.w.calls[0].startswith("https://fr.wikipedia.org/"))
        self.assertEqual(self.req("/wiki/search?q=x&lang=../evil")[0], 400)


if __name__ == "__main__":
    unittest.main()
