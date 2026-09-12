# Capabilities, text input, and voice

Four features that turn out to be one feature, which is why they are specified
together:

1. an app says what it needs — nothing, the network, or the proxy;
2. the OS knows when an app is taking text, whoever is typing it;
3. the button on top records, anywhere, and the words become text;
4. if the words start with "Carlos" they are a command, not text.

The thread joining them: **text is text and the app should not care where it
came from.** A key from the built-in matrix, a key from a Bluetooth keyboard,
and a word said out loud all end up in the same place by the same path. Every
app that already handles typing gets voice for free, and no app contains the
word "voice" anywhere in it.

## 1. Capabilities

`CappInfo.flags` gains two bits:

    CAPP_NEEDS_NET     the internet, via WiFi
    CAPP_NEEDS_PROXY   tools/webproxy.py on a PC (implies NEEDS_NET)

Nothing declared means standalone: Mines, Pinball and Photos run on a device
that has never seen a network, and should keep working when the laptop is off.

What the OS does with them, at launch, before `capp_main` runs:

- **NEEDS_NET**: bring WiFi up if it is not up. This already happens ad hoc
  inside `http_get`; moving it to launch means the app is not left rendering a
  half-state while a radio negotiates, and the twenty seconds of joining are
  attributed to starting the app rather than to the first thing it does.
- **NEEDS_PROXY**: as above, then one short request to `/status`. If nothing
  answers, the app is told at startup rather than discovering it mid-draw.

Refusal is *not* part of this. An app that needs the proxy still opens with the
proxy down — Web can show its cached page, Claude can show its log — it simply
knows. `api->caps_ok()` returns what was found, so an app can say "the server
is not answering" in its own words instead of failing at the first request.

The launcher shows a small glyph on an icon that needs something, because
"why doesn't this work away from the desk" is a question worth answering before
it is asked rather than after.

## 2. Text input mode

The OS already knows whether an app is taking text: `wants_text()`, which
exists so `; . , /` can be arrows when nothing is being typed. That flag is
promoted from a keyboard detail to *the* input-mode signal, and gains one
companion:

    void input_text(const char *s);     /* kernel-side, not an app call */

`input_text` delivers a string to whatever has focus **as keystrokes**, one
character at a time, through the identical path a keypress takes. Not a new
callback, not a buffer an app must poll: the app's `key()` handler runs, its
editor inserts characters, its scroll follows the caret, exactly as if the
words had been typed quickly.

This is the whole reason voice needs no per-app work. It also means an app that
handles a fast typist correctly handles voice correctly, and one that does not
has a bug worth fixing anyway.

Where the text goes when nothing is taking text (`wants_text()` false — the
launcher, a game, the desktop) is the interesting case, and the answer is: it
does not. A transcript arriving with no text field is treated as a command
attempt and reported if it is not one. Dictating into Minesweeper should say so,
not silently type into nothing.

## 3. Voice

**Capture.** `kernel/drv/mic.c`, ESP-IDF I²S in PDM RX mode, 16 kHz mono 16-bit
— what whisper wants, so nothing resamples. DAT 46, CLK 43. G43 is shared with
the speaker's LRCLK, so recording and playback stay mutually exclusive; the
driver says so rather than failing quietly.

Audio goes straight to `/cache/voice.wav` as it is captured. Twenty seconds at
16 kHz mono is 640 KB, which does not fit in a heap with 120 KB free, and the
card is right there.

**The button.** G0, the button on the top edge. Press to start, release to stop
— push-to-talk, because a device with no wake-word engine cannot know when you
have finished, and a fixed five-second window is either too short or a wait.

It is polled in the main loop next to the keyboard, so it works in every shell
and over every app, including one that has the screen. G0 is also the BOOT
button: holding it *at power-on* enters the ROM downloader, which is why safe
mode is the escape key and not this one.

**Recognition.** `POST /voice` to the proxy with `audio/wav`, which shells out
to whisper.cpp — already installed for the sibling project at
`projects/cardlet/server/whisper/`, `whisper-cli.exe` with `ggml-base.en.bin`.
The reply is the transcript as plain text. Recognition on the PC for the same
reason layout is: there is no small version of it.

## 4. "Carlos"

If the transcript begins with the wake word, it is not text. The rest of it goes
to `POST /command`, where a Claude session with a fixed system prompt turns
English into one line of a tiny RPC vocabulary, which the device executes:

    open NAME            launch an app by name
    shell launcher|desktop|console
    bright N             0..100
    wifi on|off
    say TEXT             put TEXT into the focused app, as if typed
    key NAME             escape, enter, up, down, left, right
    none REASON          nothing matched; REASON is shown

Deliberately tiny, and validated on the device against that list. The device
executes the verb, not the sentence: an LLM deciding what "turn it down" means
is useful, an LLM handing a device an arbitrary command to run is not, and the
gap between those two is this table.

`say` exists so "Carlos, write good morning" works — the wake word chooses
between typing and commanding, and one of the commands is to type.

## Order of work

1. capability flags, and the launch-time bring-up — no new hardware
2. `input_text`, wired to the existing key path — testable on the host
3. the WAV writer and the RPC parser — pure logic, host tests first
4. the mic driver and the G0 button — device only, needs eyes and ears
5. `/voice` and `/command` on the proxy
6. the wake word, joining 3 and 5

Each step leaves the device working. 1 and 2 are useful with no microphone at
all: an app that knows it needs the proxy is better than one that discovers it.
