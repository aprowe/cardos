"""server/plugins.py and tools/appmeta.py: an app's server code is found
beside the app -- in this repository's apps/, and in another repository's
apps folder named by CARDOS_APP_DIRS.

    python -m server.tests.test_plugins
"""
import os
import shutil
import sys
import tempfile
import unittest
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from server import plugins
from tools import appmeta

APP_C = """/* Hello: an app kept in a repository of its own.
 *
 * @app folder Games
 * @app release extra
 * @app routes /hi
 */
"""

PLUGIN = '''from . import words

def get_hello(h, path, args):
    h.text(words.HELLO)

ROUTES = [("GET", "/hello/greet", get_hello, "open"),
          ("GET", "/hi/there", get_hello, "open")]
'''


class Plugins(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, True)
        app = os.path.join(self.dir, "hello")
        os.makedirs(os.path.join(app, "server"))
        self.put("hello/hello.c", APP_C)
        self.put("hello/server/__init__.py", PLUGIN)
        self.put("hello/server/words.py", 'HELLO = "hello from elsewhere\\n"\n')
        self.old_env = os.environ.get("CARDOS_APP_DIRS")
        os.environ["CARDOS_APP_DIRS"] = self.dir
        plugins._loaded = None
        self.addCleanup(self.restore)

    def restore(self):
        if self.old_env is None:
            os.environ.pop("CARDOS_APP_DIRS", None)
        else:
            os.environ["CARDOS_APP_DIRS"] = self.old_env
        plugins._loaded = None
        for m in [m for m in sys.modules if m.startswith("cardos_apps")]:
            del sys.modules[m]

    def put(self, rel, text):
        with open(os.path.join(self.dir, rel), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)

    def test_this_repositorys_plugins_load(self):
        mods = plugins.load(dirs=[appmeta.APPS], strict=True)
        self.assertTrue({"chat", "jar", "crossword"} <= set(mods))

    def test_another_repositorys_app_is_found_everywhere(self):
        self.assertIn(self.dir, appmeta.app_dirs())
        a = appmeta.apps()["hello"]
        self.assertEqual(a.folder, "Games")
        self.assertEqual(appmeta.folders()["hello"], "Games")
        mods = plugins.load(strict=True)
        self.assertIn("hello", mods)
        self.assertEqual(mods["hello"].__name__, "cardos_apps.hello.server")
        paths = {r[1]: r[2] for r in plugins.routes()}
        self.assertIn("/hello/greet", paths)        # its own name
        self.assertIn("/hi/there", paths)           # and what it declared

        class H:
            def text(self, s):
                self.said = s
        h = H()
        paths["/hello/greet"](h, "/hello/greet", {})
        self.assertEqual(h.said, "hello from elsewhere\n")   # its relative import works

    def test_a_path_it_did_not_declare_is_refused(self):
        self.put("hello/server/__init__.py",
                 'ROUTES = [("GET", "/jar/day", lambda h, p, a: None, "open")]\n')
        with self.assertRaises(ValueError):
            plugins.load(strict=True)
        plugins._loaded = None
        self.assertNotIn("hello", plugins.load())        # the server leaves it out

    def test_two_apps_of_one_name_are_refused(self):
        os.makedirs(os.path.join(self.dir, "wordle"))
        self.put("wordle/wordle.c", APP_C)
        with self.assertRaises(SystemExit):
            appmeta.apps()


if __name__ == "__main__":
    unittest.main()
