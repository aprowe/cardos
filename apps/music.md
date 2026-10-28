# Music

_Moved from CLAUDE.md on 2026-10-27._

**Music: tracks dragged onto the dashboard, played on the device**
(2026-10-06; MP3 since 2026-10-12, below). The device played PCM WAV only, so the dashboard's Music page
converts in the browser -- decodeAudioData, an OfflineAudioContext at
22050 Hz mono, lifted to near full scale -- and uploads a 16-bit WAV
(about 2.6 MB a minute); `server/music.py` checks it is one and keeps it.
`apps/music.c` (Make) syncs on open: the list, then each missing track
streamed to /music/ID.part with a bar, renamed when whole; a key stops it.
Enter plays, space pauses and goes on, [ ] (or shift+left/right) seek ten
seconds, left/right step, esc stops, s shuffles, + - volume; the next track
starts when one ends. Pause holds the place by sending silence between
blocks (the channel stays up, so no click) and seek moves the read inside
the data -- `audio_pause`/`audio_seek_ms`, in CappAudio as pause/paused/
seek_ms. Leaving the app lets the current track finish. `api->http_stream`
now carries the device's token to its own server.

The server keeps each
upload as a 44.1 kHz mono 96 kbps MP3 (0.7 MB a minute; the WAV was 2.6),
title and artist from its tags, and `/music` lists `fmt` and `artist` as
fields 5 and 6; `python -m server.music mp3` makes the old WAVs MP3 under
the same ids, and the device swaps them at its next sync. The app
(`apps/music.c`) is the nano: a grey title bar, white lists with a blue
bar and chevrons, Songs, Artists, Shuffle Songs, Now Playing ("3 of 12",
a cover drawn from the name, a bar with time gone and left; up/down are
the volume there, as the wheel was), in Inter (`pod13`, `pod13b`,
`pod16b`). MP3s put in /music by hand are named by their ID3 tags
(`apps/id3.h`).

Music lists any WAV put in /music by
hand (`M.local`: never removed by a sync, deleted from the card only);
named by a server track's id it counts as that track.

The server converts any uploaded audio with ffmpeg
(`server/music.py`); the dashboard sends files as they are.
