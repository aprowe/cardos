#!/usr/bin/env python3
"""Load a folder of downloaded New York Times crosswords into the CardOS
server's library, from a checkout.

    python tools/crossword_import.py FOLDER_OR_ZIP [...] [--dry-run] [--replace]
                                     [--state DIR] [--date YYYY-MM-DD] [-v | -q]

This is `python -m server.crossword import`, the same code with the same
options; it is here so it can be run from the repository without thinking
about the module path. All of it lives in server/crossword.py: read that
file's docstring for what is accepted (.puz files and the two kinds of
JSON, in folders read all the way down or in .zip archives), where the
dates come from, what is skipped and what is reported.

The puzzles themselves have to be fetched by hand and put in a folder on
the machine the server runs on -- nothing here downloads them. The library
is CARDOS_STATE/crosswords (--state DIR, or the environment's CARDOS_STATE,
or ~/.cardos): use the server's own, or the puzzles go to a library the
server does not read. Try it with --dry-run first.

Exit status: 0 when every file was read (or skipped as already there), 1 if
any could not be, 2 if it could not run (no such folder, nothing in it).
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from server import crossword  # noqa: E402


if __name__ == "__main__":
    sys.exit(crossword.import_main(sys.argv[1:], prog="python tools/crossword_import.py"))
