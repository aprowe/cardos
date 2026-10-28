# Claude

_Moved from CLAUDE.md on 2026-10-27._

`claude [-k song]
PATH` opens the Claude app about one file instead of the device agent:
`server/talk.py` takes the document, Claude answers there with no tools,
and a revised document (a ```doc fence in the answer) waits on the server
until ctrl-s saves it over the file; ctrl-z puts it back.
