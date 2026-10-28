# Chat

_Moved from CLAUDE.md on 2026-10-27._

**Chat has dog emojis: Arty and Marcy** (2026-10-13). Tab in Chat opens a
picker of eight moods (happy, love, sad, sleepy, hungry, grumpy, zoomies,
huh) for one dog -- Tab again is the other, kept as the second line of
/config/chat.txt; arrows choose (no fn: Chat stops taking text while the
picker is up), Enter puts it into the text box, and Enter there sends. An
emoji is only text on the wire, `:arty-happy:` among the words, so the
server and older Chats need nothing; Chat draws each as a picture four log
rows tall among the message's wrapped words (each row a log line marked
`\x01`, painted by the row hook `tl_paint_log_ex` in `apps/termlog.h`), and
the notify poll turns them into words for the banner ("Arty: happy", or
"(Arty: happy)" among words; `msg.banner_text`). The art is pixel art in
`apps/chat/tools/make_emoji.py` -- each dog an 18x18 grid of letters drawn by hand
(Arty: the small white terrier, black head with tan; Marcy: the black one,
white blaze), each mood stamps of eyes, mouth and a mark beside the head,
and a grey outline so a black dog shows on Chat's black. `python
apps/chat/tools/make_emoji.py` writes `apps/chat/chat_emoji.h` (palette indices, the
palette in the panel's byte order as `CAPP_RGB` makes it; 7 KB of constants,
in flash); `--preview FILE.html` draws them all big and at device size.
Chat moved to `apps/chat/` for it. Tests: `apps/chat/test/test_chat_emoji.c`, and
`apps/chat/test/test_chat_paint.c` runs the real app into a framebuffer
(`CHAT_DUMP=dir` writes the screens).
