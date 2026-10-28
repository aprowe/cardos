# Traitors: The Game

Seven contestants at a castle, two of them Traitors -- alone with six
computer players, or two people on two Cardputers (ESP-NOW, no router; the
inviter hosts). Each round: a mission (the leader's team voted on, then
played straight or sabotaged in secret; a clean one adds to the prize pot
and wins a shield), the round table, a murder in the night. Faithful win
when no Traitor is left; Traitors at two sabotaged missions or parity.

- `traitors.h` -- the rules, the computer players (suspicion per pair, a
  Traitor playing to the herd) and `TrView`, what one seat may see, packed
  into one link message. `test/test_traitors.c`: 400 computer games finish,
  each side wins over a fifth; every rule pinned.
- `traitors.c` -- the hall, the portraits and the chat room. The round
  table is a conversation: openings, then one to three computers answer
  whatever a person says, and Claude's `lean` lines move their suspicion,
  so talk changes votes. `test/test_traitors_paint.c` plays whole games.
- `server/` -- the plugin: `POST/GET /traitors/talk` (modes `open` and
  `reply`). Roles go to the server only. Offline the app has its own lines.
- `tools/make_traitors.py` -- the contestants (name, age, job, portrait)
  and the icons, into `traitors_art.h`; `--preview F.png`.

The rules are an original design in the show's manner. Not checked yet on
two real devices: the link, a guest leaving mid-game.
