"""Speech to text, and English to one command.

Two halves of the same trip. The device records a WAV, posts it here, and gets
back either words to type or a command to run.

Recognition is whisper.cpp, invoked as a subprocess. There is no small version
of speech recognition either, which is the same reason page layout happens on
this machine: the Cardputer has 120 KB of heap.

The command half is a Claude session with a fixed system prompt whose entire
job is to turn a sentence into one line of a seven-verb vocabulary. It is
deliberately not the agent that edits the repository -- that one is thoughtful
and slow and has file access, and neither of those is wanted when someone says
"turn the brightness down". This one answers in under a second with one line,
and the device validates that line against the same seven verbs before doing
anything.
"""

import json
import os
import sys
import shutil
import subprocess
import tempfile

# The sibling project set whisper.cpp up, model and all. Reusing it beats
# downloading 141 MB again, and CARDOS_WHISPER overrides it.
DEFAULT_WHISPER_DIR = os.path.expanduser(
    r"~\Projects\cardputer\projects\cardlet\server\whisper")

SYSTEM = """\
You turn one spoken sentence into exactly one line of a command language, and
output nothing else -- no explanation, no punctuation around it, no code fence.

The language, in full:

  open NAME        launch an app. Names: notes, edit, web, todo, stocks, mines,
                   pinball, photos, claude, files, settings, memory, about
  shell WHICH      one of: launcher, desktop, console
  bright N         backlight, N from 0 to 100
  wifi on|off
  say TEXT         type TEXT into whatever is on screen
  key NAME         one of: escape, enter, up, down, left, right
  none REASON      the sentence is not one of the above; REASON is one short
                   phrase shown to the user

Rules:
- Exactly one line. If the sentence asks for two things, do the first.
- Relative requests become absolute: "turn it down" is bright 40, "dim" is
  bright 25, "brighter" is bright 100.
- "notes" means the edit app.
- Prefer `none` over a guess. A wrong command is worse than a shrug.
"""


class Voice:
    """Recognition and command translation, sharing a whisper install."""

    def __init__(self, whisper_dir=None, claude=None, model=None):
        self.dir = whisper_dir or os.environ.get("CARDOS_WHISPER",
                                                 DEFAULT_WHISPER_DIR)
        self.claude = claude
        self.model = model or "ggml-base.en.bin"

    # ---- speech to text ---------------------------------------------------

    def _cli(self):
        for name in ("whisper-cli.exe", "whisper-cli", "main.exe", "main"):
            p = os.path.join(self.dir, name)
            if os.path.exists(p):
                return p
        return shutil.which("whisper-cli")

    def ready(self):
        return bool(self._cli()) and os.path.exists(
            os.path.join(self.dir, self.model))

    def transcribe(self, wav_bytes):
        """A 16 kHz mono 16-bit WAV in, the words out."""
        cli = self._cli()
        if not cli:
            return "", "no whisper-cli in %s" % self.dir
        model = os.path.join(self.dir, self.model)
        if not os.path.exists(model):
            return "", "no model at %s" % model

        with tempfile.TemporaryDirectory() as tmp:
            wav = os.path.join(tmp, "in.wav")
            out = os.path.join(tmp, "out")
            with open(wav, "wb") as f:
                f.write(wav_bytes)

            cmd = [cli, "-m", model, "-f", wav, "-l", "en",
                   "-t", str(max(2, (os.cpu_count() or 4) - 1)),
                   "-nt",              # no timestamps: we want the words
                   "-otxt", "-of", out]
            r = subprocess.run(cmd, capture_output=True, text=True,
                               encoding="utf-8", errors="replace", timeout=120)
            txt = out + ".txt"
            if not os.path.exists(txt):
                return "", (r.stderr or "whisper produced nothing")[-300:]
            with open(txt, encoding="utf-8", errors="replace") as f:
                text = f.read().strip()

        # Whisper writes something for silence too -- "[BLANK_AUDIO]", or a
        # hallucinated "Thank you." Neither is speech, and typing either into
        # whatever has focus would be worse than saying nothing happened.
        if not text or text.startswith("["):
            return "", "nothing heard"
        return text, ""

    # ---- English to one command -------------------------------------------

    def command(self, text, chat):
        """Translate `text` into one RPC line, using the ChatService's CLI.

        A separate, stateless session: no --resume, no repository context, and
        a system prompt that admits one line of output. The agent that edits
        the tree is the wrong tool for "open notes" -- it would think about it.
        """
        cli = chat.claude if chat else None
        if not cli:
            return "none no claude on this machine"

        cmd = [cli, "-p", text,
               "--append-system-prompt", SYSTEM,
               "--output-format", "json",
               # Nothing to read, nothing to write: this turn is a translation.
               "--allowed-tools", "",
               "--permission-mode", "dontAsk"]
        if self.model and chat.model:
            cmd += ["--model", chat.model]

        try:
            r = subprocess.run(cmd, cwd=chat.cwd, capture_output=True,
                               text=True, encoding="utf-8", errors="replace",
                               timeout=60, env=chat._child_env())
        except subprocess.TimeoutExpired:
            return "none the translator timed out"

        body = (r.stdout or "").strip()
        try:
            obj = json.loads(body)
            line = obj.get("result") or ""
        except ValueError:
            line = body

        # One line, whatever it said. A model that explains itself gets its
        # explanation dropped rather than sent to a device that would refuse
        # the whole thing.
        line = line.strip().splitlines()[0].strip() if line.strip() else ""
        line = line.strip("`").strip()
        return line or "none nothing came back"


# ---- route -----------------------------------------------------------------

WAKE_WORDS = ("carlos", "karlos", "carlus", "carlo")


def post_voice(h, path, args):
    """a WAV in; the words, or one command line, out

    Both halves answer on this one request rather than through the job queue
    the chat uses. Recognition of a ten-second clip takes about two seconds
    and a command translation about one, which is inside what the device will
    wait for -- and unlike a chat turn, there is nothing useful to show while
    it happens."""
    wav = h.body(4 << 20)           # 16 kHz mono: two minutes is 3.8 MB
    if len(wav) <= 44:              # a header and no audio
        h.text("error nothing recorded\n", 400)
        return

    text, err = h.voice.transcribe(wav)
    if err:
        sys.stderr.write("voice: %s\n" % err)
        h.text("error %s\n" % err)
        return
    sys.stderr.write("voice: heard %r\n" % text[:80])

    # The wake word is checked here as well as on the device: the device
    # decides what to do, but the translation only happens if it is asked
    # for, and asking costs a model call.
    low = text.lstrip().lower()
    wake = None
    for name in WAKE_WORDS:
        if low.startswith(name):
            wake = text.lstrip()[len(name):].lstrip(" ,.:!?")
            break

    if wake is None:
        h.text("text %s\n" % text)
        return
    if not wake:
        h.text("error I heard my name and nothing after it\n")
        return

    line = h.voice.command(wake, h.chat)
    sys.stderr.write("voice: %r -> %s\n" % (wake[:60], line))
    h.text("cmd %s\n" % line)


ROUTES = [("POST", "/voice", post_voice)]
