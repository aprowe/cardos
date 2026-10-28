# Lamp

_Moved from CLAUDE.md on 2026-10-27._

`apps/lamp/` (Make) is a remote for LED lamps and strips:
models are text (`apps/lamp/lampmodel.h`, host-tested) -- the 24-key
(NEC 0xEF00) and 44-key (NEC 0x00) RGB remotes built in, more in
`/config/lamps.txt` -- the choice kept in `/config/lamp.txt`, and `f`
tries each model in turn and asks whether the lamp reacted. The owner's
lamp answered the 24-key address with its buttons elsewhere (colours that
were modes), so `c` (or Enter after a find) maps a lamp: each code 00-FF
on the model's address is sent and the person picks what it did from 21
switches and colours (`LM_OPTS`); every pick rewrites a "My lamp" model in
lamps.txt (`lm_map_text`, `lm_replace`, host-tested). Checked on the
device (2026-10-26).
