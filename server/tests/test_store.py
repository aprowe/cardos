"""server/store.py: the JSON state files.

    python -m server.tests.test_store
"""
import json
import os
import shutil
import sys
import tempfile
import threading
import unittest
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from server import store


class Store(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, True)
        self.path = os.path.join(self.dir, "users", "sam", "x.json")

    def test_missing_or_corrupt_is_the_default(self):
        self.assertIsNone(store.read_json(self.path))
        self.assertEqual(store.read_json(self.path, {}), {})
        os.makedirs(os.path.dirname(self.path))
        with open(self.path, "w") as f:
            f.write("{half")
        self.assertEqual(store.read_json(self.path, []), [])

    def test_written_whole_with_nothing_left_beside_it(self):
        store.write_json(self.path, {"a": [1, 2]}, indent=1)
        self.assertEqual(store.read_json(self.path), {"a": [1, 2]})
        self.assertEqual(os.listdir(os.path.dirname(self.path)), ["x.json"])
        with open(self.path) as f:
            self.assertEqual(f.read(), json.dumps({"a": [1, 2]}, indent=1))

    @unittest.skipIf(os.name == "nt", "POSIX modes")
    def test_owner_only(self):
        store.write_json(self.path, {})
        self.assertEqual(os.stat(self.path).st_mode & 0o777, 0o600)
        self.assertEqual(os.stat(os.path.dirname(self.path)).st_mode & 0o077, 0)

    def test_a_failed_write_keeps_the_old_file(self):
        store.write_json(self.path, {"v": 1})
        with self.assertRaises(TypeError):
            store.write_json(self.path, {"v": object()})
        self.assertEqual(store.read_json(self.path), {"v": 1})
        self.assertEqual(os.listdir(os.path.dirname(self.path)), ["x.json"])

    def test_update_changes_in_place_or_not_at_all(self):
        store.update_json(self.path, lambda d: d.update(a=1), {})
        store.update_json(self.path, lambda d: d.update(b=2), {})
        self.assertEqual(store.read_json(self.path), {"a": 1, "b": 2})
        self.assertEqual(store.update_json(self.path, lambda d: [3], {}), [3])   # a new one

        def refuse(d):
            d.append(4)
            raise ValueError("no")
        with self.assertRaises(ValueError):
            store.update_json(self.path, refuse, [])
        self.assertEqual(store.read_json(self.path), [3])

    def test_updates_from_many_threads_lose_nothing(self):
        def bump(d):
            d["n"] = d.get("n", 0) + 1

        def worker():
            for _ in range(25):
                store.update_json(self.path, bump, {})
        ts = [threading.Thread(target=worker) for _ in range(8)]
        for t in ts:
            t.start()
        for t in ts:
            t.join()
        self.assertEqual(store.read_json(self.path), {"n": 200})

    def test_one_lock_a_file(self):
        self.assertIs(store.lock_for(self.path),
                      store.lock_for(os.path.join(self.dir, "users", "sam", ".", "x.json")))
        self.assertIsNot(store.lock_for(self.path), store.lock_for(self.path + "2"))


if __name__ == "__main__":
    unittest.main()
