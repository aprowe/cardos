# Wikipedia

_Moved from CLAUDE.md on 2026-10-27._

**Wikipedia** (2026-10-12). `apps/wiki/wiki.c` (Net) searches and reads it;
`apps/wiki/server/__init__.py` does the HTTPS and JSON (with a Wikimedia-style
User-Agent) and answers in lines: `/wiki/search?q=` a result a line
(`title<TAB>description`), `/wiki/page?title=&page=N` a header
`page<TAB>N<TAB>TOTAL<TAB>title` then plain ASCII, a paragraph a line,
`## `/`### ` headings, the See also / References sections gone, cut
between paragraphs into pages of at most 5 KB (the app's buffer is 6 KB);
`/wiki/random`, `/wiki/summary?q=` (searched, then the lead paragraph) and
`/wiki/featured`; `lang=xx` on all; ten minutes of cache. The app is a
search box (wants text, so voice types into it; Enter on nothing is a
random article), results, and a reader wrapped in ui13/ui13b that fetches
the next page when you scroll past the end and the one before (landing at
its end) when you scroll above the top; `r` random, `p`/fn-p prints the
page loaded. Commands `summary TOPIC`, `featured` (for Today) and `open
TOPIC`. Data 16.7 KB. Tests: `apps/wiki/test/test_wiki.c`, `apps/wiki/server/tests/test_wiki.py`.
