# Vim

_Moved from CLAUDE.md on 2026-10-27._

**Vim: a modal code editor** (2026-10-16, `apps/vim/vim.c`, Make). The
owner asked for "an IDE with vim motions, basic autocomplete and syntax
highlighting". Normal, insert, visual (v, V) and : modes; [count]
operator [count] motion with d c y > < g~ gu gU over h j k l w b e W B E
0 ^ $ gg G f t F T ; , % H M L { } n N * #; x X r s S D C Y p P J ~ o O
a A i I; u and ctrl-r by change; the dot (`3.` too); / and ? (literal,
not regex; `:set ic`); ctrl-d/u/e/y/f/b, zz zt zb; :w :q :wq :x :e :N
:set nu sw= ts= et ic, :s and :%s (flags g, i), :d, ranges `%`, `N,M`,
`'<,'>`. Colours for C, Python and Markdown by extension. Completion in
insert mode: ctrl-n/ctrl-p, or tab after a word character -- words in the
buffer, nearest first, then the language's keywords, in a list over the
text. One unnamed register; no marks, macros, regex or windows. The core is
portable headers (`apps/vim/vimbuf.h` the gap buffer, `vimhl.h` the
highlighter, `vimcore.h` everything else), driven a key at a time by
`apps/vim/test/test_vim.c`; `apps/vim/test/test_vim_paint.c` runs the real app into a
framebuffer with the real font and checks that after every key a paint of
only the damage equals a full paint (`VIM_DUMP=dir` writes the screens).
- **Memory**: a 20 KB pool in the arena (14 KB text, 4 KB undo, 2 KB
  register on its own); Vim is `CAPP_BIG`, and at launch the text takes one
  big-mode block of big_room less 4 KB, up to 96 KB, the pool becoming
  12 KB of undo and 8 KB of register. A file too big is refused, never
  opened in part; a change too big for the undo log empties it and says
  so. Measured by build_apps: code 32.5 KB, data 24.5 KB (of 28), rodata
  4.0 KB (with the tree, below).
- **Syntax states**: a line's colours depend only on the state the line
  above left (inside a block comment, a triple-quoted string, a fence),
  kept at a checkpoint every 32 lines and invalidated from the edited line
  down, so a paint never scans the file.
- **The file tree** (2026-10-16, `apps/vim/vimtree.h`, host-tested). The
  owner: "view a file tree, open a file, view it, then close back to the
  file tree at the same spot". Vim with no file (or a folder) is a tree of
  the card rooted at /: folders first, A-Z, dotfiles hidden (`.` or `gh`),
  a coloured mark per kind (.py, .c/.h, .md, other). j k gg G ctrl-d
  ctrl-u H M L move; Enter/o open a file or fold a folder, l unfolds (then
  steps in), h folds (or goes up), `-` goes up a folder and folds it, `/`
  finds a name as it is typed (n N), R reads the card again, `:e NAME`
  makes a file beside the selection, q or :q leaves Vim (api->leave).
  nvim-tree's keys (2026-10-16): a, r and d fill in the command line --
  `add ` (NAME/ makes a folder), `rename OLDNAME`, `delete NAME` -- and
  Enter does it, Esc does not (that is the confirm); `:mkdir NAME` (also in
  a file, beside it) makes a folder; a folder must be empty to delete. In a
  file, `-` or `:Ex` shows the tree on that file (every folder on the way
  opened, it selected; E37 when it has changes), and :q / :wq / :x go back
  to the tree with the same folders open, the same row selected and the
  same scroll -- or, for a file Vim was opened with (from Files), out of
  Vim, back to the opener. Only the open folders are read, each at most 256
  entries (a "..." row says it was cut). What is kept is small and in the
  arena (the open folders' paths in 448 bytes, the selection's path, the
  scroll); the rows are rebuilt from the card whenever the tree is shown,
  into the text buffer's memory, which is empty while the tree is up -- so
  the tree costs the editor no room (data went 23.5 -> 24.5 KB). The state
  is also `/cache/vimtree`, so the tree is as it was from launch to launch.
- **Running Python**: `:run` (`:py`), `space r` in a file or on a .py in
  the tree, writes the file and opens Python on it (`api->run("python",
  PATH)`); a file that is not .py is said, not run. Python's exit() comes
  back through the way back (API 48): `CappUi.save` writes the mode (tree,
  file-from-tree, file), the file, line, column and top line, and the
  tree's state as a few versioned lines (`vim1 ... .`, at most ~700 bytes
  of CAPP_SAVE_MAX), writing an unsaved file first; capp_main takes
  `api->resumed` and reloads the file with the cursor clamped into it. A
  blob cut short or not ours is refused whole and Vim starts fresh.
  `space` followed by anything but r is still `l`; `-` with a count or
  after an operator is still the motion.
- Esc is vim's; wants_text is always yes, so ; , . / are commands and fn
  makes them arrows. `:e` alone still opens the OS picker.
- Not tried on a device yet: the big_room a real heap gives, how a full
  repaint feels on the panel, how long reading a few open folders takes on
  the card, and the trip to Python and back (the launcher side of API 48).
